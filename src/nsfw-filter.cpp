// SPDX-License-Identifier: GPL-2.0-or-later
//
// Filtre vidéo OBS "NSFW Guard" : analyse chaque image de la source avec un
// petit classifieur ONNX (thread dédié) et masque la source quand du contenu
// sensible est détecté. Un tampon de quelques images retarde l'affichage pour
// que le masque arrive avant l'image détectée.

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "classifier.h"
#include "nsfw-filter.h"

#define FILTER_ID "nsfw_guard_filter"
#define LOG_PREFIX "[nsfw-guard] "

// --- clés de réglage -------------------------------------------------------
#define S_THRESHOLD "threshold"       // % : score à partir duquel une image est jugée sensible
#define S_SEXY_WEIGHT "sexy_weight"   // % : poids de la classe "sexy" dans le score
#define S_CONSECUTIVE "consecutive"   // nb d'analyses positives consécutives avant de masquer
#define S_INTERVAL "interval"         // analyser une image sur N
#define S_HOLD_MS "hold_ms"           // maintien du masque après la dernière détection
#define S_MODE "mode"                 // 0 = noir, 1 = pixelisé, 2 = flou gaussien
#define S_PIXEL_SIZE "pixel_size"     // taille d'un bloc de pixelisation, en pixels de la source
#define S_BLUR_STRENGTH "blur_strength" // écart-type du flou gaussien, en pixels de la source
#define S_METHOD "method"             // méthode d'analyse, voir enum Method
#define S_METHOD_WARNING "method_warning"
#define S_ZONE_SIZE "zone_size"       // côté (px de la source) de la zone aléatoire

// Méthode d'analyse. Valeurs stockées dans les réglages : ne pas renuméroter.
enum Method {
	METHOD_WHOLE = 0,   // l'image entière, réduite en 224x224
	METHOD_GRID_2 = 1,  // l'image entière + une grille 2x2 de zones
	METHOD_GRID_3 = 2,  // ... 3x3
	METHOD_GRID_4 = 3,  // ... 4x4
	METHOD_ZONE = 4,    // une zone aléatoire (on y reste tant qu'elle est positive)
	METHOD_MIXED = 5,   // l'image entière + une zone aléatoire
};

// Une "vue" est une image 224x224 envoyée au modèle, découpée dans l'image de la
// source : l'image entière, une zone d'une grille, ou la zone aléatoire.
constexpr int kMaxViews = 17;                 // 1 + 4x4
constexpr size_t kPlaneBytes = static_cast<size_t>(kInputSize) * kInputSize * 4;
constexpr int kMaxPyramid = 6;                // niveaux de réduction par 2 (4K -> 3840/2^5 = 120)
#define S_DELAY_ON "delay_on"         // retarder l'image pour que le masque arrive à temps
#define S_AUTO_DELAY "auto_delay"     // calculer le délai automatiquement
#define S_DELAY_FRAMES "delay_frames" // délai manuel, en images
#define S_USE_DML "use_dml"           // GPU (DirectML) au lieu du CPU
#define S_BLOCK_UNTIL_READY "block_until_ready"
#define S_DEBUG "debug"

enum MaskMode { MASK_BLACK = 0, MASK_PIXELATE = 1, MASK_BLUR = 2 };

// Flou gaussien séparable : une passe par direction. `texel` vaut (1/largeur, 0)
// pour la passe horizontale et (0, 1/hauteur) pour la verticale. 12 échantillons
// de chaque côté ; au-delà de 4 px d'écart-type l'espacement des échantillons
// grandit pour continuer à couvrir 3 sigma (l'image est de toute façon réduite
// avant, voir render_blur_passes).
static const char *kBlurEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float2 texel;
uniform float sigma;

sampler_state blur_sampler {
	Filter   = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData mainTransform(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = v_in.uv;
	return vert_out;
}

float4 mainBlur(VertData v_in) : TARGET
{
	float s      = max(sigma, 0.5);
	float stride = max(1.0, s / 4.0);
	float4 sum   = image.Sample(blur_sampler, v_in.uv);
	float total  = 1.0;
	for (int i = 1; i <= 12; i++) {
		float d   = float(i) * stride;
		float wgt = exp(-0.5 * d * d / (s * s));
		float2 o  = texel * d;
		sum   += image.Sample(blur_sampler, v_in.uv + o) * wgt;
		sum   += image.Sample(blur_sampler, v_in.uv - o) * wgt;
		total += 2.0 * wgt;
	}
	return sum / total;
}

technique Draw
{
	pass
	{
		vertex_shader = mainTransform(v_in);
		pixel_shader  = mainBlur(v_in);
	}
}
)";

static const char *kPixelateEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float2 blocks;

sampler_state def_sampler {
	Filter   = Point;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData mainTransform(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = v_in.uv;
	return vert_out;
}

float4 mainImage(VertData v_in) : TARGET
{
	float2 uv = (floor(v_in.uv * blocks) + 0.5) / blocks;
	return image.Sample(def_sampler, uv);
}

technique Draw
{
	pass
	{
		vertex_shader = mainTransform(v_in);
		pixel_shader  = mainImage(v_in);
	}
}
)";

struct nsfw_filter {
	obs_source_t *self = nullptr;

	// --- réglages (écrits par l'UI, lus par les threads graphique/worker) ---
	std::atomic<float> threshold{0.60f};
	std::atomic<float> sexy_weight{0.25f};
	std::atomic<int> consecutive{2};
	std::atomic<int> interval{2};
	std::atomic<int> hold_ms{3000};
	std::atomic<int> mode{MASK_BLACK};
	std::atomic<int> pixel_size{16};
	std::atomic<int> blur_strength{30};
	std::atomic<int> method{METHOD_WHOLE};
	std::atomic<int> zone_size{448};
	std::atomic<bool> delay_on{true};
	std::atomic<bool> auto_delay{true};
	std::atomic<int> delay_frames{3};
	std::atomic<bool> use_dml{false};
	std::atomic<bool> block_until_ready{true};
	std::atomic<bool> debug{false};

