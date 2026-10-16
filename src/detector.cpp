// SPDX-License-Identifier: GPL-2.0-or-later

#include "detector.h"

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace {

// IoU au-delà duquel deux cadres sont considérés comme le même objet.
constexpr float kNmsIou = 0.45f;
// Garde-fou : nombre maximal de candidats examinés par la suppression des doublons.
constexpr size_t kMaxCandidates = 300;

float iou(const Detection &a, const Detection &b)
{
	const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
	const float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
	const float iw = std::max(0.0f, x1 - x0), ih = std::max(0.0f, y1 - y0);
	const float inter = iw * ih;
	const float uni = a.w * a.h + b.w * b.h - inter;
	return uni > 0.0f ? inter / uni : 0.0f;
}

} // namespace

struct Detector::Impl {
	Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "nsfw-guard-det"};
	Ort::Session session{nullptr};
	Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
	std::vector<float> input;
};

Detector::Detector(const std::wstring &model_path, bool use_directml, int side)
	: impl_(std::make_unique<Impl>()), side_(side)
{
	if (side <= 0)
		throw std::invalid_argument("taille d'entrée invalide");
	impl_->input.resize(static_cast<size_t>(3) * side * side);

	Ort::SessionOptions so;
	so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

	if (use_directml) {
		so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
		so.DisableMemPattern();
		const OrtDmlApi *dml = nullptr;
		Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION,
									 reinterpret_cast<const void **>(&dml)));
		Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(so, 0));
	} else {
		so.SetIntraOpNumThreads(4);
		so.SetInterOpNumThreads(1);
		so.AddConfigEntry("session.intra_op.allow_spinning", "0");
	}

	impl_->session = Ort::Session(impl_->env, model_path.c_str(), so);
}

Detector::~Detector() = default;

bool Detector::Run(const uint8_t *rgba, uint32_t linesize, float min_score, std::vector<Detection> &out,
		   float &elapsed_ms)
{
	out.clear();
	const int S = side_;
	const size_t hw = static_cast<size_t>(S) * S;
	float *r = impl_->input.data();
	float *g = r + hw;
	float *b = g + hw;
	constexpr float k = 1.0f / 255.0f;

	// RGBA entrelacé -> RGB planaire (NCHW), valeurs dans [0,1], comme NudeNet.
	for (int y = 0; y < S; y++) {
		const uint8_t *row = rgba + static_cast<size_t>(y) * linesize;
		const size_t base = static_cast<size_t>(y) * S;
		for (int x = 0; x < S; x++) {
			const uint8_t *p = row + static_cast<size_t>(x) * 4;
			r[base + x] = p[0] * k;
			g[base + x] = p[1] * k;
			b[base + x] = p[2] * k;
		}
	}

	const int64_t shape[4] = {1, 3, S, S};
	Ort::Value tensor = Ort::Value::CreateTensor<float>(impl_->mem, impl_->input.data(), impl_->input.size(),
							    shape, 4);
	const char *in_names[] = {"images"};
	const char *out_names[] = {"output0"};

	const auto t0 = std::chrono::steady_clock::now();
	auto result = impl_->session.Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 1);
	elapsed_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();

	if (result.empty())
		return false;

	// Sortie attendue : [1, 4 + kDetNumClasses, N] ; les 4 premières lignes sont
	// (centre x, centre y, largeur, hauteur) en pixels de l'entrée, les suivantes les
	// scores de chaque classe (déjà des probabilités).
	const auto info = result[0].GetTensorTypeAndShapeInfo();
	const std::vector<int64_t> dims = info.GetShape();
	if (dims.size() != 3 || dims[0] != 1 || dims[1] != 4 + kDetNumClasses)
		throw std::runtime_error("forme de sortie inattendue pour le détecteur");
	const size_t n = static_cast<size_t>(dims[2]);
	const float *d = result[0].GetTensorData<float>();

	std::vector<Detection> cand;
	for (size_t i = 0; i < n; i++) {
		int best = 0;
		float score = d[(4 + 0) * n + i];
		for (int c = 1; c < kDetNumClasses; c++) {
			const float s = d[static_cast<size_t>(4 + c) * n + i];
			if (s > score) {
				score = s;
				best = c;
			}
		}
		if (score < min_score)
			continue;

		const float cx = d[0 * n + i], cy = d[1 * n + i], w = d[2 * n + i], h = d[3 * n + i];
		Detection det;
		det.cls = best;
		det.score = score;
		det.x = std::clamp(cx - w * 0.5f, 0.0f, static_cast<float>(S));
		det.y = std::clamp(cy - h * 0.5f, 0.0f, static_cast<float>(S));
		det.w = std::min(w, static_cast<float>(S) - det.x);
		det.h = std::min(h, static_cast<float>(S) - det.y);
		if (det.w > 0.0f && det.h > 0.0f)
			cand.push_back(det);
	}

	std::sort(cand.begin(), cand.end(), [](const Detection &a, const Detection &b) { return a.score > b.score; });
	if (cand.size() > kMaxCandidates)
		cand.resize(kMaxCandidates);

	// Suppression des doublons, indépendante de la classe (comme dans NudeNet).
	for (const Detection &c : cand) {
		bool keep = true;
		for (const Detection &k2 : out) {
			if (iou(c, k2) > kNmsIou) {
				keep = false;
				break;
			}
		}
		if (keep)
			out.push_back(c);
	}
	return true;
}
