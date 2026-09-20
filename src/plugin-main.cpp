// SPDX-License-Identifier: GPL-2.0-or-later

#include <obs-module.h>

#include <windows.h>

#include "nsfw-filter.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-nsfw-guard", "en-US")

std::wstring g_module_dir;

MODULE_EXPORT const char *obs_module_description(void)
{
	return "NSFW Guard: masque une source quand l'IA y détecte du contenu sensible.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "NSFW Guard";
}

bool obs_module_load(void)
{
	HMODULE self = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       reinterpret_cast<LPCWSTR>(&obs_module_load), &self)) {
		wchar_t path[MAX_PATH * 2] = {};
		GetModuleFileNameW(self, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
		g_module_dir = path;
		const size_t slash = g_module_dir.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
			g_module_dir.resize(slash);
	}

	register_nsfw_filter();
	blog(LOG_INFO, "[nsfw-guard] plugin chargé");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[nsfw-guard] plugin déchargé");
}