	// --- ressources graphiques (thread graphique uniquement) ---
	std::vector<gs_texrender_t *> ring;
	std::vector<uint64_t> slot_id;
	gs_texrender_t *small_tr = nullptr;
	// Pyramide de l'image (niveau k = taille / 2^k), niveau 0 = l'image elle-même.
	gs_texrender_t *pyr[kMaxPyramid] = {};
	// Deux jeux de surfaces (alternés) pour rapatrier les vues sans bloquer le GPU.
	gs_stagesurf_t *stage[2][kMaxViews] = {};
	bool stage_valid[2] = {false, false};
	uint64_t stage_frame[2] = {0, 0};
	int stage_views[2] = {0, 0};
	int stage_zone_view[2] = {-1, -1};
	float stage_zone_x[2] = {0.0f, 0.0f};
	float stage_zone_y[2] = {0.0f, 0.0f};
	int stage_idx = 0;
	std::vector<uint8_t> map_buf;

	// --- zone aléatoire (thread graphique) ---
	// Les zones sont parcourues dans un ordre aléatoire, sans répétition, jusqu'à
	// avoir couvert tout l'écran (grille à demi-recouvrement). Une zone jugée
	// positive est ré-analysée telle quelle jusqu'à ce qu'elle redevienne négative.
	std::vector<std::pair<float, float>> zone_cycle;
	size_t zone_idx = 0;
	uint32_t zone_cycle_w = 0, zone_cycle_h = 0, zone_cycle_s = 0;
	bool zone_sticky = false;
	float zone_sx = 0.0f, zone_sy = 0.0f;
	std::mt19937 rng{std::random_device{}()};
	gs_effect_t *pixel_fx = nullptr;
	gs_effect_t *blur_fx = nullptr;
	gs_texrender_t *blur_tr[2] = {nullptr, nullptr};
	uint64_t blur_frame = UINT64_MAX; // image pour laquelle les passes de flou sont à jour
	uint32_t blur_w = 1, blur_h = 1;
	float blur_sigma = 1.0f;
	uint32_t w = 0, h = 0;

	// --- temporalité ---
	bool new_tick = false;
	uint64_t frame_id = 0; // incrémenté une fois par image réellement rendue
	int64_t display_id = -1;
	int cur_delay = 0;

	// --- état de décision ---
	int streak = 0;
	bool mask_active = false;
	uint64_t mask_start_id = 0;
	uint64_t last_pos_ns = 0;
	float last_score = 0.0f;
	float lat_ms_ema = 2.0f;
	uint64_t dbg_last_ns = 0;
	float dbg_max_score = 0.0f;

	// --- worker d'inférence ---
	std::thread worker;
	std::mutex job_m;
	std::condition_variable job_cv;
	bool quit = false;
	bool has_job = false;
	std::vector<uint8_t> job_buf; // kMaxViews plans de kPlaneBytes
	int job_views = 0;
	int job_zone_view = -1; // index de la vue "zone aléatoire" (-1 : aucune)
	float job_zone_x = 0.0f, job_zone_y = 0.0f;
	uint64_t job_frame = 0;
	uint64_t job_submit_ns = 0;
	std::wstring model_path;

	std::mutex res_m;
	bool has_result = false;
	uint64_t res_frame = 0;
	int res_views = 0;
	int res_zone_view = -1;
	float res_zone_x = 0.0f, res_zone_y = 0.0f;
	float res_probs[kMaxViews][kNumClasses] = {};
	float res_lat_ms = 0.0f;
	float res_infer_ms = 0.0f;

	std::atomic<bool> ready{false};
};

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

static void worker_main(nsfw_filter *f)
{
	std::unique_ptr<Classifier> clf;
	bool loaded_dml = false;
	bool failed = false;
	bool failed_dml = false;

	std::vector<uint8_t> buf(kPlaneBytes * kMaxViews);

	for (;;) {
		uint64_t frame = 0, submit_ns = 0;
		int views = 0;
		int zone_view = -1;
		float zone_x = 0.0f, zone_y = 0.0f;
		{
			std::unique_lock<std::mutex> lk(f->job_m);
			f->job_cv.wait(lk, [&] { return f->quit || f->has_job; });
			if (f->quit)
				break;
			buf.swap(f->job_buf);
			views = f->job_views;
			zone_view = f->job_zone_view;
			zone_x = f->job_zone_x;
			zone_y = f->job_zone_y;
			frame = f->job_frame;
			submit_ns = f->job_submit_ns;
			f->has_job = false;
		}

		const bool want_dml = f->use_dml.load();
		if ((!clf || want_dml != loaded_dml) && !(failed && failed_dml == want_dml)) {
			clf.reset();
			f->ready = false;
			try {
				clf = std::make_unique<Classifier>(f->model_path, want_dml);
				loaded_dml = want_dml;
				failed = false;
				f->ready = true;
				blog(LOG_INFO, LOG_PREFIX "modèle chargé (%s)", want_dml ? "GPU DirectML" : "CPU");
			} catch (const std::exception &e) {
				blog(LOG_ERROR, LOG_PREFIX "échec du chargement du modèle (%s) : %s",
				     want_dml ? "DirectML" : "CPU", e.what());
				failed = true;
				failed_dml = want_dml;
				if (want_dml) {
					// Repli automatique sur le CPU.
					try {
						clf = std::make_unique<Classifier>(f->model_path, false);
						loaded_dml = false;
						failed = false;
						f->ready = true;
						blog(LOG_WARNING, LOG_PREFIX "repli sur le CPU");
					} catch (const std::exception &e2) {
						blog(LOG_ERROR, LOG_PREFIX "échec du repli CPU : %s", e2.what());
					}
				}
			}
		}
		if (!clf)
			continue;

		views = std::clamp(views, 1, kMaxViews);
		float probs[kMaxViews][kNumClasses];
		float infer_ms = 0.0f;
		try {
			for (int v = 0; v < views; v++) {
				float ms = 0.0f;
				if (!clf->Run(buf.data() + static_cast<size_t>(v) * kPlaneBytes, kInputSize * 4, probs[v], ms))
					throw std::runtime_error("inférence sans résultat");
				infer_ms += ms;
			}
		} catch (const std::exception &e) {
			blog(LOG_ERROR, LOG_PREFIX "erreur d'inférence : %s", e.what());
			continue;
		}

		const uint64_t now = os_gettime_ns();
		std::lock_guard<std::mutex> lk(f->res_m);
		f->has_result = true;
		f->res_frame = frame;
		f->res_views = views;
		f->res_zone_view = zone_view;
		f->res_zone_x = zone_x;
		f->res_zone_y = zone_y;
		std::memcpy(f->res_probs, probs, sizeof(float) * kNumClasses * static_cast<size_t>(views));
		f->res_lat_ms = static_cast<float>(now - submit_ns) / 1.0e6f;
		f->res_infer_ms = infer_ms;
	}
}

