// SPDX-License-Identifier: GPL-2.0-or-later
//
// Modèles NudeNet (détecteur optionnel). Les poids ne sont PAS distribués avec ce
// plugin : l'utilisateur les récupère lui-même, depuis la release officielle de
// NudeNet, soit par téléchargement dans le plugin, soit en important le fichier.
// Chaque fichier est vérifié par sa taille et son empreinte SHA-256, épinglées ici.
#pragma once

#include <cstdint>
#include <string>

struct ModelInfo {
	const char *id;     // "320n", "640m"
	const char *file;   // nom du fichier dans les dossiers du plugin
	const char *label;  // nom affiché
	uint64_t asset_id;  // identifiant de la ressource dans la release GitHub
	uint64_t size;      // taille attendue, en octets
	const char *sha256; // empreinte attendue, en hexadécimal minuscule
};

constexpr int kModelCount = 2;
const ModelInfo &model_info(int index);

enum class DlState { Idle, Downloading, Done, Error };
enum class DlError { None, Network, HttpStatus, SizeMismatch, HashMismatch, Io };

struct DlStatus {
	DlState state = DlState::Idle;
	int model = -1;
	uint64_t done = 0;
	uint64_t total = 0;
	DlError error = DlError::None;
	int http_status = 0;
};

// Lance le téléchargement du modèle `index` dans `dest_dir` (dossier existant), dans un
// thread. Renvoie false si un téléchargement est déjà en cours ou si les arguments sont
// invalides.
bool start_model_download(int index, const std::wstring &dest_dir);

// Interrompt un téléchargement en cours et attend la fin de son thread. À appeler au
// déchargement du plugin : le code du thread ne doit plus s'exécuter une fois le DLL libéré.
void model_download_shutdown();

// État du dernier téléchargement.
DlStatus download_status();

// Compteur incrémenté à chaque modèle nouvellement installé (téléchargé ou importé) :
// les filtres le consultent pour relire l'emplacement des modèles.
uint32_t models_generation();

// SHA-256 d'un fichier, en hexadécimal minuscule. Faux si le fichier est illisible.
bool sha256_file(const std::wstring &path, std::string &hex);

// Importe un fichier fourni par l'utilisateur : identifie le modèle par sa taille et son
// empreinte, puis le copie dans `dest_dir` sous son nom normalisé. Renvoie l'index du
// modèle (>= 0), ou -1 si le fichier n'est aucun des modèles attendus.
int import_model_file(const std::wstring &src_path, const std::wstring &dest_dir);
