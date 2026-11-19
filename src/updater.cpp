// SPDX-License-Identifier: GPL-2.0-or-later

#include "updater.h"
#include "model-download.h" // sha256_file

#include <obs-module.h>
#include <util/platform.h>

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.0.0"
#endif

#define LOG_PREFIX "[nsfw-guard] "

namespace {

const wchar_t *const kApiHost = L"api.github.com";
const std::wstring kRepoPath = L"/repos/helldog136/obs-nsfw-guard";
constexpr int64_t kCheckEverySec = 24 * 3600;
constexpr uint64_t kMaxInstaller = 300ULL * 1024 * 1024;
constexpr size_t kMaxJson = 4 * 1024 * 1024;
// Paramètres de l'installeur lancé à la fermeture d'OBS : /WAITOBS lui fait attendre la fin
// d'OBS avant d'installer ; /SILENT affiche seulement la progression.
const wchar_t *const kInstallerArgs = L"/SP- /SILENT /SUPPRESSMSGBOXES /NORESTART /WAITOBS";

// --- état partagé (protégé par g_m) ---
std::mutex g_m;
UpdStatus g_st;
bool g_auto = true;
int64_t g_last_check = 0;
std::string g_pending_path;    // installeur téléchargé, prêt à être lancé (UTF-8)
std::string g_pending_version; // sa version
int64_t g_setup_id = 0, g_setup_size = 0, g_sha_id = 0; // ressources de la dernière release

// --- thread de travail (un seul à la fois) ---
std::mutex g_thread_m;
std::thread g_thread;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_cancel{false};

std::string module_config(const char *name)
{
	char *p = obs_module_config_path(name);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

std::wstring widen(const std::string &utf8)
{
	std::wstring r;
	wchar_t *w = nullptr;
	os_utf8_to_wcs_ptr(utf8.c_str(), 0, &w);
	if (w) {
		r = w;
		bfree(w);
	}
	std::replace(r.begin(), r.end(), L'/', L'\\');
	return r;
}

bool file_exists(const std::string &utf8)
{
	return !utf8.empty() && GetFileAttributesW(widen(utf8).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// "v1.2.3" -> {1, 2, 3}. Les suffixes ("-beta") sont ignorés.
std::vector<int> parse_version(std::string v)
{
	if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
		v.erase(0, 1);
	std::vector<int> r;
	int cur = 0;
	bool any = false;
	for (char c : v) {
		if (std::isdigit(static_cast<unsigned char>(c))) {
			cur = cur * 10 + (c - '0');
			any = true;
		} else if (c == '.') {
			r.push_back(cur);
			cur = 0;
			any = false;
		} else {
			break;
		}
	}
	if (any)
		r.push_back(cur);
	r.resize(std::max<size_t>(r.size(), 3), 0);
	return r;
}

bool is_newer(const std::string &candidate, const std::string &current)
{
	return parse_version(candidate) > parse_version(current);
}

std::string strip_v(const std::string &tag)
{
	return !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
}

bool ends_with(const std::string &s, const char *suffix)
{
	const size_t n = std::strlen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Appelant : g_m verrouillé.
void save_config()
{
	const std::string file = module_config("updater.json");
	if (file.empty())
		return;
	const std::string dir = module_config("");
	if (!dir.empty())
		os_mkdirs(dir.c_str());
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "auto_check", g_auto);
	obs_data_set_int(d, "last_check", g_last_check);
	obs_data_set_string(d, "pending_path", g_pending_path.c_str());
	obs_data_set_string(d, "pending_version", g_pending_version.c_str());
	obs_data_save_json_safe(d, file.c_str(), "tmp", "bak");
	obs_data_release(d);
}

void set_error(const std::string &msg)
{
	blog(LOG_WARNING, LOG_PREFIX "mise à jour : %s", msg.c_str());
	std::lock_guard<std::mutex> lk(g_m);
	g_st.state = UpdState::Error;
	g_st.error = msg;
}

struct Handle {
	HINTERNET h = nullptr;
	explicit Handle(HINTERNET x) : h(x) {}
	~Handle()
	{
		if (h)
			WinHttpCloseHandle(h);
	}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
};

// GET https://api.github.com<path>. Le corps est passé à `sink` par morceaux (qui renvoie
// faux pour abandonner). Renvoie le code HTTP, ou 0 en cas d'erreur réseau / d'abandon.
int http_get(const std::wstring &path, const wchar_t *accept, const std::function<bool(const uint8_t *, size_t)> &sink)
{
	Handle session(WinHttpOpen(L"obs-nsfw-guard-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				   WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session.h)
		return 0;
	WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 30000);
	Handle conn(WinHttpConnect(session.h, kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
	if (!conn.h)
		return 0;
	Handle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
				      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if (!req.h)
		return 0;
	const std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
	if (!WinHttpSendRequest(req.h, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(req.h, nullptr))
		return 0;
	DWORD code = 0, size = sizeof(code);
	if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
				 &code, &size, WINHTTP_NO_HEADER_INDEX))
		return 0;
	if (code != 200)
		return static_cast<int>(code);

	std::vector<uint8_t> buf(64 * 1024);
	for (;;) {
		if (g_cancel)
			return 0;
		DWORD got = 0;
		if (!WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &got))
			return 0;
		if (got == 0)
			break;
		if (!sink(buf.data(), got))
			return 0;
	}
	return 200;
}

// Termine le thread courant : remet g_busy à faux quoi qu'il arrive.
struct BusyGuard {
	~BusyGuard() { g_busy = false; }
};

void check_thread()
{
	BusyGuard guard;
	{
		std::lock_guard<std::mutex> lk(g_m);
		g_st.state = UpdState::Checking;
		g_st.error.clear();
	}

	std::string body;
	const int code = http_get(kRepoPath + L"/releases/latest", L"application/vnd.github+json",
				  [&](const uint8_t *p, size_t n) {
					  if (body.size() + n > kMaxJson)
						  return false;
					  body.append(reinterpret_cast<const char *>(p), n);
					  return true;
				  });
	if (g_cancel)
		return;
	if (code == 404) { // aucune release publiée
		std::lock_guard<std::mutex> lk(g_m);
		g_last_check = static_cast<int64_t>(std::time(nullptr));
		g_st.state = UpdState::UpToDate;
		save_config();
		return;
	}
	if (code != 200)
		return set_error(code ? "réponse HTTP " + std::to_string(code) + " de GitHub" : "GitHub injoignable");

	obs_data_t *d = obs_data_create_from_json(body.c_str());
	if (!d)
		return set_error("réponse de GitHub illisible");
	const std::string tag = obs_data_get_string(d, "tag_name");
	const std::string page = obs_data_get_string(d, "html_url");
	int64_t setup_id = 0, setup_size = 0, sha_id = 0;
	obs_data_array_t *assets = obs_data_get_array(d, "assets");
	const size_t count = assets ? obs_data_array_count(assets) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *a = obs_data_array_item(assets, i);
		const std::string name = obs_data_get_string(a, "name");
		if (ends_with(name, "-windows-x64-setup.exe")) {
			setup_id = obs_data_get_int(a, "id");
			setup_size = obs_data_get_int(a, "size");
		} else if (ends_with(name, "-windows-x64-setup.exe.sha256")) {
			sha_id = obs_data_get_int(a, "id");
		}
		obs_data_release(a);
	}
	obs_data_array_release(assets);
	obs_data_release(d);

	if (tag.empty())
		return set_error("release sans numéro de version");

	const std::string latest = strip_v(tag);
	const bool newer = is_newer(latest, PLUGIN_VERSION);
	{
		std::lock_guard<std::mutex> lk(g_m);
		g_last_check = static_cast<int64_t>(std::time(nullptr));
		g_setup_id = setup_id;
		g_setup_size = setup_size;
		g_sha_id = sha_id;
		g_st.latest = latest;
		g_st.page_url = page;
		if (!newer)
			g_st.state = UpdState::UpToDate;
		else if (g_pending_version == latest && file_exists(g_pending_path))
			g_st.state = UpdState::Ready;
		else
			g_st.state = UpdState::Available;
		save_config();
	}
	if (newer)
		blog(LOG_INFO, LOG_PREFIX "nouvelle version %s disponible (installée : %s) : %s", latest.c_str(),
		     PLUGIN_VERSION, page.c_str());
	else
		blog(LOG_INFO, LOG_PREFIX "le plugin est à jour (%s)", PLUGIN_VERSION);
}

void download_thread()
{
	BusyGuard guard;
	int64_t setup_id, setup_size, sha_id;
	std::string latest;
	{
		std::lock_guard<std::mutex> lk(g_m);
		setup_id = g_setup_id;
		setup_size = g_setup_size;
		sha_id = g_sha_id;
		latest = g_st.latest;
		g_st.state = UpdState::Downloading;
		g_st.percent = 0;
		g_st.error.clear();
	}
	if (!setup_id || !sha_id || setup_size <= 0 || static_cast<uint64_t>(setup_size) > kMaxInstaller)
		return set_error("installeur ou empreinte absents de la release");

	// 1) Empreinte publiée par la CI ("<sha256>  <fichier>").
	std::string sha_txt;
	int code = http_get(kRepoPath + L"/releases/assets/" + std::to_wstring(sha_id), L"application/octet-stream",
			    [&](const uint8_t *p, size_t n) {
				    if (sha_txt.size() + n > 4096)
					    return false;
				    sha_txt.append(reinterpret_cast<const char *>(p), n);
				    return true;
			    });
	if (g_cancel)
		return;
	if (code != 200)
		return set_error("empreinte : réponse HTTP " + std::to_string(code));
	std::string expected;
	for (char c : sha_txt) {
		if (!std::isxdigit(static_cast<unsigned char>(c)))
			break;
		expected.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (expected.size() != 64)
		return set_error("empreinte illisible");

	// 2) Installeur, dans le dossier de configuration du plugin.
	const std::string dir = module_config("update");
	if (dir.empty() || os_mkdirs(dir.c_str()) == MKDIR_ERROR)
		return set_error("dossier de téléchargement inaccessible");
	const std::wstring part = widen(dir + "/obs-nsfw-guard-setup.exe.part");
	FILE *out = _wfopen(part.c_str(), L"wb");
	if (!out)
		return set_error("écriture impossible dans " + dir);
	uint64_t total = 0;
	bool io_ok = true;
	code = http_get(kRepoPath + L"/releases/assets/" + std::to_wstring(setup_id), L"application/octet-stream",
			[&](const uint8_t *p, size_t n) {
				total += n;
				if (total > static_cast<uint64_t>(setup_size))
					return false;
				if (std::fwrite(p, 1, n, out) != n)
					io_ok = false;
				std::lock_guard<std::mutex> lk(g_m);
				g_st.percent = static_cast<int>(total * 100 / static_cast<uint64_t>(setup_size));
				return io_ok;
			});
	const bool closed = std::fclose(out) == 0;
	if (g_cancel) {
		_wremove(part.c_str());
		return;
	}
	if (code != 200 || !io_ok || !closed || total != static_cast<uint64_t>(setup_size)) {
		_wremove(part.c_str());
		return set_error(code != 200 ? "téléchargement interrompu (HTTP " + std::to_string(code) + ")"
					     : "fichier téléchargé incomplet");
	}
	std::string hex;
	if (!sha256_file(part, hex) || hex != expected) {
		_wremove(part.c_str());
		return set_error("empreinte SHA-256 de l'installeur incorrecte : fichier rejeté");
	}

	const std::string final_path = dir + "/obs-nsfw-guard-" + latest + "-setup.exe";
	if (!MoveFileExW(part.c_str(), widen(final_path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
		_wremove(part.c_str());
		return set_error("impossible de renommer l'installeur téléchargé");
	}

	{
		std::lock_guard<std::mutex> lk(g_m);
		g_pending_path = final_path;
		g_pending_version = latest;
		g_st.state = UpdState::Ready;
		g_st.percent = 100;
		save_config();
	}
	blog(LOG_INFO, LOG_PREFIX "version %s téléchargée : elle sera installée à la fermeture d'OBS", latest.c_str());
}

void start(void (*fn)())
{
	bool expected = false;
	if (!g_busy.compare_exchange_strong(expected, true))
		return;
	std::lock_guard<std::mutex> lk(g_thread_m);
	if (g_thread.joinable())
		g_thread.join();
	g_cancel = false;
	g_thread = std::thread(fn);
}

// Supprime du dossier de téléchargement tout ce qui n'est pas l'installeur en attente.
void clean_update_dir(const std::string &keep)
{
	const std::string dir = module_config("update");
	if (dir.empty())
		return;
	const std::wstring wdir = widen(dir);
	const std::wstring wkeep = keep.empty() ? L"" : widen(keep);
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((wdir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		const std::wstring p = wdir + L"\\" + fd.cFileName;
		if (_wcsicmp(p.c_str(), wkeep.c_str()) != 0)
			DeleteFileW(p.c_str());
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

} // namespace

void updater_init()
{
	bool due = false;
	{
		std::lock_guard<std::mutex> lk(g_m);
		g_st.current = PLUGIN_VERSION;
		const std::string file = module_config("updater.json");
		obs_data_t *d = file.empty() ? nullptr : obs_data_create_from_json_file_safe(file.c_str(), "bak");
		if (d) {
			obs_data_set_default_bool(d, "auto_check", true);
			g_auto = obs_data_get_bool(d, "auto_check");
			g_last_check = obs_data_get_int(d, "last_check");
			g_pending_path = obs_data_get_string(d, "pending_path");
			g_pending_version = obs_data_get_string(d, "pending_version");
			obs_data_release(d);
		}

		// Installeur en attente déjà installé (ou disparu) : on l'oublie.
		if (!g_pending_path.empty() &&
		    (!is_newer(g_pending_version, PLUGIN_VERSION) || !file_exists(g_pending_path))) {
			if (!is_newer(g_pending_version, PLUGIN_VERSION))
				blog(LOG_INFO, LOG_PREFIX "mise à jour vers %s installée", PLUGIN_VERSION);
			g_pending_path.clear();
			g_pending_version.clear();
			save_config();
		}
		clean_update_dir(g_pending_path);

		if (!g_pending_path.empty()) {
			g_st.state = UpdState::Ready;
			g_st.latest = g_pending_version;
		}
		const int64_t now = static_cast<int64_t>(std::time(nullptr));
		due = g_auto && (now - g_last_check > kCheckEverySec || now < g_last_check);
	}
	if (due)
		start(check_thread);
}

void updater_shutdown()
{
	g_cancel = true;
	{
		std::lock_guard<std::mutex> lk(g_thread_m);
		if (g_thread.joinable())
			g_thread.join();
	}

	std::string pending, version;
	{
		std::lock_guard<std::mutex> lk(g_m);
		pending = g_pending_path;
		version = g_pending_version;
	}
	if (pending.empty() || !file_exists(pending))
		return;

	// L'installeur demande les droits d'administrateur (fenêtre Windows) puis attend que
	// OBS soit fermé. Si l'utilisateur refuse, il sera reproposé à la prochaine fermeture.
	const std::wstring exe = widen(pending);
	SHELLEXECUTEINFOW sei = {};
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOASYNC;
	sei.lpFile = exe.c_str();
	sei.lpParameters = kInstallerArgs;
	sei.nShow = SW_SHOWNORMAL;
	if (ShellExecuteExW(&sei))
		blog(LOG_INFO, LOG_PREFIX "installeur de la version %s lancé", version.c_str());
	else
		blog(LOG_WARNING, LOG_PREFIX "impossible de lancer l'installeur de la version %s (code %lu)",
		     version.c_str(), GetLastError());
}

UpdStatus updater_status()
{
	std::lock_guard<std::mutex> lk(g_m);
	return g_st;
}

bool updater_auto_check()
{
	std::lock_guard<std::mutex> lk(g_m);
	return g_auto;
}

void updater_set_auto_check(bool on)
{
	bool due = false;
	{
		std::lock_guard<std::mutex> lk(g_m);
		if (g_auto == on)
			return;
		g_auto = on;
		save_config();
		due = on && static_cast<int64_t>(std::time(nullptr)) - g_last_check > kCheckEverySec;
	}
	if (due)
		start(check_thread);
}

void updater_check_now()
{
	start(check_thread);
}

void updater_download()
{
	{
		std::lock_guard<std::mutex> lk(g_m);
		if (g_st.state != UpdState::Available && g_st.state != UpdState::Error)
			return;
		if (!is_newer(g_st.latest, PLUGIN_VERSION))
			return;
	}
	start(download_thread);
}

void updater_cancel_pending()
{
	std::lock_guard<std::mutex> lk(g_m);
	if (g_pending_path.empty())
		return;
	DeleteFileW(widen(g_pending_path).c_str());
	g_pending_path.clear();
	g_pending_version.clear();
	save_config();
	g_st.state = is_newer(g_st.latest, PLUGIN_VERSION) ? UpdState::Available : UpdState::UpToDate;
	blog(LOG_INFO, LOG_PREFIX "mise à jour téléchargée annulée");
}