// ---------------------------------------------------------------------------
// Cycle de vie
// ---------------------------------------------------------------------------

static const char *filter_get_name(void *)
{
	return obs_module_text("NsfwGuard");
}

static void filter_update(void *data, obs_data_t *s)
{
	auto *f = static_cast<nsfw_filter *>(data);
	f->threshold = static_cast<float>(obs_data_get_int(s, S_THRESHOLD)) / 100.0f;
	f->sexy_weight = static_cast<float>(obs_data_get_int(s, S_SEXY_WEIGHT)) / 100.0f;
	f->consecutive = static_cast<int>(obs_data_get_int(s, S_CONSECUTIVE));
	f->interval = std::max(1, static_cast<int>(obs_data_get_int(s, S_INTERVAL)));
	f->hold_ms = static_cast<int>(obs_data_get_int(s, S_HOLD_MS));
	f->mode = static_cast<int>(obs_data_get_int(s, S_MODE));
	f->pixel_size = std::max(1, static_cast<int>(obs_data_get_int(s, S_PIXEL_SIZE)));
	f->blur_strength = std::max(1, static_cast<int>(obs_data_get_int(s, S_BLUR_STRENGTH)));
	f->method = std::clamp(static_cast<int>(obs_data_get_int(s, S_METHOD)), 0, static_cast<int>(METHOD_MIXED));
	f->zone_size = std::max(64, static_cast<int>(obs_data_get_int(s, S_ZONE_SIZE)));
	f->delay_on = obs_data_get_bool(s, S_DELAY_ON);
	f->auto_delay = obs_data_get_bool(s, S_AUTO_DELAY);
	f->delay_frames = static_cast<int>(obs_data_get_int(s, S_DELAY_FRAMES));
	f->use_dml = obs_data_get_bool(s, S_USE_DML);
	f->block_until_ready = obs_data_get_bool(s, S_BLOCK_UNTIL_READY);
	f->debug = obs_data_get_bool(s, S_DEBUG);
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new nsfw_filter();
	f->self = source;
	f->job_buf.resize(kPlaneBytes * kMaxViews);
	f->map_buf.resize(kPlaneBytes * kMaxViews);

	char *model = obs_module_file("nsfw_model.onnx");
	if (model) {
		wchar_t *wide = nullptr;
		os_utf8_to_wcs_ptr(model, 0, &wide);
		if (wide) {
			f->model_path = wide;
			bfree(wide);
		}
		bfree(model);
	} else {
		blog(LOG_ERROR, LOG_PREFIX "nsfw_model.onnx introuvable dans les données du plugin");
	}

	std::string err;
	if (!ClassifierGlobalInit(g_module_dir, err))
		blog(LOG_ERROR, LOG_PREFIX "onnxruntime : %s", err.c_str());

	obs_enter_graphics();
	char *error = nullptr;
	f->pixel_fx = gs_effect_create(kPixelateEffect, "nsfw_guard_pixelate", &error);
	if (!f->pixel_fx)
		blog(LOG_ERROR, LOG_PREFIX "effet de pixelisation invalide : %s", error ? error : "?");
	bfree(error);
	error = nullptr;
	f->blur_fx = gs_effect_create(kBlurEffect, "nsfw_guard_blur", &error);
	if (!f->blur_fx)
		blog(LOG_ERROR, LOG_PREFIX "effet de flou invalide : %s", error ? error : "?");
	bfree(error);
	obs_leave_graphics();

	filter_update(f, settings);
	f->worker = std::thread(worker_main, f);
	return f;
}

static void filter_destroy(void *data)
{
	auto *f = static_cast<nsfw_filter *>(data);
	{
		std::lock_guard<std::mutex> lk(f->job_m);
		f->quit = true;
	}
	f->job_cv.notify_all();
	if (f->worker.joinable())
		f->worker.join();

	obs_enter_graphics();
	for (auto *t : f->ring)
		if (t)
			gs_texrender_destroy(t);
	if (f->small_tr)
		gs_texrender_destroy(f->small_tr);
	for (auto *t : f->pyr)
		if (t)
			gs_texrender_destroy(t);
	for (auto &set : f->stage)
		for (auto *s : set)
			if (s)
				gs_stagesurface_destroy(s);
	if (f->pixel_fx)
		gs_effect_destroy(f->pixel_fx);
	if (f->blur_fx)
		gs_effect_destroy(f->blur_fx);
	for (auto *t : f->blur_tr)
		if (t)
			gs_texrender_destroy(t);
	obs_leave_graphics();

	delete f;
}

// ---------------------------------------------------------------------------
// Propriétés
// ---------------------------------------------------------------------------

static bool mode_toggled(obs_properties_t *props, obs_property_t *, obs_data_t *s)
{
	const int mode = static_cast<int>(obs_data_get_int(s, S_MODE));
	obs_property_set_visible(obs_properties_get(props, S_PIXEL_SIZE), mode == MASK_PIXELATE);
	obs_property_set_visible(obs_properties_get(props, S_BLUR_STRENGTH), mode == MASK_BLUR);
	return true;
}

