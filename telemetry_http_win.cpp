// telemetry_http_win.cpp
// -----------------------------------------------------------------------------
// Windows HTTP transport + platform helpers for bz.telemetry.
//
// Uses WinHTTP for the HTTP POST because:
//   - It honours system-wide HTTP proxies (netsh winhttp set proxy / IE settings)
//   - Ships with every supported version of Windows — no vendored libcurl
//   - Synchronous I/O on the worker thread matches the macOS NSURLSession path
//
// Platform helper notes
//   - App support dir : %APPDATA% via SHGetKnownFolderPath (FOLDERID_RoamingAppData)
//   - OS version      : RtlGetVersion (bypasses the Win10 compatibility shim in
//                       GetVersionEx that lies to unmanifested processes)
//   - Host version    : GetFileVersionInfo on the host executable (Ableton Live, etc.)
//   - UUID v4         : UuidCreate from the Windows RPC runtime
// -----------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>    // CoTaskMemFree
#include <winhttp.h>    // WinHTTP session / request APIs
#include <shlobj.h>     // SHGetKnownFolderPath / FOLDERID_RoamingAppData
#include <rpc.h>        // UUID / UuidCreate
#include <winver.h>     // GetFileVersionInfoW / VerQueryValueW

#include <cstdio>
#include <string>
#include <vector>

#include "telemetry_core.hpp"

namespace bz { namespace telemetry {

// ---- Encoding helpers -------------------------------------------------------
//
// File-system paths returned to std::filesystem::path(std::string) on Windows
// must use the system ANSI code page (CP_ACP), because that is how MSVC's
// std::filesystem::path narrows / widens paths.  All other outbound
// std::string values (OS version string, host version hint) use UTF-8.

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

// Used for file-system path output (platform_app_support_dir).
static std::string wide_to_system_path(const wchar_t* ws) {
    if (!ws || ws[0] == L'\0') return {};
    int n = WideCharToMultiByte(CP_ACP, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, ws, -1, &out[0], n, nullptr, nullptr);
    return out;
}

// Used for human-readable strings (OS name, app version).
static std::string wide_to_utf8(const wchar_t* ws) {
    if (!ws || ws[0] == L'\0') return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &out[0], n, nullptr, nullptr);
    return out;
}

// ---- HTTP POST --------------------------------------------------------------

bool http_post_ndjson(const std::string& endpoint,
                      const std::string& bearer_token,
                      const std::string& ndjson_body,
                      std::string&       error_out) {
    if (endpoint.empty()) {
        error_out = "endpoint not configured";
        return false;
    }

    std::wstring wurl = utf8_to_wide(endpoint);

    // Parse scheme / host / port / path out of the endpoint URL.
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t scheme_buf[16]  = {};
    wchar_t host_buf[256]   = {};
    wchar_t path_buf[1024]  = {};
    uc.lpszScheme   = scheme_buf;  uc.dwSchemeLength   = ARRAYSIZE(scheme_buf);
    uc.lpszHostName = host_buf;    uc.dwHostNameLength = ARRAYSIZE(host_buf);
    uc.lpszUrlPath  = path_buf;    uc.dwUrlPathLength  = ARRAYSIZE(path_buf);

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        error_out = "invalid endpoint URL";
        return false;
    }

    const bool use_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    // WinHttpCrackUrl sets nPort to the scheme default (80 / 443) when the
    // URL has no explicit port, so we can pass it straight to WinHttpConnect.
    HINTERNET session = WinHttpOpen(
        L"bz.telemetry/1.0 (Max-for-Live; Windows)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        error_out = "WinHttpOpen failed";
        return false;
    }

    // Match the 30-second timeout used by the macOS NSURLSession path.
    WinHttpSetTimeouts(session,
        /*resolve*/  10000,
        /*connect*/  30000,
        /*send*/     30000,
        /*receive*/  30000);

    HINTERNET conn = WinHttpConnect(session, host_buf, uc.nPort, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        error_out = "WinHttpConnect failed";
        return false;
    }

