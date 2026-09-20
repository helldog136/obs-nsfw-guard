// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Ordre des classes du modèle : drawings, hentai, neutral, porn, sexy
constexpr int kClassDrawings = 0;
constexpr int kClassHentai = 1;
constexpr int kClassNeutral = 2;
constexpr int kClassPorn = 3;
constexpr int kClassSexy = 4;
constexpr int kNumClasses = 5;
constexpr int kInputSize = 224;

// Charge onnxruntime.dll depuis `dir`, par chemin complet pour ne jamais tomber
// sur celui de System32 ou d'un autre plugin. Précharge aussi le DirectML.dll
// de Windows pour l'option GPU.
bool ClassifierGlobalInit(const std::wstring &dir, std::string &err);

class Classifier {
public:
	// Lève std::exception en cas d'échec de chargement du modèle.
	Classifier(const std::wstring &model_path, bool use_directml);
	~Classifier();

	// rgba : image kInputSize x kInputSize, 4 octets par pixel.
	// Retourne les probabilités (softmax) dans `probs`.
	bool Run(const uint8_t *rgba, uint32_t linesize, float probs[kNumClasses], float &elapsed_ms);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