static bool method_toggled(obs_properties_t *props, obs_property_t *, obs_data_t *s)
{
	const int method = static_cast<int>(obs_data_get_int(s, S_METHOD));
	const bool uses_zone = method == METHOD_ZONE || method == METHOD_MIXED;
	// Avertissement pour les méthodes qui multiplient le travail (grilles, mixte).
	const bool costly = (method >= METHOD_GRID_2 && method <= METHOD_GRID_4) || method == METHOD_MIXED;
	obs_property_set_visible(obs_properties_get(props, S_ZONE_SIZE), uses_zone);
	obs_property_set_visible(obs_properties_get(props, S_METHOD_WARNING), costly);
	return true;
}

static bool delay_toggled(obs_properties_t *props, obs_property_t *, obs_data_t *s)
{
	const bool on = obs_data_get_bool(s, S_DELAY_ON);
	const bool automatic = obs_data_get_bool(s, S_AUTO_DELAY);
	obs_property_set_visible(obs_properties_get(props, S_AUTO_DELAY), on);
	obs_property_set_visible(obs_properties_get(props, S_DELAY_FRAMES), on && !automatic);
	return true;
}

static obs_properties_t *filter_properties(void *)
{
	obs_properties_t *p = obs_properties_create();

	obs_property_t *prop = obs_properties_add_int_slider(p, S_THRESHOLD, obs_module_text("Threshold"), 1, 100, 1);
	obs_property_set_long_description(prop, obs_module_text("Threshold.Desc"));

	prop = obs_properties_add_int_slider(p, S_SEXY_WEIGHT, obs_module_text("SexyWeight"), 0, 100, 1);
	obs_property_set_long_description(prop, obs_module_text("SexyWeight.Desc"));

	prop = obs_properties_add_int_slider(p, S_CONSECUTIVE, obs_module_text("Consecutive"), 1, 30, 1);
	obs_property_set_long_description(prop, obs_module_text("Consecutive.Desc"));

	obs_properties_add_int_slider(p, S_HOLD_MS, obs_module_text("HoldMs"), 0, 30000, 100);

	prop = obs_properties_add_list(p, S_MODE, obs_module_text("Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Mode.Black"), MASK_BLACK);
	obs_property_list_add_int(prop, obs_module_text("Mode.Pixelate"), MASK_PIXELATE);
	obs_property_list_add_int(prop, obs_module_text("Mode.Blur"), MASK_BLUR);
	obs_property_set_modified_callback(prop, mode_toggled);

	prop = obs_properties_add_int_slider(p, S_PIXEL_SIZE, obs_module_text("PixelSize"), 2, 200, 1);
	obs_property_set_long_description(prop, obs_module_text("PixelSize.Desc"));
	prop = obs_properties_add_int_slider(p, S_BLUR_STRENGTH, obs_module_text("BlurStrength"), 1, 100, 1);
	obs_property_set_long_description(prop, obs_module_text("BlurStrength.Desc"));

	prop = obs_properties_add_bool(p, S_DELAY_ON, obs_module_text("DelayOn"));
	obs_property_set_long_description(prop, obs_module_text("DelayOn.Desc"));
	obs_property_set_modified_callback(prop, delay_toggled);

	prop = obs_properties_add_bool(p, S_AUTO_DELAY, obs_module_text("AutoDelay"));
	obs_property_set_modified_callback(prop, delay_toggled);
	obs_properties_add_int_slider(p, S_DELAY_FRAMES, obs_module_text("DelayFrames"), 0, 30, 1);

	obs_properties_add_int_slider(p, S_INTERVAL, obs_module_text("Interval"), 1, 30, 1);

	prop = obs_properties_add_list(p, S_METHOD, obs_module_text("Method"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(prop, obs_module_text("Method.Desc"));
	obs_property_list_add_int(prop, obs_module_text("Method.Whole"), METHOD_WHOLE);
	obs_property_list_add_int(prop, obs_module_text("Method.Zone"), METHOD_ZONE);
	obs_property_list_add_int(prop, obs_module_text("Method.Mixed"), METHOD_MIXED);
	obs_property_list_add_int(prop, obs_module_text("Method.Grid2"), METHOD_GRID_2);
	obs_property_list_add_int(prop, obs_module_text("Method.Grid3"), METHOD_GRID_3);
	obs_property_list_add_int(prop, obs_module_text("Method.Grid4"), METHOD_GRID_4);
	obs_property_set_modified_callback(prop, method_toggled);

	prop = obs_properties_add_int_slider(p, S_ZONE_SIZE, obs_module_text("ZoneSize"), 224, 1080, 16);
	obs_property_set_long_description(prop, obs_module_text("ZoneSize.Desc"));

	// Le texte affiché d'un champ d'information est la valeur du réglage (voir les défauts).
	prop = obs_properties_add_text(p, S_METHOD_WARNING, obs_module_text("Method.WarningLabel"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(prop, OBS_TEXT_INFO_WARNING);

	prop = obs_properties_add_bool(p, S_USE_DML, obs_module_text("UseDml"));
	obs_property_set_long_description(prop, obs_module_text("UseDml.Desc"));
	obs_properties_add_bool(p, S_BLOCK_UNTIL_READY, obs_module_text("BlockUntilReady"));
	obs_properties_add_bool(p, S_DEBUG, obs_module_text("Debug"));

	return p;
}

static void filter_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, S_THRESHOLD, 60);
	obs_data_set_default_int(s, S_SEXY_WEIGHT, 25);
	obs_data_set_default_int(s, S_CONSECUTIVE, 2);
	obs_data_set_default_int(s, S_INTERVAL, 2);
	obs_data_set_default_int(s, S_HOLD_MS, 3000);
	obs_data_set_default_int(s, S_MODE, MASK_BLACK);
	obs_data_set_default_int(s, S_PIXEL_SIZE, 16);
	obs_data_set_default_int(s, S_BLUR_STRENGTH, 30);
	obs_data_set_default_int(s, S_METHOD, METHOD_WHOLE);
	obs_data_set_default_int(s, S_ZONE_SIZE, 448);
	obs_data_set_default_string(s, S_METHOD_WARNING, obs_module_text("Method.Warning"));
	obs_data_set_default_bool(s, S_DELAY_ON, true);
	obs_data_set_default_bool(s, S_AUTO_DELAY, true);
	obs_data_set_default_int(s, S_DELAY_FRAMES, 3);
	obs_data_set_default_bool(s, S_USE_DML, false);
	obs_data_set_default_bool(s, S_BLOCK_UNTIL_READY, true);
	obs_data_set_default_bool(s, S_DEBUG, false);
}

// ---------------------------------------------------------------------------
// Rendu
// ---------------------------------------------------------------------------

static void filter_tick(void *data, float)
{
	static_cast<nsfw_filter *>(data)->new_tick = true;
}

static float frame_ms()
{
	obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0)
		return 1000.0f * static_cast<float>(ovi.fps_den) / static_cast<float>(ovi.fps_num);
	return 16.7f;
}

static void consume_result(nsfw_filter *f)
{
	float probs[kMaxViews][kNumClasses];
	uint64_t frame;
	int views, zone_view;
	float zone_x, zone_y;
	float lat_ms, infer_ms;
	{
		std::lock_guard<std::mutex> lk(f->res_m);
		if (!f->has_result)
			return;
		f->has_result = false;
		frame = f->res_frame;
		views = f->res_views;
		zone_view = f->res_zone_view;
		zone_x = f->res_zone_x;
		zone_y = f->res_zone_y;
		lat_ms = f->res_lat_ms;
		infer_ms = f->res_infer_ms;
		std::memcpy(probs, f->res_probs, sizeof(float) * kNumClasses * static_cast<size_t>(views));
	}

	f->lat_ms_ema = 0.9f * f->lat_ms_ema + 0.1f * lat_ms;

	// Le score de l'image est celui de sa vue la plus sensible : il suffit qu'une
	// zone soit jugée sensible pour que l'image le soit.
	const float sexy_weight = f->sexy_weight.load();
	const float threshold = f->threshold.load();
	float score = 0.0f;
	float zone_score = 0.0f;
	int best_view = 0;
	for (int v = 0; v < views; v++) {
		const float s = std::min(1.0f, probs[v][kClassPorn] + probs[v][kClassHentai] +
						       sexy_weight * probs[v][kClassSexy]);
		if (v == zone_view)
			zone_score = s;
		if (s > score) {
			score = s;
			best_view = v;
		}
	}
	f->last_score = score;

	// Zone aléatoire : une zone positive est ré-analysée telle quelle ; dès qu'une
	// analyse de cette zone revient négative, on repart sur des zones aléatoires.
	if (zone_view >= 0 && zone_view < views) {
		if (zone_score >= threshold) {
			f->zone_sticky = true;
			f->zone_sx = zone_x;
			f->zone_sy = zone_y;
		} else if (f->zone_sticky && zone_x == f->zone_sx && zone_y == f->zone_sy) {
			f->zone_sticky = false;
		}
	}
	f->dbg_max_score = std::max(f->dbg_max_score, score);

	const uint64_t now = os_gettime_ns();
	if (score >= threshold) {
		f->streak++;
		if (f->streak >= f->consecutive.load()) {
			if (!f->mask_active) {
				f->mask_active = true;
				const uint64_t back = static_cast<uint64_t>(f->consecutive.load() - 1) *
						      static_cast<uint64_t>(f->interval.load());
				f->mask_start_id = frame > back ? frame - back : 0;
				if (best_view == zone_view)
					blog(LOG_INFO,
					     LOG_PREFIX "contenu sensible détecté (score %.2f, zone aléatoire en x=%.0f y=%.0f sur %ux%u) : masque activé",
					     score, zone_x, zone_y, f->w, f->h);
				else
					blog(LOG_INFO, LOG_PREFIX "contenu sensible détecté (score %.2f, vue %d/%d) : masque activé",
					     score, best_view, views);
			}
			f->last_pos_ns = now;
		}
	} else {
		f->streak = 0;
	}

	if (f->debug) {
		if (now - f->dbg_last_ns > 1000000000ULL) {
			blog(LOG_INFO,
			     LOG_PREFIX "score max=%.2f  dernier=%.2f (vue %d/%d) | inférence=%.2f ms  latence=%.2f ms | délai=%d img | masque=%s%s",
			     f->dbg_max_score, score, best_view, views, infer_ms, f->lat_ms_ema, f->cur_delay,
			     f->mask_active ? "OUI" : "non", f->zone_sticky ? " | zone verrouillée" : "");
			f->dbg_last_ns = now;
			f->dbg_max_score = 0.0f;
		}
	}
}

static void ensure_gfx(nsfw_filter *f, uint32_t w, uint32_t h, int delay)
{
	if (w != f->w || h != f->h) {
		for (auto *t : f->ring)
			if (t)
				gs_texrender_destroy(t);
		f->ring.clear();
		f->slot_id.clear();
		f->w = w;
		f->h = h;
	}

	// Taille du tampon par paliers de 8 pour éviter de le réallouer sans cesse.
	size_t want = static_cast<size_t>(((delay + 2) + 7) / 8) * 8;
	want = std::max<size_t>(want, 8);
	if (want > f->ring.size()) {
		while (f->ring.size() < want)
			f->ring.push_back(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
		f->slot_id.assign(f->ring.size(), UINT64_MAX);
	}

	if (!f->small_tr)
		f->small_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
}

static void capture_target(nsfw_filter *f, obs_source_t *target, gs_texrender_t *tr)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, f->w, f->h))
		return;

	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, static_cast<float>(f->w), 0.0f, static_cast<float>(f->h), -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	obs_source_video_render(target);
	gs_blend_state_pop();

	gs_texrender_end(tr);
}

