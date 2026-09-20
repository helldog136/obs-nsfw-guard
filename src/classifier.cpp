// SPDX-License-Identifier: GPL-2.0-or-later

#include "classifier.h"

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace {
bool g_ort_ready = false;
} // namespace

bool ClassifierGlobalInit(const std::wstring &dir, std::string &err)
{
	if (g_ort_ready)
		return true;

	// DirectML : on utilise la copie fournie par Windows (System32), chargée par
	// chemin complet. D'autres plugins peuvent embarquer une version plus ancienne
	// dans le dossier d'OBS ; sans cela, onnxruntime prendrait celle-là par nom.
	// Si le chargement échoue, l'option GPU se repliera sur le CPU.
	wchar_t sys_dir[MAX_PATH] = {};
	if (GetSystemDirectoryW(sys_dir, MAX_PATH))
		LoadLibraryW((std::wstring(sys_dir) + L"\\DirectML.dll").c_str());

	HMODULE ort = LoadLibraryExW((dir + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!ort) {
		err = "impossible de charger onnxruntime.dll (code " + std::to_string(GetLastError()) + ")";
		return false;
	}

	using GetApiBaseFn = const OrtApiBase *(ORT_API_CALL *)();
	auto get_base = reinterpret_cast<GetApiBaseFn>(GetProcAddress(ort, "OrtGetApiBase"));
	if (!get_base) {
		err = "OrtGetApiBase introuvable dans onnxruntime.dll";
		return false;
	}

	const OrtApi *api = get_base()->GetApi(ORT_API_VERSION);
	if (!api) {
		err = "version d'API onnxruntime incompatible";
		return false;
	}

	Ort::InitApi(api);
	g_ort_ready = true;
	return true;
}

struct Classifier::Impl {
	Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "nsfw-guard"};
	Ort::Session session{nullptr};
	Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
	std::vector<float> input = std::vector<float>(3 * kInputSize * kInputSize);
};

Classifier::Classifier(const std::wstring &model_path, bool use_directml) : impl_(std::make_unique<Impl>())
{
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
		// Petit modèle : 2 threads suffisent et on évite l'attente active.
		so.SetIntraOpNumThreads(2);
		so.SetInterOpNumThreads(1);
		so.AddConfigEntry("session.intra_op.allow_spinning", "0");
	}

	impl_->session = Ort::Session(impl_->env, model_path.c_str(), so);
}

Classifier::~Classifier() = default;

bool Classifier::Run(const uint8_t *rgba, uint32_t linesize, float probs[kNumClasses], float &elapsed_ms)
{
	constexpr int hw = kInputSize * kInputSize;
	float *r = impl_->input.data();
	float *g = r + hw;
	float *b = g + hw;
	constexpr float k = 1.0f / 255.0f;

	// RGBA entrelacé -> CHW planaire, valeurs dans [0,1].
	// La normalisation ImageNet est déjà incluse dans le graphe du modèle.
	for (int y = 0; y < kInputSize; y++) {
		const uint8_t *row = rgba + static_cast<size_t>(y) * linesize;
		const int base = y * kInputSize;
		for (int x = 0; x < kInputSize; x++) {
			const uint8_t *p = row + x * 4;
			r[base + x] = p[0] * k;
			g[base + x] = p[1] * k;
			b[base + x] = p[2] * k;
		}
	}

	const int64_t shape[3] = {3, kInputSize, kInputSize};
	Ort::Value tensor = Ort::Value::CreateTensor<float>(impl_->mem, impl_->input.data(), impl_->input.size(),
							    shape, 3);
	const char *in_names[] = {"input"};
	const char *out_names[] = {"output"};

	const auto t0 = std::chrono::steady_clock::now();
	auto out = impl_->session.Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 1);
	elapsed_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();

	if (out.empty())
		return false;
	const float *logits = out[0].GetTensorData<float>();

	// Softmax numériquement stable.
	float mx = logits[0];
	for (int i = 1; i < kNumClasses; i++)
		mx = std::max(mx, logits[i]);
	float sum = 0.0f;
	for (int i = 0; i < kNumClasses; i++) {
		probs[i] = std::exp(logits[i] - mx);
		sum += probs[i];
	}
	for (int i = 0; i < kNumClasses; i++)
		probs[i] /= sum;
	return true;
}
