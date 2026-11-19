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
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "classifier.h"
#include "detector.h"
#include "model-download.h"
#include "nsfw-filter.h"
#include "updater.h"

#define FILTER_ID "nsfw_guard_filter"
#define LOG_PREFIX "[nsfw-guard] "

// --- clés de réglage -------------------------------------------------------
#ifdef S_THRESHOLD
#undef S_THRESHOLD // définie par un en-tête Windows
#endif
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
#define S_ENGINE "engine"             // moteur d'analyse, voir enum Engine
#define S_ENGINE_WARNING "engine_warning"
#define S_DET_THRESHOLD "det_threshold" // % : confiance minimale d'une détection NudeNet
#define S_LOCALIZED "localized"       // ne masquer que la zone détectée (moteurs NudeNet)
#define S_REGION_MARGIN "region_margin" // % : marge ajoutée autour d'une zone détectée
#define S_CLASSES_GROUP "classes_group"
#define S_MODELS_GROUP "models_group"
#define S_MODELS_STATUS "models_status" // texte d'état affiché (réécrit à chaque ouverture des réglages)
#define S_DL_ACCEPT "dl_accept"       // l'utilisateur a lu la licence et accepte le téléchargement
#define S_IMPORT_PATH "import_path"   // fichier modèle fourni par l'utilisateur

// Page officielle où l'utilisateur peut télécharger lui-même les modèles.
#define NUDENET_RELEASE_URL L"https://github.com/notAI-tech/NudeNet/releases/tag/v3.4-weights"
#define S_CLASS_PREFIX "cls_"         // cls_0 .. cls_17 : classes NudeNet qui déclenchent le masque

// Moteur d'analyse. Valeurs stockées dans les réglages : ne pas renuméroter.
enum Engine {
	ENGINE_MOBILENET = 0,   // classifieur MobileNet livré avec le plugin
	ENGINE_NUDENET_320N = 1, // détecteur NudeNet 320n (petit, rapide)
	ENGINE_NUDENET_640M = 2, // détecteur NudeNet 640m (précis, plus lourd)
};

static float frame_ms();

static int engine_side(int engine)
{
	return engine == ENGINE_NUDENET_320N ? 320 : engine == ENGINE_NUDENET_640M ? 640 : kInputSize;
}

// Classes NudeNet qui déclenchent le masque par défaut : tout ce qui est explicite.
static bool class_default_on(int cls)
{
	switch (cls) {
	case 2:  // BUTTOCKS_EXPOSED
	case 3:  // FEMALE_BREAST_EXPOSED
	case 4:  // FEMALE_GENITALIA_EXPOSED
	case 6:  // ANUS_EXPOSED
	case 14: // MALE_GENITALIA_EXPOSED
		return true;
	default:
		return false;
	}
}

// Méthode d'analyse. Valeurs stockées dans les réglages : ne pas renuméroter.
enum Method {
	METHOD_WHOLE = 0,   // l'image entière, réduite en 224x224
	METHOD_GRID_2 = 1,  // l'image entière + une grille 2x2 de zones
	METHOD_GRID_3 = 2,  // ... 3x3
	METHOD_GRID_4 = 3,  // ... 4x4
	METHOD_ZONE = 4,    // une zone aléatoire (on y reste tant qu'elle est positive)
	METHOD_MIXED = 5,   // l'image entière + une zone aléatoire
};

// Une "vue" est une image carrée (224 pour le classifieur, 320 ou 640 pour NudeNet)
// envoyée au modèle, découpée dans l'image de la source : l'image entière, une zone
// d'une grille, ou la zone aléatoire.
constexpr int kMaxViews = 17;                 // 1 + 4x4
constexpr int kMaxPyramid = 6;                // niveaux de réduction par 2 (4K -> 3840/2^5 = 120)
constexpr int kMaxRegions = 16;               // zones masquées simultanément (mode localisé)
constexpr float kDetFloor = 0.15f;            // score en dessous duquel une détection est ignorée d'office

struct ViewRect {
	float x, y, w, h; // en pixels de l'image pleine taille
};

// Zone de l'image actuellement masquée (mode localisé), en pixels de la source.
struct Region {
	float x, y, w, h;
	uint64_t start_id; // le masque s'applique aux images affichées à partir de celle-ci
	uint64_t last_ns;  // dernière confirmation, pour le maintien
};
#define S_DELAY_ON "delay_on"         // retarder l'image pour que le masque arrive à temps
#define S_AUTO_DELAY "auto_delay"     // calculer le délai automatiquement
#define S_DELAY_FRAMES "delay_frames" // délai manuel, en images
#define S_USE_DML "use_dml"           // GPU (DirectML) au lieu du CPU
#define S_BLOCK_UNTIL_READY "block_until_ready"
#define S_DEBUG "debug"
#define S_UPD_GROUP "upd_group"
#define S_UPD_STATUS "upd_status"  // texte d'état des mises à jour (réécrit à chaque ouverture)
#define S_UPD_AUTO "upd_auto"      // reflet du réglage global (updater.json), pas un réglage de la source
#define S_STATUS "status"             // texte d'état affiché (réécrit à chaque ouverture des réglages)

// État du moteur d'analyse, affiché dans les réglages.
enum EngineStatus {
	STATUS_LOADING = 0,
	STATUS_READY_CPU = 1,
	STATUS_READY_GPU = 2,
	STATUS_RUNTIME_ERROR = 3, // onnxruntime.dll introuvable ou incompatible
	STATUS_MODEL_ERROR = 4,   // le modèle n'a pas pu être chargé
	STATUS_INFER_ERROR = 5,   // l'inférence échoue
};

// Sans résultat d'analyse depuis ce délai (et au moins 3x le temps d'analyse mesuré),
// l'analyse est considérée comme bloquée : la source est masquée si « Masquer tant que
// le modèle n'est pas prêt » est coché (sécurité par défaut).
constexpr uint64_t kStallNs = 3000000000ULL;
// Une source qui n'a pas été rendue pendant ce délai (scène inactive, source masquée) :
// les images en tampon sont périmées et ne doivent pas être réaffichées.
constexpr uint64_t kRenderGapNs = 500000000ULL;
// Le délai automatique ne diminue que s'il est resté trop grand pendant ce délai
// (chaque changement fait sauter ou répéter une image, et décale l'audio).
constexpr uint64_t kDelayDownNs = 10000000000ULL;
// Intervalle minimal entre deux messages d'erreur identiques dans le journal.
constexpr uint64_t kErrLogNs = 5000000000ULL;

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
	std::atomic<int> engine{ENGINE_MOBILENET};
	std::atomic<float> det_threshold{0.40f};
	std::atomic<bool> localized{true};
	std::atomic<float> region_margin{0.25f};
	std::atomic<uint32_t> class_mask{0}; // bit i = la classe NudeNet i déclenche le masque
	std::atomic<bool> det_failed{false}; // détecteur indisponible : repli sur le classifieur
	std::atomic<bool> dl_accept{false};
	uint32_t seen_gen = 0; // dernière génération de modèles installés prise en compte
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
	int stage_side[2] = {0, 0};
	int stage_engine[2] = {0, 0};
	ViewRect stage_rect[2][kMaxViews] = {};
	int stage_zone_view[2] = {-1, -1};
	float stage_zone_x[2] = {0.0f, 0.0f};
	float stage_zone_y[2] = {0.0f, 0.0f};
	int stage_idx = 0;
	int surf_side = 0; // côté des surfaces de rapatriement actuellement créées
	std::vector<uint8_t> map_buf;

	// --- zones masquées (mode localisé, thread graphique) ---
	std::vector<Region> regions;

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
	std::atomic<int> cur_delay{0}; // lu aussi par l'interface (affichage)

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
	std::vector<uint8_t> job_buf; // `job_views` plans consécutifs de job_side x job_side x 4 octets
	int job_views = 0;
	int job_engine = ENGINE_MOBILENET;
	int job_side = kInputSize;
	ViewRect job_rect[kMaxViews] = {};
	int job_zone_view = -1; // index de la vue "zone aléatoire" (-1 : aucune)
	float job_zone_x = 0.0f, job_zone_y = 0.0f;
	uint64_t job_frame = 0;
	uint64_t job_submit_ns = 0;
	std::wstring model_path;
	std::wstring det_path[2]; // NudeNet 320n, 640m ; vide si le fichier est absent

	std::mutex res_m;
	bool has_result = false;
	uint64_t res_frame = 0;
	int res_views = 0;
	int res_engine = ENGINE_MOBILENET;
	int res_side = kInputSize;
	ViewRect res_rect[kMaxViews] = {};
	int res_zone_view = -1;
	float res_zone_x = 0.0f, res_zone_y = 0.0f;
	float res_probs[kMaxViews][kNumClasses] = {}; // classifieur
	std::vector<Detection> res_dets;               // détecteur (toutes vues confondues)
	float res_lat_ms = 0.0f;
	float res_infer_ms = 0.0f;

	std::atomic<bool> ready{false};
	std::atomic<int> status{STATUS_LOADING};

	// --- sûreté (thread graphique) ---
	uint64_t pending_since_ns = 0; // première analyse soumise restée sans résultat (0 : aucune)
	uint64_t last_render_ns = 0;   // dernière image traitée, pour détecter une reprise après pause
	uint64_t delay_down_ns = 0;    // depuis quand le délai automatique pourrait diminuer
	bool stalled = false;
};

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