// --- vues : ce qu'on envoie au modèle pour une image ------------------------

struct ViewRect {
	float x, y, w, h; // en pixels de l'image pleine taille
};

struct ViewPlan {
	ViewRect rect[kMaxViews];
	int count = 0;
	int zone_view = -1; // index de la zone aléatoire dans rect[], -1 si aucune
};

// Prochaine zone aléatoire de côté `s`, en pixels de la source. Les positions
// forment une grille à demi-recouvrement, parcourue dans un ordre aléatoire :
// tout l'écran est couvert avant qu'une zone ne revienne. Une zone verrouillée
// (résultat positif) est reprise telle quelle.
static void pick_zone(nsfw_filter *f, uint32_t s, float &x, float &y)
{
	if (f->zone_sticky) {
		x = std::min(f->zone_sx, static_cast<float>(f->w - s));
		y = std::min(f->zone_sy, static_cast<float>(f->h - s));
		return;
	}

	if (f->zone_cycle.empty() || f->zone_idx >= f->zone_cycle.size() || f->zone_cycle_w != f->w ||
	    f->zone_cycle_h != f->h || f->zone_cycle_s != s) {
		const float step = std::max(1.0f, static_cast<float>(s) / 2.0f);
		auto positions = [&](uint32_t total) {
			std::vector<float> p;
			const float last = static_cast<float>(total - s);
			for (float v = 0.0f; v < last; v += step)
				p.push_back(v);
			p.push_back(last); // dernière position collée au bord
			return p;
		};
		const std::vector<float> xs = positions(f->w), ys = positions(f->h);
		f->zone_cycle.clear();
		for (float py : ys)
			for (float px : xs)
				f->zone_cycle.emplace_back(px, py);
		std::shuffle(f->zone_cycle.begin(), f->zone_cycle.end(), f->rng);
		f->zone_idx = 0;
		f->zone_cycle_w = f->w;
		f->zone_cycle_h = f->h;
		f->zone_cycle_s = s;
	}

	x = f->zone_cycle[f->zone_idx].first;
	y = f->zone_cycle[f->zone_idx].second;
	f->zone_idx++;
}