    const DWORD req_flags = use_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(
        conn, L"POST",
        (path_buf[0] != L'\0') ? path_buf : L"/",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        req_flags);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        error_out = "WinHttpOpenRequest failed";
        return false;
    }

    WinHttpAddRequestHeaders(req,
        L"Content-Type: application/x-ndjson",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(req,
        L"Accept: application/json",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (!bearer_token.empty()) {
        std::wstring auth = L"Authorization: Bearer " + utf8_to_wide(bearer_token);
        WinHttpAddRequestHeaders(req, auth.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    const DWORD body_len = static_cast<DWORD>(ndjson_body.size());
    if (!WinHttpSendRequest(req,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            (LPVOID)ndjson_body.data(), body_len, body_len, 0)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "WinHttpSendRequest failed (%lu)", GetLastError());
        error_out = buf;
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    if (!WinHttpReceiveResponse(req, nullptr)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "WinHttpReceiveResponse failed (%lu)", GetLastError());
        error_out = buf;
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    // Check HTTP status code.
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(req,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status_code, &status_size,
        WINHTTP_NO_HEADER_INDEX);

    const bool success = (status_code >= 200 && status_code < 300);
    if (!success) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %lu", status_code);
        error_out = buf;

        // Pull a short body snippet to surface "missing API key" etc.
        DWORD avail = 0;
        WinHttpQueryDataAvailable(req, &avail);
        if (avail > 0) {
            if (avail > 256) avail = 256;
            std::vector<char> preview(static_cast<size_t>(avail + 1), '\0');
            DWORD bytes_read = 0;
            WinHttpReadData(req, preview.data(), avail, &bytes_read);
            if (bytes_read > 0) {
                error_out += ": ";
                error_out.append(preview.data(), bytes_read);
            }
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return success;
}

// ---- Platform helpers -------------------------------------------------------

std::string platform_app_support_dir() {
    // SHGetKnownFolderPath returns a wide path, e.g.
    // C:\Users\<user>\AppData\Roaming
    PWSTR path_w = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path_w);
    if (SUCCEEDED(hr) && path_w) {
        std::string out = wide_to_system_path(path_w);
        CoTaskMemFree(path_w);
        return out;
    }
    // Fallback: %APPDATA% environment variable (always set on modern Windows).
    const char* appdata = std::getenv("APPDATA");
    return appdata ? std::string(appdata) : std::string(".");
}

std::string platform_os_string() {
    // GetVersionEx lies to unmanifested apps on Windows 10/11 by reporting
    // 6.2 (Windows 8).  RtlGetVersion (ntdll) returns the real version.
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOEXW*);
    OSVERSIONINFOEXW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) fn(&osvi);
    }
    if (osvi.dwMajorVersion == 0) {
        // Should never happen; defensive fallback.
        osvi.dwMajorVersion = 10;
        osvi.dwMinorVersion = 0;
        osvi.dwBuildNumber  = 0;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Windows %lu.%lu.%lu",
                  osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);
    return buf;
}

std::string platform_max_version_hint() {
    wchar_t exe_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) return {};

    // Read the host process's ProductName + ProductVersion via VERSIONINFO.
    DWORD dummy   = 0;
    DWORD vi_size = GetFileVersionInfoSizeW(exe_path, &dummy);
    if (vi_size == 0) {
        // No version resource: return the executable name without extension.
        const wchar_t* sep  = wcsrchr(exe_path, L'\\');
        std::wstring   name = sep ? (sep + 1) : exe_path;
        if (name.size() > 4 &&
            _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) {
            name.resize(name.size() - 4);
        }
        return wide_to_utf8(name.c_str());
    }

    std::vector<BYTE> vi_data(vi_size);
    if (!GetFileVersionInfoW(exe_path, 0, vi_size, vi_data.data())) return {};

    struct LangAndCP { WORD lang; WORD codepage; };
    LangAndCP* trans   = nullptr;
    UINT       trans_n = 0;
    VerQueryValueW(vi_data.data(), L"\\VarFileInfo\\Translation",
                   reinterpret_cast<LPVOID*>(&trans), &trans_n);

    std::string out;
    if (trans && trans_n >= sizeof(LangAndCP)) {
        wchar_t sub[64]  = {};
        wchar_t* val     = nullptr;
        UINT     val_n   = 0;

        _snwprintf_s(sub, ARRAYSIZE(sub), _TRUNCATE,
                     L"\\StringFileInfo\\%04x%04x\\ProductName",
                     (unsigned)trans->lang, (unsigned)trans->codepage);
        if (VerQueryValueW(vi_data.data(), sub,
                           reinterpret_cast<LPVOID*>(&val), &val_n) && val) {
            out = wide_to_utf8(val);
        }

        _snwprintf_s(sub, ARRAYSIZE(sub), _TRUNCATE,
                     L"\\StringFileInfo\\%04x%04x\\ProductVersion",
                     (unsigned)trans->lang, (unsigned)trans->codepage);
        if (VerQueryValueW(vi_data.data(), sub,
                           reinterpret_cast<LPVOID*>(&val), &val_n) && val) {
            if (!out.empty()) out += ' ';
            out += wide_to_utf8(val);
        }
    }

    if (out.empty()) {
        const wchar_t* sep  = wcsrchr(exe_path, L'\\');
        std::wstring   name = sep ? (sep + 1) : exe_path;
        if (name.size() > 4 &&
            _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) {
            name.resize(name.size() - 4);
        }
        out = wide_to_utf8(name.c_str());
    }
    return out;
}

std::string platform_uuid_v4() {
    // UuidCreate produces a v4 (random) UUID on Windows XP and later.
    UUID uuid = {};
    UuidCreate(&uuid);
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned long>(uuid.Data1),
                  static_cast<unsigned>(uuid.Data2),
                  static_cast<unsigned>(uuid.Data3),
                  uuid.Data4[0], uuid.Data4[1],
                  uuid.Data4[2], uuid.Data4[3],
                  uuid.Data4[4], uuid.Data4[5],
                  uuid.Data4[6], uuid.Data4[7]);
    return buf;
}

}}  // namespace bz::telemetry
