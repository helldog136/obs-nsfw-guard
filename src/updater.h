// SPDX-License-Identifier: GPL-2.0-or-later
//
// Mises à jour du plugin : vérification de la dernière release GitHub du projet (au plus une
// fois par jour, désactivable), téléchargement de son installeur (taille et SHA-256 vérifiés)
// et lancement de cet installeur à la FERMETURE d'OBS, pour ne jamais interrompre un stream.
#pragma once

#include <string>

enum class UpdState {
	Idle,        // pas encore vérifié
	Checking,    // vérification en cours
	UpToDate,    // aucune version plus récente
	Available,   // une version plus récente existe
	Downloading, // téléchargement de l'installeur en cours
	Ready,       // installeur téléchargé : il sera lancé à la fermeture d'OBS
	Error,       // échec de la dernière opération (voir `error`)
};

struct UpdStatus {
	UpdState state = UpdState::Idle;
	std::string current;  // version installée
	std::string latest;   // dernière version publiée (si connue)
	std::string page_url; // page de la release
	std::string error;    // message technique (journal)
	int percent = 0;      // progression du téléchargement
};

// À appeler au chargement du module : lit la configuration, nettoie les installeurs périmés
// et lance une vérification en arrière-plan si elle est due.
void updater_init();

// À appeler au déchargement du module : arrête le thread en cours et, si un installeur a été
// téléchargé, le lance (il attend lui-même la fin d'OBS avant d'installer).
void updater_shutdown();

UpdStatus updater_status();

bool updater_auto_check();
void updater_set_auto_check(bool on);

// Actions demandées depuis les réglages du filtre (sans effet si une opération est en cours).
void updater_check_now();
void updater_download();
void updater_cancel_pending();
