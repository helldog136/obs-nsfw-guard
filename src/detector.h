// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Classes du modèle NudeNet (YOLOv8), dans l'ordre des sorties du réseau
// (vérifié dans les métadonnées des fichiers 320n.onnx et 640m.onnx).
constexpr int kDetNumClasses = 18;

struct Detection {
	int cls = 0;       // index de classe, 0..kDetNumClasses-1
	float score = 0.0f;
	float x = 0.0f;    // coin haut-gauche, en pixels de la vue envoyée au modèle
	float y = 0.0f;
	float w = 0.0f;
	float h = 0.0f;
	int view = 0;      // index de la vue dans laquelle la détection a été faite
};

// Détecteur d'objets NudeNet exécuté par ONNX Runtime. Le chargement de la
// bibliothèque est fait par ClassifierGlobalInit (classifier.h).
class Detector {
public:
	// `side` : côté de l'image carrée attendue par le modèle (320 pour 320n, 640 pour 640m).
	// Lève std::exception en cas d'échec de chargement.
	Detector(const std::wstring &model_path, bool use_directml, int side);
	~Detector();

	int side() const { return side_; }

	// rgba : image side x side, 4 octets par pixel. Remplit `out` avec les détections
	// dont le score dépasse `min_score`, après suppression des cadres redondants.
	bool Run(const uint8_t *rgba, uint32_t linesize, float min_score, std::vector<Detection> &out,
		 float &elapsed_ms);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	int side_;
};