static void worker_main(nsfw_filter *f)
{
	std::unique_ptr<Classifier> clf;
	bool clf_dml = false;
	bool clf_failed = false;
	bool clf_failed_dml = false;

	std::unique_ptr<Detector> det;
	int det_engine = -1;
	bool det_dml = false;

	std::vector<uint8_t> buf;
	std::vector<Detection> dets, view_dets;
	uint64_t last_err_log = 0;
	bool ort_err_logged = false;

	// Erreur d'inférence : journalisée au plus toutes les kErrLogNs pour ne pas noyer le
	// journal d'OBS (jusqu'à 60 analyses par seconde).
	auto log_infer_error = [&](const char *what) {
		f->status = STATUS_INFER_ERROR;
		const uint64_t t = os_gettime_ns();
		if (t - last_err_log > kErrLogNs) {
			blog(LOG_ERROR, LOG_PREFIX "erreur d'inférence : %s", what);
			last_err_log = t;
		}
	};

	for (;;) {
		uint64_t frame = 0, submit_ns = 0;
		int views = 0, engine = ENGINE_MOBILENET, side = kInputSize;
		int zone_view = -1;
		float zone_x = 0.0f, zone_y = 0.0f;
		ViewRect rects[kMaxViews] = {};
		std::wstring det_path;
		{
			std::unique_lock<std::mutex> lk(f->job_m);
			f->job_cv.wait(lk, [&] { return f->quit || f->has_job; });
			if (f->quit)
				break;
			buf.swap(f->job_buf);
			views = f->job_views;
			engine = f->job_engine;
			side = f->job_side;
			std::copy(f->job_rect, f->job_rect + kMaxViews, rects);
			if (engine == ENGINE_NUDENET_320N || engine == ENGINE_NUDENET_640M)
				det_path = f->det_path[engine - 1];
			zone_view = f->job_zone_view;
			zone_x = f->job_zone_x;
			zone_y = f->job_zone_y;
			frame = f->job_frame;
			submit_ns = f->job_submit_ns;
			f->has_job = false;
		}

		// ONNX Runtime absent ou incompatible : rien ne doit toucher à son API (plantage).
		if (!OrtAvailable()) {
			f->ready = false;
			f->status = STATUS_RUNTIME_ERROR;
			if (!ort_err_logged) {
				blog(LOG_ERROR, LOG_PREFIX "ONNX Runtime indisponible : aucune analyse possible. Réinstallez le plugin.");
				ort_err_logged = true;
			}
			continue;
		}

		views = std::clamp(views, 1, kMaxViews);
		const size_t plane = static_cast<size_t>(side) * side * 4;
		if (buf.size() < plane * static_cast<size_t>(views))
			continue;

		const bool want_dml = f->use_dml.load();
		float probs[kMaxViews][kNumClasses];
		float infer_ms = 0.0f;
		dets.clear();

		if (engine == ENGINE_MOBILENET) {
			if ((!clf || want_dml != clf_dml) && !(clf_failed && clf_failed_dml == want_dml)) {
				clf.reset();
				f->ready = false;
				f->status = STATUS_LOADING;
				try {
					clf = std::make_unique<Classifier>(f->model_path, want_dml);
					clf_dml = want_dml;
					clf_failed = false;
					blog(LOG_INFO, LOG_PREFIX "modèle chargé (%s)", want_dml ? "GPU DirectML" : "CPU");
				} catch (const std::exception &e) {
					blog(LOG_ERROR, LOG_PREFIX "échec du chargement du modèle (%s) : %s",
					     want_dml ? "DirectML" : "CPU", e.what());
					clf_failed = true;
					clf_failed_dml = want_dml;
					if (want_dml) {
						// Repli automatique sur le CPU.
						try {
							clf = std::make_unique<Classifier>(f->model_path, false);
							clf_dml = false;
							clf_failed = false;
							blog(LOG_WARNING, LOG_PREFIX "repli sur le CPU");
						} catch (const std::exception &e2) {
							blog(LOG_ERROR, LOG_PREFIX "échec du repli CPU : %s", e2.what());
						}
					}
				}
			}
			if (!clf) {
				f->status = STATUS_MODEL_ERROR;
				continue;
			}
			f->ready = true;

			try {
				for (int v = 0; v < views; v++) {
					float ms = 0.0f;
					if (!clf->Run(buf.data() + static_cast<size_t>(v) * plane, side * 4, probs[v], ms))
						throw std::runtime_error("inférence sans résultat");
					infer_ms += ms;
				}
			} catch (const std::exception &e) {
				log_infer_error(e.what());
				continue;
			}
			f->status = clf_dml ? STATUS_READY_GPU : STATUS_READY_CPU;
		} else {
			if (!det || det_engine != engine || det_dml != want_dml) {
				det.reset();
				f->ready = false;
				f->status = STATUS_LOADING;
				const char *name = engine == ENGINE_NUDENET_320N ? "NudeNet 320n" : "NudeNet 640m";
				if (det_path.empty()) {
					blog(LOG_ERROR,
					     LOG_PREFIX "%s : modèle non installé (réglages du filtre > Modèles NudeNet) ; repli sur le classifieur MobileNet",
					     name);
					f->det_failed = true;
					continue;
				}
				try {
					det = std::make_unique<Detector>(det_path, want_dml, side);
					det_dml = want_dml;
					det_engine = engine;
					blog(LOG_INFO, LOG_PREFIX "%s chargé (%s)", name, want_dml ? "GPU DirectML" : "CPU");
				} catch (const std::exception &e) {
					blog(LOG_ERROR, LOG_PREFIX "échec du chargement de %s (%s) : %s", name,
					     want_dml ? "DirectML" : "CPU", e.what());
					if (want_dml) {
						try {
							det = std::make_unique<Detector>(det_path, false, side);
							det_dml = false;
							det_engine = engine;
							blog(LOG_WARNING, LOG_PREFIX "repli sur le CPU");
						} catch (const std::exception &e2) {
							blog(LOG_ERROR, LOG_PREFIX "échec du repli CPU : %s", e2.what());
						}
					}
					if (!det) {
						blog(LOG_ERROR, LOG_PREFIX "repli sur le classifieur MobileNet");
						f->det_failed = true;
						continue;
					}
				}
			}
			f->ready = true;

			try {
				for (int v = 0; v < views; v++) {
					float ms = 0.0f;
					if (!det->Run(buf.data() + static_cast<size_t>(v) * plane, side * 4, kDetFloor, view_dets,
						      ms))
						throw std::runtime_error("inférence sans résultat");
					infer_ms += ms;
					for (Detection d : view_dets) {
						d.view = v;
						dets.push_back(d);
					}
				}
			} catch (const std::exception &e) {
				log_infer_error(e.what());
				continue;
			}
			f->status = det_dml ? STATUS_READY_GPU : STATUS_READY_CPU;
		}

		const uint64_t now = os_gettime_ns();
		std::lock_guard<std::mutex> lk(f->res_m);
		f->has_result = true;
		f->res_frame = frame;
		f->res_views = views;
		f->res_engine = engine;
		f->res_side = side;
		std::copy(rects, rects + kMaxViews, f->res_rect);
		f->res_zone_view = zone_view;
		f->res_zone_x = zone_x;
		f->res_zone_y = zone_y;
		if (engine == ENGINE_MOBILENET)
			std::memcpy(f->res_probs, probs, sizeof(float) * kNumClasses * static_cast<size_t>(views));
		f->res_dets = dets;
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

// Chemin (UTF-16) d'un fichier des données du plugin ; vide s'il n'existe pas.
static std::wstring module_file_wide(const char *name)
{
	std::wstring result;
	char *path = obs_module_file(name);
	if (path) {
		wchar_t *wide = nullptr;
		os_utf8_to_wcs_ptr(path, 0, &wide);
		if (wide) {
			result = wide;
			bfree(wide);
		}
		bfree(path);
	}
	return result;
}

static std::wstring widen_ascii(const char *s)
{
	return std::wstring(s, s + std::strlen(s));
}

// Dossier des modèles NudeNet téléchargés ou importés par l'utilisateur : dans son profil
// OBS (plugin_config), donc sans droits d'administrateur. Il est créé si `create`.
static std::wstring models_dir_wide(bool create)
{
	std::wstring result;
	char *path = obs_module_config_path("models");
	if (!path)
		return result;
	if (create)
		os_mkdirs(path);
	wchar_t *wide = nullptr;
	os_utf8_to_wcs_ptr(path, 0, &wide);
	if (wide) {
		result = wide;
		bfree(wide);
	}
	bfree(path);
	return result;
}

// Emplacement du modèle `index` : dossier de l'utilisateur d'abord, puis données du
// plugin (installation manuelle). Vide s'il est introuvable.
static std::wstring find_det_model(int index)
{
	const ModelInfo &mi = model_info(index);
	const std::wstring dir = models_dir_wide(false);
	if (!dir.empty()) {
		const std::wstring p = dir + L"/" + widen_ascii(mi.file);
		if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
			return p;
	}
	return module_file_wide(mi.file);
}

// Les modèles NudeNet ne sont pas livrés avec le plugin : on relit leur emplacement à
// chaque mise à jour et dès qu'un modèle vient d'être installé, sans redémarrer OBS.
static void refresh_det_paths(nsfw_filter *f)
{
	const std::wstring p320 = find_det_model(0);
	const std::wstring p640 = find_det_model(1);
	{
		std::lock_guard<std::mutex> lk(f->job_m);
		f->det_path[0] = p320;
		f->det_path[1] = p640;
	}
	f->det_failed = false;
}

static void filter_update(void *data, obs_data_t *s)
{
	auto *f = static_cast<nsfw_filter *>(data);

	refresh_det_paths(f);
	f->dl_accept = obs_data_get_bool(s, S_DL_ACCEPT);

	f->engine = std::clamp(static_cast<int>(obs_data_get_int(s, S_ENGINE)), 0, static_cast<int>(ENGINE_NUDENET_640M));
	f->det_threshold = static_cast<float>(obs_data_get_int(s, S_DET_THRESHOLD)) / 100.0f;
	f->localized = obs_data_get_bool(s, S_LOCALIZED);
	f->region_margin = static_cast<float>(obs_data_get_int(s, S_REGION_MARGIN)) / 100.0f;
	uint32_t mask = 0;
	for (int c = 0; c < kDetNumClasses; c++) {
		const std::string key = std::string(S_CLASS_PREFIX) + std::to_string(c);
		if (obs_data_get_bool(s, key.c_str()))
			mask |= 1u << c;
	}
	f->class_mask = mask;

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
	f->model_path = module_file_wide("nsfw_model.onnx");
	if (f->model_path.empty())
		blog(LOG_ERROR, LOG_PREFIX "nsfw_model.onnx introuvable dans les données du plugin");

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

// Résultat du dernier import de fichier : -2 aucun, -1 refusé, sinon index du modèle.
static std::atomic<int> g_last_import{-2};

// Texte d'état affiché dans le groupe « Modèles NudeNet ».
static std::string models_status_text()
{
	std::string t;
	for (int i = 0; i < kModelCount; i++) {
		if (!t.empty())
			t += "\n";
		t += std::string(model_info(i).label) + " : ";
		t += obs_module_text(find_det_model(i).empty() ? "Models.NotInstalled" : "Models.Installed");
	}

	const DlStatus st = download_status();
	if (st.state == DlState::Downloading && st.model >= 0) {
		const int pct = st.total ? static_cast<int>(st.done * 100 / st.total) : 0;
		t += "\n" + std::string(model_info(st.model).label) + " : " + obs_module_text("Dl.Progress") + " " +
		     std::to_string(pct) + " %";
	} else if (st.state == DlState::Error && st.model >= 0) {
		t += "\n" + std::string(model_info(st.model).label) + " : " +
		     obs_module_text(("Dl.Err." + std::to_string(static_cast<int>(st.error))).c_str());
		if (st.error == DlError::HttpStatus)
			t += " (" + std::to_string(st.http_status) + ")";
	} else if (st.state == DlState::Done && st.model >= 0) {
		t += "\n" + std::string(model_info(st.model).label) + " : " + obs_module_text("Dl.Done");
	}

	const int imp = g_last_import.load();
	if (imp >= 0)
		t += "\n" + std::string(obs_module_text("Dl.ImportOk")) + " " + model_info(imp).label;
	else if (imp == -1)
		t += "\n" + std::string(obs_module_text("Dl.ImportFail"));
	return t;
}

// Texte d'état affiché en tête des réglages : moteur, erreurs éventuelles, délai vidéo.
static std::string filter_status_text(const nsfw_filter *f)
{
	static const char *const keys[] = {"Status.Loading",      "Status.ReadyCpu",   "Status.ReadyGpu",
					   "Status.RuntimeError", "Status.ModelError", "Status.InferError"};
	const int st = std::clamp(f->status.load(), 0, static_cast<int>(STATUS_INFER_ERROR));
	std::string t = obs_module_text(keys[st]);
	if (f->engine.load() != ENGINE_MOBILENET && f->det_failed.load())
		t += "\n" + std::string(obs_module_text("Status.DetFallback"));

	const int delay = f->cur_delay.load();
	const int ms = static_cast<int>(std::lround(static_cast<float>(delay) * frame_ms()));
	t += "\n" + std::string(obs_module_text("Status.Delay")) + " " + std::to_string(delay) + " " +
	     obs_module_text("Status.Frames") + " (" + std::to_string(ms) + " ms)";
	return t;
}

// Texte d'état du groupe « Mises à jour ».
static std::string update_status_text()
{
	const UpdStatus u = updater_status();
	std::string t = std::string(obs_module_text("Upd.Installed")) + " " + u.current;
	switch (u.state) {
	case UpdState::Idle:
		break;
	case UpdState::Checking:
		t += "\n" + std::string(obs_module_text("Upd.Checking"));
		break;
	case UpdState::UpToDate:
		t += "\n" + std::string(obs_module_text("Upd.UpToDate"));
		break;
	case UpdState::Available:
		t += "\n" + std::string(obs_module_text("Upd.Available")) + " " + u.latest;
		break;
	case UpdState::Downloading:
		t += "\n" + std::string(obs_module_text("Upd.Downloading")) + " " + u.latest + " : " +
		     std::to_string(u.percent) + " %";
		break;
	case UpdState::Ready:
		t += "\n" + std::string(obs_module_text("Upd.Ready")) + " " + u.latest + ". " +
		     obs_module_text("Upd.ReadyNote");
		break;
	case UpdState::Error:
		t += "\n" + std::string(obs_module_text("Upd.Error"));
		break;
	}
	return t;
}

static bool upd_check_clicked(obs_properties_t *, obs_property_t *, void *)
{
	updater_check_now();
	return true;
}

static bool upd_download_clicked(obs_properties_t *, obs_property_t *, void *)
{
	updater_download();
	return true;
}

static bool upd_cancel_clicked(obs_properties_t *, obs_property_t *, void *)
{
	updater_cancel_pending();
	return true;
}

static bool upd_page_clicked(obs_properties_t *, obs_property_t *, void *)
{
	const UpdStatus u = updater_status();
	const std::string url = u.page_url.empty() ? "https://github.com/helldog136/obs-nsfw-guard/releases/latest"
						   : u.page_url;
	if (url.rfind("https://github.com/", 0) == 0) // jamais autre chose qu'une page GitHub
		ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return false;
}

static bool upd_auto_toggled(obs_properties_t *, obs_property_t *, obs_data_t *s)
{
	updater_set_auto_check(obs_data_get_bool(s, S_UPD_AUTO));
	return false;
}

static bool start_download(void *priv, int index)
{
	auto *f = static_cast<nsfw_filter *>(priv);
	if (!f->dl_accept.load())
		return false;
	if (!start_model_download(index, models_dir_wide(true)))
		blog(LOG_WARNING, LOG_PREFIX "téléchargement non démarré (déjà en cours ?)");
	else
		blog(LOG_INFO, LOG_PREFIX "téléchargement de %s depuis le dépôt NudeNet", model_info(index).label);
	return true; // réaffiche les réglages avec l'état à jour
}

static bool download_320n_clicked(obs_properties_t *, obs_property_t *, void *priv)
{
	return start_download(priv, 0);
}

static bool download_640m_clicked(obs_properties_t *, obs_property_t *, void *priv)
{
	return start_download(priv, 1);
}

static bool refresh_clicked(obs_properties_t *, obs_property_t *, void *)
{
	return true; // le réaffichage recalcule le texte d'état
}

static bool open_page_clicked(obs_properties_t *, obs_property_t *, void *)
{
	ShellExecuteW(nullptr, L"open", NUDENET_RELEASE_URL, nullptr, nullptr, SW_SHOWNORMAL);
	return false;
}

static bool open_dir_clicked(obs_properties_t *, obs_property_t *, void *)
{
	const std::wstring dir = models_dir_wide(true);
	if (!dir.empty())
		ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return false;
}

// Un fichier a été choisi : on vérifie qu'il s'agit bien d'un modèle NudeNet attendu
// (taille et empreinte SHA-256) avant de le copier dans le dossier des modèles.
static bool import_selected(obs_properties_t *, obs_property_t *, obs_data_t *s)
{
	const char *path = obs_data_get_string(s, S_IMPORT_PATH);
	if (!path || !*path)
		return false;

	wchar_t *wide = nullptr;
	os_utf8_to_wcs_ptr(path, 0, &wide);
	int index = -1;
	if (wide) {
		index = import_model_file(wide, models_dir_wide(true));
		bfree(wide);
	}
	g_last_import = index;
	if (index >= 0)
		blog(LOG_INFO, LOG_PREFIX "%s importé", model_info(index).label);
	else
		blog(LOG_WARNING, LOG_PREFIX "fichier refusé : ce n'est pas un modèle NudeNet attendu (%s)", path);

	obs_data_set_string(s, S_IMPORT_PATH, ""); // permet de rechoisir le même fichier
	return true;
}

// Les boutons de téléchargement ne sont proposés qu'une fois la licence acceptée.
static bool accept_toggled(obs_properties_t *props, obs_property_t *, obs_data_t *s)
{
	const bool ok = obs_data_get_bool(s, S_DL_ACCEPT);
	obs_property_set_visible(obs_properties_get(props, "dl_320n"), ok);
	obs_property_set_visible(obs_properties_get(props, "dl_640m"), ok);
	return true;
}

// Affiche ou masque les réglages selon le moteur (classifieur ou détecteur NudeNet).
static bool engine_toggled(obs_properties_t *props, obs_property_t *, obs_data_t *s)
{
	const int engine = static_cast<int>(obs_data_get_int(s, S_ENGINE));
	const bool det = engine != ENGINE_MOBILENET;
	obs_property_set_visible(obs_properties_get(props, S_THRESHOLD), !det);
	obs_property_set_visible(obs_properties_get(props, S_SEXY_WEIGHT), !det);
	obs_property_set_visible(obs_properties_get(props, S_DET_THRESHOLD), det);
	obs_property_set_visible(obs_properties_get(props, S_CLASSES_GROUP), det);
	obs_property_set_visible(obs_properties_get(props, S_MODELS_GROUP), det);
	obs_property_set_visible(obs_properties_get(props, S_LOCALIZED), det);
	obs_property_set_visible(obs_properties_get(props, S_REGION_MARGIN),
				 det && obs_data_get_bool(s, S_LOCALIZED));
	// Avertissement : le gros modèle et/ou le GPU (DirectML) sont déconseillés (voir les textes).
	obs_property_set_visible(obs_properties_get(props, S_ENGINE_WARNING),
				 engine == ENGINE_NUDENET_640M || obs_data_get_bool(s, S_USE_DML));
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

static obs_properties_t *filter_properties(void *data)
{
	auto *f = static_cast<nsfw_filter *>(data);
	obs_properties_t *p = obs_properties_create();

	// Le texte d'un champ d'information est la valeur du réglage : on y écrit l'état à jour.
	if (f && f->self) {
		obs_data_t *st = obs_source_get_settings(f->self);
		obs_data_set_string(st, S_MODELS_STATUS, models_status_text().c_str());
		obs_data_set_string(st, S_STATUS, filter_status_text(f).c_str());
		obs_data_set_string(st, S_UPD_STATUS, update_status_text().c_str());
		obs_data_set_bool(st, S_UPD_AUTO, updater_auto_check());
		obs_data_release(st);
	}

	obs_property_t *status = obs_properties_add_text(p, S_STATUS, obs_module_text("Status"), OBS_TEXT_INFO);
	obs_property_set_long_description(status, obs_module_text("Status.Desc"));
	obs_properties_add_button2(p, "status_refresh", obs_module_text("Status.Refresh"), refresh_clicked, f);

	obs_property_t *prop = obs_properties_add_list(p, S_ENGINE, obs_module_text("Engine"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(prop, obs_module_text("Engine.Desc"));
	obs_property_list_add_int(prop, obs_module_text("Engine.MobileNet"), ENGINE_MOBILENET);
	obs_property_list_add_int(prop, obs_module_text("Engine.NudeNet320n"), ENGINE_NUDENET_320N);
	obs_property_list_add_int(prop, obs_module_text("Engine.NudeNet640m"), ENGINE_NUDENET_640M);
	obs_property_set_modified_callback(prop, engine_toggled);

	// Le texte affiché d'un champ d'information est la valeur du réglage (voir les défauts).
	prop = obs_properties_add_text(p, S_ENGINE_WARNING, obs_module_text("Engine.WarningLabel"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(prop, OBS_TEXT_INFO_WARNING);

	prop = obs_properties_add_int_slider(p, S_THRESHOLD, obs_module_text("Threshold"), 1, 100, 1);
	obs_property_set_long_description(prop, obs_module_text("Threshold.Desc"));

	prop = obs_properties_add_int_slider(p, S_SEXY_WEIGHT, obs_module_text("SexyWeight"), 0, 100, 1);
	obs_property_set_long_description(prop, obs_module_text("SexyWeight.Desc"));

	prop = obs_properties_add_int_slider(p, S_DET_THRESHOLD, obs_module_text("DetThreshold"), 15, 100, 1);
	obs_property_set_long_description(prop, obs_module_text("DetThreshold.Desc"));

	// Modèles NudeNet : optionnels, non distribués avec le plugin. Deux voies : téléchargement
	// direct depuis le dépôt officiel (après acceptation), ou import d'un fichier téléchargé.
	obs_properties_t *models = obs_properties_create();
	obs_properties_add_text(models, S_MODELS_STATUS, obs_module_text("Models.StatusLabel"), OBS_TEXT_INFO);
	prop = obs_properties_add_bool(models, S_DL_ACCEPT, obs_module_text("Dl.Accept"));
	obs_property_set_long_description(prop, obs_module_text("Dl.Accept.Desc"));
	obs_property_set_modified_callback(prop, accept_toggled);
	obs_properties_add_button2(models, "dl_320n", obs_module_text("Dl.Button.320n"), download_320n_clicked, f);
	obs_properties_add_button2(models, "dl_640m", obs_module_text("Dl.Button.640m"), download_640m_clicked, f);
	obs_properties_add_button2(models, "dl_refresh", obs_module_text("Dl.Refresh"), refresh_clicked, f);
	obs_properties_add_button2(models, "dl_open_page", obs_module_text("Dl.OpenPage"), open_page_clicked, f);
	prop = obs_properties_add_path(models, S_IMPORT_PATH, obs_module_text("Dl.Import"), OBS_PATH_FILE,
				       "NudeNet (*.onnx)", nullptr);
	obs_property_set_long_description(prop, obs_module_text("Dl.Import.Desc"));
	obs_property_set_modified_callback(prop, import_selected);
	obs_properties_add_button2(models, "dl_open_dir", obs_module_text("Dl.OpenDir"), open_dir_clicked, f);
	obs_properties_add_group(p, S_MODELS_GROUP, obs_module_text("Models"), OBS_GROUP_NORMAL, models);

	obs_properties_t *classes = obs_properties_create();
	for (int c = 0; c < kDetNumClasses; c++) {
		const std::string key = std::string(S_CLASS_PREFIX) + std::to_string(c);
		const std::string label = "Class." + std::to_string(c);
		obs_properties_add_bool(classes, key.c_str(), obs_module_text(label.c_str()));
	}
	obs_properties_add_group(p, S_CLASSES_GROUP, obs_module_text("Classes"), OBS_GROUP_NORMAL, classes);

	prop = obs_properties_add_bool(p, S_LOCALIZED, obs_module_text("Localized"));
	obs_property_set_long_description(prop, obs_module_text("Localized.Desc"));
	obs_property_set_modified_callback(prop, engine_toggled);
	prop = obs_properties_add_int_slider(p, S_REGION_MARGIN, obs_module_text("RegionMargin"), 0, 100, 5);
	obs_property_set_long_description(prop, obs_module_text("RegionMargin.Desc"));

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
	obs_property_set_modified_callback(prop, engine_toggled);
	prop = obs_properties_add_bool(p, S_BLOCK_UNTIL_READY, obs_module_text("BlockUntilReady"));
	obs_property_set_long_description(prop, obs_module_text("BlockUntilReady.Desc"));
	obs_properties_add_bool(p, S_DEBUG, obs_module_text("Debug"));

	// Mises à jour du plugin (réglage global, commun à tous les filtres).
	{
		const UpdStatus u = updater_status();
		obs_properties_t *upd = obs_properties_create();
		obs_properties_add_text(upd, S_UPD_STATUS, obs_module_text("Upd.StatusLabel"), OBS_TEXT_INFO);
		obs_property_t *auto_prop = obs_properties_add_bool(upd, S_UPD_AUTO, obs_module_text("Upd.Auto"));
		obs_property_set_long_description(auto_prop, obs_module_text("Upd.Auto.Desc"));
		obs_property_set_modified_callback(auto_prop, upd_auto_toggled);
		obs_properties_add_button2(upd, "upd_check", obs_module_text("Upd.Check"), upd_check_clicked, f);
		obs_property_t *dl = obs_properties_add_button2(upd, "upd_download", obs_module_text("Upd.Download"),
								upd_download_clicked, f);
		obs_property_set_long_description(dl, obs_module_text("Upd.Download.Desc"));
		obs_property_set_visible(dl, u.state == UpdState::Available ||
						     (u.state == UpdState::Error && !u.latest.empty() &&
						      u.latest != u.current));
		obs_property_t *cancel = obs_properties_add_button2(upd, "upd_cancel", obs_module_text("Upd.Cancel"),
								    upd_cancel_clicked, f);
		obs_property_set_visible(cancel, u.state == UpdState::Ready);
		obs_properties_add_button2(upd, "upd_page", obs_module_text("Upd.Page"), upd_page_clicked, f);
		obs_properties_add_group(p, S_UPD_GROUP, obs_module_text("Upd"), OBS_GROUP_NORMAL, upd);
	}

	return p;
}

static void filter_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, S_ENGINE, ENGINE_MOBILENET);
	obs_data_set_default_string(s, S_ENGINE_WARNING, obs_module_text("Engine.Warning"));
	obs_data_set_default_bool(s, S_DL_ACCEPT, false);
	obs_data_set_default_int(s, S_DET_THRESHOLD, 40);
	obs_data_set_default_bool(s, S_LOCALIZED, true);
	obs_data_set_default_int(s, S_REGION_MARGIN, 25);
	for (int c = 0; c < kDetNumClasses; c++) {
		const std::string key = std::string(S_CLASS_PREFIX) + std::to_string(c);
		obs_data_set_default_bool(s, key.c_str(), class_default_on(c));
	}
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
	auto *f = static_cast<nsfw_filter *>(data);
	f->new_tick = true;

	// Un modèle NudeNet vient d'être téléchargé ou importé : on le prend en compte tout de suite.
	const uint32_t gen = models_generation();
	if (gen != f->seen_gen) {
		f->seen_gen = gen;
		refresh_det_paths(f);
	}
}

static float frame_ms()
{
	obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0)
		return 1000.0f * static_cast<float>(ovi.fps_den) / static_cast<float>(ovi.fps_num);
	return 16.7f;
}

static const char *const kClassNames[kDetNumClasses] = {
	"FEMALE_GENITALIA_COVERED", "FACE_FEMALE",        "BUTTOCKS_EXPOSED",     "FEMALE_BREAST_EXPOSED",
	"FEMALE_GENITALIA_EXPOSED", "MALE_BREAST_EXPOSED", "ANUS_EXPOSED",         "FEET_EXPOSED",
	"BELLY_COVERED",            "FEET_COVERED",        "ARMPITS_COVERED",      "ARMPITS_EXPOSED",
	"FACE_MALE",                "BELLY_EXPOSED",       "MALE_GENITALIA_EXPOSED", "ANUS_COVERED",
	"FEMALE_BREAST_COVERED",    "BUTTOCKS_COVERED",
};

static float rect_iou(const ViewRect &a, const ViewRect &b)
{
	const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
	const float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
	const float inter = std::max(0.0f, x1 - x0) * std::max(0.0f, y1 - y0);
	const float uni = a.w * a.h + b.w * b.h - inter;
	return uni > 0.0f ? inter / uni : 0.0f;
}

// Ajoute (ou met à jour) la zone masquée correspondant à une détection. `r` est la
// zone de la source qui a été envoyée au modèle : l'image y est collée en haut à
// gauche d'un carré (le reste est du noir), à l'échelle max(r.w, r.h) / side.
static void add_region(nsfw_filter *f, const Detection &d, const ViewRect &r, int side, uint64_t start_id,
		       uint64_t now_ns)
{
	const float scale = std::max(r.w, r.h) / static_cast<float>(side);
	float x0 = r.x + d.x * scale, y0 = r.y + d.y * scale;
	float x1 = x0 + d.w * scale, y1 = y0 + d.h * scale;

	// Marge autour de la détection, proportionnelle à sa taille.
	const float m = f->region_margin.load() * std::max(x1 - x0, y1 - y0);
	x0 = std::clamp(x0 - m, 0.0f, static_cast<float>(f->w));
	y0 = std::clamp(y0 - m, 0.0f, static_cast<float>(f->h));
	x1 = std::clamp(x1 + m, 0.0f, static_cast<float>(f->w));
	y1 = std::clamp(y1 + m, 0.0f, static_cast<float>(f->h));
	if (x1 - x0 < 1.0f || y1 - y0 < 1.0f)
		return;

	const ViewRect nr = {x0, y0, x1 - x0, y1 - y0};
	for (Region &e : f->regions) {
		if (rect_iou({e.x, e.y, e.w, e.h}, nr) > 0.3f) {
			// Même objet qu'à l'analyse précédente : la zone suit son déplacement.
			e.x = nr.x;
			e.y = nr.y;
			e.w = nr.w;
			e.h = nr.h;
			e.last_ns = now_ns;
			return;
		}
	}
	if (static_cast<int>(f->regions.size()) < kMaxRegions)
		f->regions.push_back({nr.x, nr.y, nr.w, nr.h, start_id, now_ns});
}

static void consume_result(nsfw_filter *f)
{
	float probs[kMaxViews][kNumClasses];
	std::vector<Detection> dets;
	ViewRect rects[kMaxViews];
	uint64_t frame;
	int views, zone_view, engine, side;
	float zone_x, zone_y;
	float lat_ms, infer_ms;
	{
		std::lock_guard<std::mutex> lk(f->res_m);
		if (!f->has_result)
			return;
		f->has_result = false;
		frame = f->res_frame;
		views = f->res_views;
		engine = f->res_engine;
		side = f->res_side;
		std::copy(f->res_rect, f->res_rect + kMaxViews, rects);
		zone_view = f->res_zone_view;
		zone_x = f->res_zone_x;
		zone_y = f->res_zone_y;
		lat_ms = f->res_lat_ms;
		infer_ms = f->res_infer_ms;
		if (engine == ENGINE_MOBILENET)
			std::memcpy(probs, f->res_probs, sizeof(float) * kNumClasses * static_cast<size_t>(views));
		else
			dets = f->res_dets;
	}
	f->pending_since_ns = 0;

	f->lat_ms_ema = 0.9f * f->lat_ms_ema + 0.1f * lat_ms;

	const bool det_engine = engine != ENGINE_MOBILENET;
	const float threshold = det_engine ? f->det_threshold.load() : f->threshold.load();
	const uint32_t class_mask = f->class_mask.load();

	// Score de chaque vue : classifieur = formule porn + hentai + poids x sexy ;
	// détecteur = meilleure confiance parmi les classes qui déclenchent le masque.
	float view_score[kMaxViews] = {};
	if (!det_engine) {
		const float sexy_weight = f->sexy_weight.load();
		for (int v = 0; v < views; v++)
			view_score[v] = std::min(1.0f, probs[v][kClassPorn] + probs[v][kClassHentai] +
							       sexy_weight * probs[v][kClassSexy]);
	} else {
		for (const Detection &d : dets)
			if (d.view >= 0 && d.view < views && ((class_mask >> d.cls) & 1u))
				view_score[d.view] = std::max(view_score[d.view], d.score);
	}

	// L'image est sensible si une de ses vues l'est.
	float score = 0.0f;
	int best_view = 0;
	for (int v = 0; v < views; v++) {
		if (view_score[v] > score) {
			score = view_score[v];
			best_view = v;
		}
	}
	const float zone_score = zone_view >= 0 && zone_view < views ? view_score[zone_view] : 0.0f;
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
			const uint64_t back = static_cast<uint64_t>(f->consecutive.load() - 1) *
					      static_cast<uint64_t>(f->interval.load());
			const uint64_t start_id = frame > back ? frame - back : 0;

			if (det_engine && f->localized.load()) {
				// Mode localisé : on ne masque que les cadres détectés.
				const bool had_regions = !f->regions.empty();
				for (const Detection &d : dets) {
					if (d.view < 0 || d.view >= views || !((class_mask >> d.cls) & 1u) ||
					    d.score < threshold)
						continue;
					add_region(f, d, rects[d.view], side, start_id, now);
					if (!had_regions)
						blog(LOG_INFO, LOG_PREFIX "détection %s (confiance %.2f) : zone masquée",
						     kClassNames[d.cls], d.score);
				}
			} else if (!f->mask_active) {
				f->mask_active = true;
				f->mask_start_id = start_id;
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
			if (det_engine)
				blog(LOG_INFO,
				     LOG_PREFIX "NudeNet : %zu détection(s), meilleur score retenu max=%.2f dernier=%.2f (vue %d/%d) | inférence=%.2f ms  latence=%.2f ms | délai=%d img | zones=%zu masque=%s%s",
				     dets.size(), f->dbg_max_score, score, best_view, views, infer_ms, f->lat_ms_ema,
				     f->cur_delay.load(), f->regions.size(), f->mask_active ? "OUI" : "non",
				     f->zone_sticky ? " | zone verrouillée" : "");
			else
				blog(LOG_INFO,
				     LOG_PREFIX "score max=%.2f  dernier=%.2f (vue %d/%d) | inférence=%.2f ms  latence=%.2f ms | délai=%d img | masque=%s%s",
				     f->dbg_max_score, score, best_view, views, infer_ms, f->lat_ms_ema, f->cur_delay.load(),
				     f->mask_active ? "OUI" : "non", f->zone_sticky ? " | zone verrouillée" : "");
			if (det_engine) {
				// Détail des détections (toutes classes) pour calibrer les réglages.
				for (const Detection &d : dets)
					blog(LOG_INFO, LOG_PREFIX "  vue %d : %s %.2f%s", d.view, kClassNames[d.cls], d.score,
					     ((class_mask >> d.cls) & 1u) ? " [déclenche]" : "");
			}
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
		f->regions.clear(); // coordonnées de l'ancienne résolution
		f->w = w;
		f->h = h;
	}

	// Taille du tampon par paliers de 8 pour éviter de le réallouer sans cesse.
	size_t want = static_cast<size_t>(((delay + 2) + 7) / 8) * 8;
	want = std::max<size_t>(want, 8);
	if (want > f->ring.size()) {
		// Les images en tampon sont rangées à l'emplacement id % taille : on les déplace à
		// leur nouvel emplacement au lieu de les jeter (sinon la source disparaîtrait le temps
		// de remplir à nouveau le tampon). Les ids présents couvrent moins de `want` images
		// consécutives, donc deux images ne peuvent pas viser le même emplacement.
		std::vector<gs_texrender_t *> ring(want, nullptr);
		std::vector<uint64_t> ids(want, UINT64_MAX);
		std::vector<gs_texrender_t *> spare;
		for (size_t i = 0; i < f->ring.size(); i++) {
			const uint64_t id = f->slot_id[i];
			const size_t j = id == UINT64_MAX ? 0 : static_cast<size_t>(id % want);
			if (id != UINT64_MAX && f->ring[i] && !ring[j]) {
				ring[j] = f->ring[i];
				ids[j] = id;
			} else if (f->ring[i]) {
				spare.push_back(f->ring[i]);
			}
		}
		for (auto &t : ring) {
			if (t)
				continue;
			if (!spare.empty()) {
				t = spare.back();
				spare.pop_back();
			} else {
				t = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			}
		}
		for (auto *t : spare)
			gs_texrender_destroy(t);
		f->ring.swap(ring);
		f->slot_id.swap(ids);
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

// Moteur réellement utilisé : le détecteur est abandonné (repli sur MobileNet) si son
// modèle est introuvable ou n'a pas pu être chargé.
static int effective_engine(const nsfw_filter *f)
{
	const int engine = f->engine.load();
	return engine != ENGINE_MOBILENET && f->det_failed.load() ? ENGINE_MOBILENET : engine;
}

struct ViewPlan {
	ViewRect rect[kMaxViews];
	int count = 0;
	int zone_view = -1;    // index de la zone aléatoire dans rect[], -1 si aucune
	int engine = ENGINE_MOBILENET;
	int side = kInputSize; // côté du carré envoyé au modèle
	// NudeNet reçoit chaque vue sans déformation (collée en haut à gauche d'un carré
	// noir) ; le classifieur reçoit la vue étirée à la taille du carré.
	bool letterbox = false;
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
	plan.engine = effective_engine(f);
	plan.side = engine_side(plan.engine);
	plan.letterbox = plan.engine != ENGINE_MOBILENET;
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

// Niveau de la pyramide à utiliser pour une vue : le plus réduit qui garde encore
// au moins `side` px (on ne sur-échantillonne jamais). Vue étirée : il faut `side`
// px sur chaque côté ; vue sans déformation : sur le plus grand côté.
static int pyramid_level_for(const ViewRect &r, int side, bool letterbox)
{
	const float extent = letterbox ? std::max(r.w, r.h) : std::min(r.w, r.h);
	int k = 0;
	while (k + 1 < kMaxPyramid && extent / static_cast<float>(1u << (k + 1)) >= static_cast<float>(side))
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

// Dessine une vue dans small_tr (carré de `side` px). La zone à lire est choisie par la
// projection (gs_ortho) : elle est mise à l'échelle du viewport.
//  - letterbox faux : la zone est étirée pour remplir tout le carré ;
//  - letterbox vrai : la zone est collée en haut à gauche, sans déformation, à l'échelle
//    max(w, h) / side ; le reste du carré est noir (comme le fait NudeNet).
static bool render_view(nsfw_filter *f, gs_texture_t *full, const ViewRect &r, int side, bool letterbox)
{
	const uint32_t sz = static_cast<uint32_t>(side);
	const int level = pyramid_level_for(r, side, letterbox);
	gs_texture_t *src = pyramid_texture(f, full, level);
	if (!src)
		return false;

	const uint32_t lw = std::max<uint32_t>(1, f->w >> level);
	const uint32_t lh = std::max<uint32_t>(1, f->h >> level);
	const float sx = static_cast<float>(lw) / static_cast<float>(f->w);
	const float sy = static_cast<float>(lh) / static_cast<float>(f->h);
	const float ext_w = letterbox ? std::max(r.w, r.h) : r.w;
	const float ext_h = letterbox ? std::max(r.w, r.h) : r.h;
	const float x0 = r.x * sx, y0 = r.y * sy;
	const float x1 = x0 + ext_w * sx, y1 = y0 + ext_h * sy;

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
	const ViewPlan plan = plan_views(f);
	const uint32_t sz = static_cast<uint32_t>(plan.side);

	// Changement de taille de vue (autre moteur) : les surfaces de rapatriement sont
	// à refaire, et celles en attente ne correspondent plus.
	if (f->surf_side != plan.side) {
		for (auto &set : f->stage) {
			for (auto *&s : set) {
				if (s)
					gs_stagesurface_destroy(s);
				s = nullptr;
			}
		}
		f->stage_valid[0] = f->stage_valid[1] = false;
		f->surf_side = plan.side;
	}

	// Niveau de pyramide le plus profond dont une vue a besoin.
	int max_level = 0;
	for (int v = 0; v < plan.count; v++)
		max_level = std::max(max_level, pyramid_level_for(plan.rect[v], plan.side, plan.letterbox));
	build_pyramid(f, full, max_level);

	const int cur = f->stage_idx;
	const int prev = 1 - cur;

	int staged = 0;
	for (int v = 0; v < plan.count; v++) {
		if (!render_view(f, full, plan.rect[v], plan.side, plan.letterbox))
			break;
		if (!f->stage[cur][v])
			f->stage[cur][v] = gs_stagesurface_create(sz, sz, GS_RGBA);
		gs_stage_texture(f->stage[cur][v], gs_texrender_get_texture(f->small_tr));
		f->stage_rect[cur][v] = plan.rect[v];
		staged++;
	}
	f->stage_valid[cur] = staged > 0;
	f->stage_views[cur] = staged;
	f->stage_side[cur] = plan.side;
	f->stage_engine[cur] = plan.engine;
	f->stage_frame[cur] = f->frame_id;
	// La zone n'est utilisable que si sa vue a bien été préparée.
	f->stage_zone_view[cur] = plan.zone_view >= 0 && plan.zone_view < staged ? plan.zone_view : -1;
	f->stage_zone_x[cur] = plan.zone_view >= 0 ? plan.rect[plan.zone_view].x : 0.0f;
	f->stage_zone_y[cur] = plan.zone_view >= 0 ? plan.rect[plan.zone_view].y : 0.0f;

	if (f->stage_valid[prev]) {
		const int pviews = f->stage_views[prev];
		const size_t pplane = static_cast<size_t>(f->stage_side[prev]) * f->stage_side[prev] * 4;
		if (f->map_buf.size() < pplane * static_cast<size_t>(pviews))
			f->map_buf.resize(pplane * static_cast<size_t>(pviews));

		int copied = 0;
		for (int v = 0; v < pviews; v++) {
			uint8_t *data = nullptr;
			uint32_t linesize = 0;
			if (!f->stage[prev][v] || !gs_stagesurface_map(f->stage[prev][v], &data, &linesize))
				break;
			const uint32_t psz = static_cast<uint32_t>(f->stage_side[prev]);
			uint8_t *dst = f->map_buf.data() + static_cast<size_t>(v) * pplane;
			for (uint32_t y = 0; y < psz; y++)
				std::memcpy(dst + static_cast<size_t>(y) * psz * 4,
					    data + static_cast<size_t>(y) * linesize, static_cast<size_t>(psz) * 4);
			gs_stagesurface_unmap(f->stage[prev][v]);
			copied++;
		}

		if (copied == pviews && copied > 0) {
			{
				std::lock_guard<std::mutex> lk(f->job_m);
				f->job_buf.swap(f->map_buf);
				f->job_views = copied;
				f->job_engine = f->stage_engine[prev];
				f->job_side = f->stage_side[prev];
				std::copy(f->stage_rect[prev], f->stage_rect[prev] + copied, f->job_rect);
				f->job_zone_view = f->stage_zone_view[prev];
				f->job_zone_x = f->stage_zone_x[prev];
				f->job_zone_y = f->stage_zone_y[prev];
				f->job_frame = f->stage_frame[prev];
				f->job_submit_ns = os_gettime_ns();
				f->has_job = true;
			}
			f->job_cv.notify_one();
			if (!f->pending_since_ns)
				f->pending_since_ns = os_gettime_ns();
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

// Dessine le rectangle (x, y, w, h) de la source, en pixels, avec les coordonnées de
// texture correspondantes (la texture couvre toute la source : fw x fh pixels).
static void draw_quad(float x, float y, float w, float h, float fw, float fh)
{
	const float u0 = x / fw, v0 = y / fh, u1 = (x + w) / fw, v1 = (y + h) / fh;
	gs_render_start(true);
	gs_texcoord(u0, v0, 0);
	gs_vertex2f(x, y);
	gs_texcoord(u1, v0, 0);
	gs_vertex2f(x + w, y);
	gs_texcoord(u0, v1, 0);
	gs_vertex2f(x, y + h);
	gs_texcoord(u1, v1, 0);
	gs_vertex2f(x + w, y + h);
	gs_render_stop(GS_TRISTRIP);
}

// Mode localisé : dessine par-dessus l'image déjà affichée le masque (noir, pixelisation
// ou flou) uniquement dans les zones détectées.
static void draw_regions(nsfw_filter *f, gs_texture_t *tex)
{
	std::vector<const Region *> active;
	for (const Region &r : f->regions)
		if (f->display_id >= 0 && static_cast<uint64_t>(f->display_id) >= r.start_id)
			active.push_back(&r);
	if (active.empty())
		return;

	const float fw = static_cast<float>(f->w), fh = static_cast<float>(f->h);

	if (f->mode == MASK_BLUR && f->blur_fx) {
		render_blur_passes(f, tex);
		gs_texture_t *horiz = gs_texrender_get_texture(f->blur_tr[1]);
		if (horiz) {
			vec2 texel;
			vec2_set(&texel, 0.0f, 1.0f / static_cast<float>(f->blur_h));
			gs_effect_set_texture(gs_effect_get_param_by_name(f->blur_fx, "image"), horiz);
			gs_effect_set_vec2(gs_effect_get_param_by_name(f->blur_fx, "texel"), &texel);
			gs_effect_set_float(gs_effect_get_param_by_name(f->blur_fx, "sigma"), f->blur_sigma);
			while (gs_effect_loop(f->blur_fx, "Draw"))
				for (const Region *r : active)
					draw_quad(r->x, r->y, r->w, r->h, fw, fh);
			return;
		}
	}

	if (f->mode == MASK_PIXELATE && f->pixel_fx) {
		const float px = static_cast<float>(std::max(1, f->pixel_size.load()));
		vec2 b;
		vec2_set(&b, std::max(1.0f, fw / px), std::max(1.0f, fh / px));
		gs_effect_set_texture(gs_effect_get_param_by_name(f->pixel_fx, "image"), tex);
		gs_effect_set_vec2(gs_effect_get_param_by_name(f->pixel_fx, "blocks"), &b);
		while (gs_effect_loop(f->pixel_fx, "Draw"))
			for (const Region *r : active)
				draw_quad(r->x, r->y, r->w, r->h, fw, fh);
		return;
	}

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	vec4 black;
	vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &black);
	for (const Region *r : active) {
		gs_matrix_push();
		gs_matrix_translate3f(r->x, r->y, 0.0f);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(std::ceil(r->w)),
				       static_cast<uint32_t>(std::ceil(r->h)));
		gs_matrix_pop();
	}
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

		const uint64_t now = os_gettime_ns();
		const bool resumed = f->last_render_ns == 0 || now - f->last_render_ns > kRenderGapNs;
		if (resumed && f->last_render_ns != 0) {
			// La source n'était plus rendue (scène inactive...) : les images en tampon sont
			// périmées. On ne les réaffiche pas ; le tampon se remplit à nouveau.
			std::fill(f->slot_id.begin(), f->slot_id.end(), UINT64_MAX);
			f->stage_valid[0] = f->stage_valid[1] = false;
			f->pending_since_ns = 0;
		}
		f->last_render_ns = now;

		consume_result(f);

		// Chien de garde : des analyses sont soumises mais aucun résultat ne revient.
		const uint64_t stall_ns =
			std::max(kStallNs, static_cast<uint64_t>(3.0f * f->lat_ms_ema * 1.0e6f));
		const bool stalled = f->pending_since_ns != 0 && now - f->pending_since_ns > stall_ns;
		if (stalled != f->stalled) {
			f->stalled = stalled;
			if (stalled)
				blog(LOG_WARNING, LOG_PREFIX "aucun résultat d'analyse depuis %.1f s%s",
				     static_cast<double>(now - f->pending_since_ns) / 1.0e9,
				     f->block_until_ready.load() ? " : source masquée par sécurité" : "");
			else
				blog(LOG_INFO, LOG_PREFIX "l'analyse a repris");
		}

		if (f->mask_active &&
		    now - f->last_pos_ns > static_cast<uint64_t>(f->hold_ms.load()) * 1000000ULL) {
			f->mask_active = false;
			f->streak = 0;
			blog(LOG_INFO, LOG_PREFIX "masque levé");
		}

		// Zones masquées : on garde chacune tant qu'elle a été confirmée récemment.
		if (!f->regions.empty()) {
			const uint64_t hold = static_cast<uint64_t>(f->hold_ms.load()) * 1000000ULL;
			f->regions.erase(std::remove_if(f->regions.begin(), f->regions.end(),
							[&](const Region &r) { return now - r.last_ns > hold; }),
					 f->regions.end());
			if (f->regions.empty()) {
				f->streak = 0;
				blog(LOG_INFO, LOG_PREFIX "zones masquées levées");
			}
		}

		// Délai : en automatique, il augmente d'une image à la fois (une image répétée plutôt
		// qu'un retour en arrière) et ne diminue qu'après kDelayDownNs de marge stable, pour
		// limiter les saccades et garder un décalage audio constant.
		const int want_delay = compute_delay(f);
		if (resumed || !f->delay_on.load() || !f->auto_delay.load()) {
			f->cur_delay = want_delay;
			f->delay_down_ns = 0;
		} else if (want_delay > f->cur_delay) {
			f->cur_delay++;
			f->delay_down_ns = 0;
		} else if (want_delay < f->cur_delay) {
			if (!f->delay_down_ns) {
				f->delay_down_ns = now;
			} else if (now - f->delay_down_ns > kDelayDownNs) {
				f->cur_delay--;
				f->delay_down_ns = now;
			}
		} else {
			f->delay_down_ns = 0;
		}
		ensure_gfx(f, w, h, f->cur_delay.load());

		const size_t n = f->ring.size();
		const size_t slot = static_cast<size_t>(f->frame_id % n);
		capture_target(f, target, f->ring[slot]);
		f->slot_id[slot] = f->frame_id;

		if (f->frame_id % static_cast<uint64_t>(f->interval.load()) == 0)
			analyze(f, gs_texrender_get_texture(f->ring[slot]));

		f->display_id = static_cast<int64_t>(f->frame_id) - f->cur_delay.load();
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
			    ((!f->ready.load() || f->stalled) && f->block_until_ready.load());

	if (masked) {
		draw_mask(f, tex);
		return;
	}

	gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(eff, "image"), tex);
	while (gs_effect_loop(eff, "Draw"))
		gs_draw_sprite(tex, 0, f->w, f->h);

	draw_regions(f, tex);
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