static ViewPlan plan_views(nsfw_filter *f)
{
	ViewPlan plan;
	const float W = static_cast<float>(f->w), H = static_cast<float>(f->h);
	const int method = f->method.load();

	const bool whole = method != METHOD_ZONE;
	if (whole)
		plan.rect[plan.count++] = {0.0f, 0.0f, W, H};

	if (method >= METHOD_GRID_2 && method <= METHOD_GRID_4) {
		const int n = method + 1; // 2, 3, 4
		const float tw = W / static_cast<float>(n), th = H / static_cast<float>(n);
		for (int row = 0; row < n; row++)
			for (int col = 0; col < n; col++)
				plan.rect[plan.count++] = {static_cast<float>(col) * tw, static_cast<float>(row) * th, tw, th};
	}

	if (method == METHOD_ZONE || method == METHOD_MIXED) {
		const uint32_t s = std::min<uint32_t>(static_cast<uint32_t>(f->zone_size.load()), std::min(f->w, f->h));
		float x = 0.0f, y = 0.0f;
		pick_zone(f, s, x, y);
		plan.zone_view = plan.count;
		plan.rect[plan.count++] = {x, y, static_cast<float>(s), static_cast<float>(s)};
	}
	return plan;
}

// Niveau de la pyramide à utiliser pour une vue : le plus réduit qui garde
// encore au moins 224 px sur le plus petit côté (on ne sur-échantillonne jamais).
static int pyramid_level_for(const ViewRect &r)
{
	const float smallest = std::min(r.w, r.h);
	int k = 0;
	while (k + 1 < kMaxPyramid && smallest / static_cast<float>(1u << (k + 1)) >= static_cast<float>(kInputSize))
		k++;
	return k;
}

static gs_texture_t *pyramid_texture(nsfw_filter *f, gs_texture_t *full, int level)
{
	return level == 0 ? full : gs_texrender_get_texture(f->pyr[level - 1]);
}

