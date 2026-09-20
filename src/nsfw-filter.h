// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

// Dossier contenant le DLL du plugin (et onnxruntime.dll / DirectML.dll).
extern std::wstring g_module_dir;

void register_nsfw_filter();
