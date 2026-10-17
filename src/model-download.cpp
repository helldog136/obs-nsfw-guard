// SPDX-License-Identifier: GPL-2.0-or-later

#include "model-download.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Épinglé sur la release "v3.4-weights" de https://github.com/notAI-tech/NudeNet
const ModelInfo kModels[kModelCount] = {
	{"320n", "nudenet_320n.onnx", "NudeNet 320n", 176831997ULL, 12150158ULL,
	 "c15d8273adad2d0a92f014cc69ab2d6c311a06777a55545f2c4eb46f51911f0f"},
	{"640m", "nudenet_640m.onnx", "NudeNet 640m", 176832019ULL, 103538690ULL,
	 "04fe3d77980780c1f8297dc6d7f942fd5b3abe6942a188f742a85241e4f634eb"},
};

std::mutex g_status_m;
DlStatus g_status;
std::atomic<bool> g_busy{false};
std::atomic<uint32_t> g_generation{0};

void set_status(const DlStatus &s)
{
	std::lock_guard<std::mutex> lk(g_status_m);
	g_status = s;
}

// SHA-256 incrémental (BCrypt).
class Sha256 {
public:
	Sha256()
	{
		ok_ = BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
		      BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) == 0;
	}
	~Sha256()
	{
		if (hash_)
			BCryptDestroyHash(hash_);
		if (alg_)
			BCryptCloseAlgorithmProvider(alg_, 0);
	}
	bool ok() const { return ok_; }
	void update(const uint8_t *data, size_t len)
	{
		if (ok_)
			ok_ = BCryptHashData(hash_, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0) == 0;
	}
	bool finish(std::string &hex)
	{
		uint8_t out[32];
		if (!ok_ || BCryptFinishHash(hash_, out, sizeof(out), 0) != 0)
			return false;
		static const char *digits = "0123456789abcdef";
		hex.clear();
		for (uint8_t b : out) {
			hex.push_back(digits[b >> 4]);
			hex.push_back(digits[b & 15]);
		}
		return true;
	}

private:
	BCRYPT_ALG_HANDLE alg_ = nullptr;
	BCRYPT_HASH_HANDLE hash_ = nullptr;
	bool ok_ = false;
};

std::wstring join(const std::wstring &dir, const char *file)
{
	std::wstring name(file, file + std::strlen(file));
	return dir + (dir.empty() || dir.back() == L'\\' || dir.back() == L'/' ? L"" : L"\\") + name;
}

struct WinHttpHandle {
	HINTERNET h = nullptr;
	explicit WinHttpHandle(HINTERNET handle = nullptr) : h(handle) {}
	~WinHttpHandle()
	{
		if (h)
			WinHttpCloseHandle(h);
	}
	WinHttpHandle(const WinHttpHandle &) = delete;
	WinHttpHandle &operator=(const WinHttpHandle &) = delete;
};