// Réductions successives par 2. Un filtre bilinéaire à exactement 2:1 fait la
// moyenne de 4 pixels : chaque niveau est un vrai résumé du précédent, ce qui
// évite de "sauter" des pixels comme le ferait une réduction directe.
static void build_pyramid(nsfw_filter *f, gs_texture_t *full, int max_level)
{
	if (max_level <= 0)
		return;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(eff, "image");

	gs_texture_t *prev = full;
	for (int k = 1; k <= max_level; k++) {
		const uint32_t lw = std::max<uint32_t>(1, f->w >> k);
		const uint32_t lh = std::max<uint32_t>(1, f->h >> k);
		if (!f->pyr[k - 1])
			f->pyr[k - 1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

		gs_texrender_reset(f->pyr[k - 1]);
		if (!gs_texrender_begin(f->pyr[k - 1], lw, lh))
			break;
		gs_ortho(0.0f, static_cast<float>(lw), 0.0f, static_cast<float>(lh), -100.0f, 100.0f);
		gs_effect_set_texture(image, prev);
		while (gs_effect_loop(eff, "Draw"))
			gs_draw_sprite(prev, 0, lw, lh);
		gs_texrender_end(f->pyr[k - 1]);
		prev = gs_texrender_get_texture(f->pyr[k - 1]);
	}
	gs_blend_state_pop();
}

// Dessine la vue `v` en 224x224 dans small_tr. La zone à lire est choisie par la
// projection (gs_ortho) : elle est mise à l'échelle du viewport 224x224.
static bool render_view(nsfw_filter *f, gs_texture_t *full, const ViewRect &r)
{
	constexpr uint32_t sz = kInputSize;
	const int level = pyramid_level_for(r);
	gs_texture_t *src = pyramid_texture(f, full, level);
	if (!src)
		return false;

	const uint32_t lw = std::max<uint32_t>(1, f->w >> level);
	const uint32_t lh = std::max<uint32_t>(1, f->h >> level);
	const float sx = static_cast<float>(lw) / static_cast<float>(f->w);
	const float sy = static_cast<float>(lh) / static_cast<float>(f->h);
	const float x0 = r.x * sx, y0 = r.y * sy;
	const float x1 = x0 + r.w * sx, y1 = y0 + r.h * sy;

	gs_texrender_reset(f->small_tr);
	if (!gs_texrender_begin(f->small_tr, sz, sz))
		return false;

	vec4 black;
	vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
	gs_ortho(x0, x1, y0, y1, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(eff, "image"), src);
	while (gs_effect_loop(eff, "Draw"))
		gs_draw_sprite(src, 0, lw, lh);
	gs_blend_state_pop();
	gs_texrender_end(f->small_tr);
	return true;
}

// Prépare toutes les vues de l'image, les rapatrie côté CPU (avec une image de
// retard, via deux jeux de surfaces en alternance pour ne jamais bloquer le GPU)
// et les soumet au worker.
static void analyze(nsfw_filter *f, gs_texture_t *full)
{
	constexpr uint32_t sz = kInputSize;
	const ViewPlan plan = plan_views(f);

	// Niveau de pyramide le plus profond dont une vue a besoin.
	int max_level = 0;
	for (int v = 0; v < plan.count; v++)
		max_level = std::max(max_level, pyramid_level_for(plan.rect[v]));
	build_pyramid(f, full, max_level);

	const int cur = f->stage_idx;
	const int prev = 1 - cur;

	int staged = 0;
	for (int v = 0; v < plan.count; v++) {
		if (!render_view(f, full, plan.rect[v]))
			break;
		if (!f->stage[cur][v])
			f->stage[cur][v] = gs_stagesurface_create(sz, sz, GS_RGBA);
		gs_stage_texture(f->stage[cur][v], gs_texrender_get_texture(f->small_tr));
		staged++;
	}
	f->stage_valid[cur] = staged > 0;
	f->stage_views[cur] = staged;
	f->stage_frame[cur] = f->frame_id;
	// La zone n'est utilisable que si sa vue a bien été préparée.
	f->stage_zone_view[cur] = plan.zone_view >= 0 && plan.zone_view < staged ? plan.zone_view : -1;
	f->stage_zone_x[cur] = plan.zone_view >= 0 ? plan.rect[plan.zone_view].x : 0.0f;
	f->stage_zone_y[cur] = plan.zone_view >= 0 ? plan.rect[plan.zone_view].y : 0.0f;

	if (f->stage_valid[prev]) {
		int copied = 0;
		for (int v = 0; v < f->stage_views[prev]; v++) {
			uint8_t *data = nullptr;
			uint32_t linesize = 0;
			if (!f->stage[prev][v] || !gs_stagesurface_map(f->stage[prev][v], &data, &linesize))
				break;
			uint8_t *dst = f->map_buf.data() + static_cast<size_t>(v) * kPlaneBytes;
			for (uint32_t y = 0; y < sz; y++)
				std::memcpy(dst + static_cast<size_t>(y) * sz * 4,
					    data + static_cast<size_t>(y) * linesize, sz * 4);
			gs_stagesurface_unmap(f->stage[prev][v]);
			copied++;
		}

		if (copied == f->stage_views[prev] && copied > 0) {
			{
				std::lock_guard<std::mutex> lk(f->job_m);
				f->job_buf.swap(f->map_buf);
				f->job_views = copied;
				f->job_zone_view = f->stage_zone_view[prev];
				f->job_zone_x = f->stage_zone_x[prev];
				f->job_zone_y = f->stage_zone_y[prev];
				f->job_frame = f->stage_frame[prev];
				f->job_submit_ns = os_gettime_ns();
				f->has_job = true;
			}
			f->job_cv.notify_one();
		}
		f->stage_valid[prev] = false;
	}
	f->stage_idx = prev;
}

static int compute_delay(nsfw_filter *f)
{
	if (!f->delay_on)
		return 0;
	if (!f->auto_delay)
		return std::clamp(f->delay_frames.load(), 0, 30);

	// Analyse d'une image sur `interval`, +1 image pour le rapatriement GPU->CPU,
	// + le temps de calcul mesuré, + 1 image de marge.
	const float fm = frame_ms();
	const int lat_frames = static_cast<int>(std::ceil(f->lat_ms_ema / fm));
	return std::clamp(f->interval.load() + 1 + lat_frames + 1, 1, 30);
}

// Prépare le flou : l'image est réduite d'un facteur `d` (pour que l'écart-type
// en pixels réduits reste <= 4, ce qui suffit à 12 échantillons par côté), puis
// floutée horizontalement. La passe verticale est faite au moment du dessin.
// Fait une seule fois par image, même si le rendu est appelé plusieurs fois.
static void render_blur_passes(nsfw_filter *f, gs_texture_t *src)
{
	if (f->blur_frame == f->frame_id)
		return;

	const int strength = std::max(1, f->blur_strength.load());
	const int d = std::max(1, (strength + 3) / 4);
	const uint32_t sw = std::max<uint32_t>(1, f->w / static_cast<uint32_t>(d));
	const uint32_t sh = std::max<uint32_t>(1, f->h / static_cast<uint32_t>(d));

	for (auto &t : f->blur_tr)
		if (!t)
			t = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	f->blur_w = sw;
	f->blur_h = sh;
	f->blur_sigma = std::max(0.5f, static_cast<float>(strength) / static_cast<float>(d));

	vec4 clear;
	vec4_zero(&clear);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	// 1) réduction
	gs_texrender_reset(f->blur_tr[0]);
	if (gs_texrender_begin(f->blur_tr[0], sw, sh)) {
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(sw), 0.0f, static_cast<float>(sh), -100.0f, 100.0f);
		gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(eff, "image"), src);
		while (gs_effect_loop(eff, "Draw"))
			gs_draw_sprite(src, 0, sw, sh);
		gs_texrender_end(f->blur_tr[0]);
	}

	// 2) flou horizontal
	gs_texrender_reset(f->blur_tr[1]);
	if (gs_texrender_begin(f->blur_tr[1], sw, sh)) {
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(sw), 0.0f, static_cast<float>(sh), -100.0f, 100.0f);
		gs_texture_t *reduced = gs_texrender_get_texture(f->blur_tr[0]);
		vec2 texel;
		vec2_set(&texel, 1.0f / static_cast<float>(sw), 0.0f);
		gs_effect_set_texture(gs_effect_get_param_by_name(f->blur_fx, "image"), reduced);
		gs_effect_set_vec2(gs_effect_get_param_by_name(f->blur_fx, "texel"), &texel);
		gs_effect_set_float(gs_effect_get_param_by_name(f->blur_fx, "sigma"), f->blur_sigma);
		while (gs_effect_loop(f->blur_fx, "Draw"))
			gs_draw_sprite(reduced, 0, sw, sh);
		gs_texrender_end(f->blur_tr[1]);
	}

	gs_blend_state_pop();
	f->blur_frame = f->frame_id;
}

static void draw_mask(nsfw_filter *f, gs_texture_t *tex)
{
	if (f->mode == MASK_BLUR && f->blur_fx && tex) {
		render_blur_passes(f, tex);

		// 3) flou vertical, dessiné à la taille réelle (remise à l'échelle linéaire)
		gs_texture_t *horiz = gs_texrender_get_texture(f->blur_tr[1]);
		if (horiz) {
			vec2 texel;
			vec2_set(&texel, 0.0f, 1.0f / static_cast<float>(f->blur_h));
			gs_effect_set_texture(gs_effect_get_param_by_name(f->blur_fx, "image"), horiz);
			gs_effect_set_vec2(gs_effect_get_param_by_name(f->blur_fx, "texel"), &texel);
			gs_effect_set_float(gs_effect_get_param_by_name(f->blur_fx, "sigma"), f->blur_sigma);
			while (gs_effect_loop(f->blur_fx, "Draw"))
				gs_draw_sprite(horiz, 0, f->w, f->h);
			return;
		}
	}

	if (f->mode == MASK_PIXELATE && f->pixel_fx && tex) {
		gs_eparam_t *img = gs_effect_get_param_by_name(f->pixel_fx, "image");
		gs_eparam_t *blocks = gs_effect_get_param_by_name(f->pixel_fx, "blocks");
		const float px = static_cast<float>(std::max(1, f->pixel_size.load()));
		vec2 b;
		vec2_set(&b, std::max(1.0f, static_cast<float>(f->w) / px),
			 std::max(1.0f, static_cast<float>(f->h) / px));
		gs_effect_set_texture(img, tex);
		gs_effect_set_vec2(blocks, &b);
		while (gs_effect_loop(f->pixel_fx, "Draw"))
			gs_draw_sprite(tex, 0, f->w, f->h);
		return;
	}

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	vec4 black;
	vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color, &black);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, f->w, f->h);
}

static void filter_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<nsfw_filter *>(data);
	obs_source_t *target = obs_filter_get_target(f->self);
	obs_source_t *parent = obs_filter_get_parent(f->self);
	if (!target || !parent) {
		obs_source_skip_video_filter(f->self);
		return;
	}

	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	if (!w || !h) {
		obs_source_skip_video_filter(f->self);
		return;
	}

	if (f->new_tick) {
		f->new_tick = false;
		f->frame_id++;

		consume_result(f);

		const uint64_t now = os_gettime_ns();
		if (f->mask_active &&
		    now - f->last_pos_ns > static_cast<uint64_t>(f->hold_ms.load()) * 1000000ULL) {
			f->mask_active = false;
			f->streak = 0;
			blog(LOG_INFO, LOG_PREFIX "masque levé");
		}

		f->cur_delay = compute_delay(f);
		ensure_gfx(f, w, h, f->cur_delay);

		const size_t n = f->ring.size();
		const size_t slot = static_cast<size_t>(f->frame_id % n);
		capture_target(f, target, f->ring[slot]);
		f->slot_id[slot] = f->frame_id;

		if (f->frame_id % static_cast<uint64_t>(f->interval.load()) == 0)
			analyze(f, gs_texrender_get_texture(f->ring[slot]));

		f->display_id = static_cast<int64_t>(f->frame_id) - f->cur_delay;
	}

	if (f->ring.empty() || f->display_id < 0)
		return;

	const size_t n = f->ring.size();
	const size_t slot = static_cast<size_t>(static_cast<uint64_t>(f->display_id) % n);
	if (f->slot_id[slot] != static_cast<uint64_t>(f->display_id))
		return; // image pas (encore) disponible : on n'affiche rien plutôt que du hors-sujet

	gs_texture_t *tex = gs_texrender_get_texture(f->ring[slot]);
	if (!tex)
		return;

	const bool masked = (f->mask_active && static_cast<uint64_t>(f->display_id) >= f->mask_start_id) ||
			    (!f->ready.load() && f->block_until_ready.load());

	if (masked) {
		draw_mask(f, tex);
		return;
	}

	gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(eff, "image"), tex);
	while (gs_effect_loop(eff, "Draw"))
		gs_draw_sprite(tex, 0, f->w, f->h);
}

// ---------------------------------------------------------------------------

static obs_source_info make_info()
{
	obs_source_info info = {};
	info.id = FILTER_ID;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = filter_get_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.update = filter_update;
	info.get_defaults = filter_defaults;
	info.get_properties = filter_properties;
	info.video_tick = filter_tick;
	info.video_render = filter_render;
	return info;
}

void register_nsfw_filter()
{
	static obs_source_info info = make_info();
	obs_register_source(&info);
}