void download_thread(int index, std::wstring dest_dir)
{
	const ModelInfo &mi = kModels[index];
	DlStatus st;
	st.state = DlState::Downloading;
	st.model = index;
	st.total = mi.size;
	set_status(st);

	auto fail = [&](DlError e, int http = 0) {
		st.state = DlState::Error;
		st.error = e;
		st.http_status = http;
		set_status(st);
		g_busy = false;
	};

	const std::wstring dest = join(dest_dir, mi.file);
	const std::wstring part = dest + L".part";

	// Route officielle de l'API GitHub pour les ressources d'une release : elle répond par
	// une redirection vers le serveur de fichiers (suivie automatiquement par WinHTTP) et
	// n'exige pas de compte. Limite : 60 requêtes par heure et par adresse IP.
	WinHttpHandle session(WinHttpOpen(L"obs-nsfw-guard", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
					  WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session.h)
		return fail(DlError::Network);
	WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);

	WinHttpHandle connection(WinHttpConnect(session.h, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
	if (!connection.h)
		return fail(DlError::Network);

	const std::wstring path = L"/repos/notAI-tech/NudeNet/releases/assets/" + std::to_wstring(mi.asset_id);
	WinHttpHandle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
						 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if (!request.h)
		return fail(DlError::Network);

	const wchar_t *headers = L"Accept: application/octet-stream\r\n";
	if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(request.h, nullptr))
		return fail(DlError::Network);

	DWORD status_code = 0, size = sizeof(status_code);
	if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				 WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size, WINHTTP_NO_HEADER_INDEX))
		return fail(DlError::Network);
	if (status_code != 200)
		return fail(DlError::HttpStatus, static_cast<int>(status_code));

	FILE *out = _wfopen(part.c_str(), L"wb");
	if (!out)
		return fail(DlError::Io);

	Sha256 hash;
	std::vector<uint8_t> buf(64 * 1024);
	uint64_t total = 0;
	bool io_ok = hash.ok();
	for (;;) {
		DWORD got = 0;
		if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
			std::fclose(out);
			_wremove(part.c_str());
			return fail(DlError::Network);
		}
		if (got == 0)
			break;
		hash.update(buf.data(), got);
		if (std::fwrite(buf.data(), 1, got, out) != got)
			io_ok = false;
		total += got;
		if (total > mi.size + 1024 * 1024) // bien plus gros que prévu : on abandonne
			break;
		st.done = total;
		set_status(st);
	}
	const bool closed = std::fclose(out) == 0;

	std::string hex;
	const bool hashed = hash.finish(hex);
	if (!io_ok || !closed || !hashed) {
		_wremove(part.c_str());
		return fail(DlError::Io);
	}
	if (total != mi.size) {
		_wremove(part.c_str());
		return fail(DlError::SizeMismatch);
	}
	if (hex != mi.sha256) {
		_wremove(part.c_str());
		return fail(DlError::HashMismatch);
	}
	if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		_wremove(part.c_str());
		return fail(DlError::Io);
	}

	st.state = DlState::Done;
	st.done = total;
	st.error = DlError::None;
	set_status(st);
	g_generation++;
	g_busy = false;
}

} // namespace

const ModelInfo &model_info(int index)
{
	return kModels[index < 0 ? 0 : index >= kModelCount ? kModelCount - 1 : index];
}

bool start_model_download(int index, const std::wstring &dest_dir)
{
	if (index < 0 || index >= kModelCount || dest_dir.empty())
		return false;
	bool expected = false;
	if (!g_busy.compare_exchange_strong(expected, true))
		return false;
	std::thread(download_thread, index, dest_dir).detach();
	return true;
}

DlStatus download_status()
{
	std::lock_guard<std::mutex> lk(g_status_m);
	return g_status;
}

uint32_t models_generation()
{
	return g_generation.load();
}

bool sha256_file(const std::wstring &path, std::string &hex)
{
	FILE *in = _wfopen(path.c_str(), L"rb");
	if (!in)
		return false;
	Sha256 hash;
	std::vector<uint8_t> buf(256 * 1024);
	size_t n;
	while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
		hash.update(buf.data(), n);
	const bool read_ok = std::ferror(in) == 0;
	std::fclose(in);
	return read_ok && hash.finish(hex);
}

int import_model_file(const std::wstring &src_path, const std::wstring &dest_dir)
{
	if (src_path.empty() || dest_dir.empty())
		return -1;

	WIN32_FILE_ATTRIBUTE_DATA attr;
	if (!GetFileAttributesExW(src_path.c_str(), GetFileExInfoStandard, &attr))
		return -1;
	const uint64_t size = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;

	// On identifie d'abord par la taille (rapide), puis on confirme par l'empreinte.
	for (int i = 0; i < kModelCount; i++) {
		if (size != kModels[i].size)
			continue;
		std::string hex;
		if (!sha256_file(src_path, hex) || hex != kModels[i].sha256)
			return -1;

		const std::wstring dest = join(dest_dir, kModels[i].file);
		const std::wstring part = dest + L".part";
		if (!CopyFileW(src_path.c_str(), part.c_str(), FALSE))
			return -1;
		if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
			_wremove(part.c_str());
			return -1;
		}
		g_generation++;
		return i;
	}
	return -1;
}
