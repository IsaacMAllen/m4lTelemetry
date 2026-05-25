// telemetry_crash_win.cpp
// -----------------------------------------------------------------------------
// Windows crash-capture hooks for bz.telemetry.
//
// What this module captures
//   - Unhandled SEH exceptions via SetUnhandledExceptionFilter:
//       EXCEPTION_ACCESS_VIOLATION, EXCEPTION_STACK_OVERFLOW,
//       EXCEPTION_INT_DIVIDE_BY_ZERO, EXCEPTION_ILLEGAL_INSTRUCTION, etc.
//     We chain to the previously-installed filter after writing the tombstone
//     so Windows Error Reporting still fires — this module is *additive*.
//
//   - Uncaught C++ exceptions via std::set_terminate.  We chain back to the
//     previously-installed handler so we don't break the default terminate path.
//
// What we DON'T do
//   - We don't capture a stack trace.  Adding one later is straightforward via
//     StackWalk64 (dbghelp.dll) — see the POTENTIAL ENHANCEMENT note below.
//
// Safety inside the exception filter
//   - CreateFile / WriteFile / CloseHandle are safe to call from an
//     UnhandledExceptionFilter context (they do not acquire any loader or
//     heap locks the way, say, malloc might).
//   - GetSystemTime and GetCurrentThreadId are kernel-mode-dispatched and
//     equally safe.
//   - We store the tombstone directory as a pre-converted wide-char buffer
//     (g_tombstones_dir_w) so the filter never calls MultiByteToWideChar.
//   - A compare_exchange reentrancy guard prevents a second exception during
//     tombstone writing from clobbering the first file or looping.
//   - We skip EXCEPTION_BREAKPOINT and the MSVC C++ SEH cookie (0xE06D7363)
//     to avoid fighting with debuggers or the C++ runtime's own terminate path.
// -----------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>

#include "telemetry_core.hpp"

namespace bz { namespace telemetry {

// Static state.  Plain C arrays because the exception filter must not touch
// std::string / std::filesystem / any heap object.  Sizes mirror the macOS
// equivalents in telemetry_crash.mm.
static wchar_t g_tombstones_dir_w[1024] = {0};   // pre-converted wide path
static char    g_tombstones_dir[1024]   = {0};
static char    g_session_id[64]         = {0};
static char    g_device_id[64]          = {0};
static char    g_device_name[128]       = {0};
static char    g_device_version[64]     = {0};

static std::atomic<bool> g_installed   { false };
static std::atomic<bool> g_in_handler  { false };

static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter    = nullptr;
static std::terminate_handler       g_prev_terminate  = nullptr;

// ---- Helpers ----------------------------------------------------------------

static void copy_safe(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static const char* exception_code_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_GUARD_PAGE:               return "EXCEPTION_GUARD_PAGE";
        case EXCEPTION_INVALID_HANDLE:           return "EXCEPTION_INVALID_HANDLE";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        default:                                 return "EXCEPTION_UNKNOWN";
    }
}

// Write key=value\n piecewise via WriteFile (no snprintf needed for most
// fields; the name/value are already null-terminated C strings).
static void write_kv(HANDLE h, const char* key, const char* value) {
    if (h == INVALID_HANDLE_VALUE || !key || !value) return;
    DWORD written;
    WriteFile(h, key,   (DWORD)strlen(key),   &written, nullptr);
    WriteFile(h, "=",   1,                     &written, nullptr);
    WriteFile(h, value, (DWORD)strlen(value),  &written, nullptr);
    WriteFile(h, "\n",  1,                     &written, nullptr);
}

static void write_kv_longlong(HANDLE h, const char* key, long long value) {
    if (h == INVALID_HANDLE_VALUE || !key) return;
    char buf[24];
    int  n = std::snprintf(buf, sizeof(buf), "%lld", value);
    if (n <= 0) return;
    DWORD written;
    WriteFile(h, key, (DWORD)strlen(key), &written, nullptr);
    WriteFile(h, "=", 1,                   &written, nullptr);
    WriteFile(h, buf, (DWORD)n,            &written, nullptr);
    WriteFile(h, "\n", 1,                  &written, nullptr);
}

// Build a timestamped tombstone filename and return an open HANDLE, or
// INVALID_HANDLE_VALUE on failure.  CREATE_NEW + a retry loop mirrors the
// O_CREAT | O_EXCL pattern used by the macOS signal handler.
static HANDLE open_tombstone(DWORD exception_code) {
    if (g_tombstones_dir_w[0] == L'\0') return INVALID_HANDLE_VALUE;

    SYSTEMTIME st;
    GetSystemTime(&st);
    DWORD tid = GetCurrentThreadId();

    wchar_t path[1280];
    _snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE,
                 L"%s\\%04d%02d%02d-%02d%02d%02d-%08lX-%lu.tomb",
                 g_tombstones_dir_w,
                 (int)st.wYear, (int)st.wMonth,  (int)st.wDay,
                 (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
                 exception_code, tid);

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    for (int i = 1; h == INVALID_HANDLE_VALUE && i < 16; ++i) {
        wchar_t path2[1320];
        _snwprintf_s(path2, ARRAYSIZE(path2), _TRUNCATE, L"%s.%d", path, i);
        h = CreateFileW(path2, GENERIC_WRITE, 0, nullptr,
                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    return h;
}

static long long filetime_to_unix_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    // FILETIME counts 100-ns intervals since 1601-01-01.
    // Subtract the offset to the Unix epoch (1970-01-01) and convert to ms.
    return static_cast<long long>((ui.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static void write_common_fields(HANDLE h, const char* name, const char* reason) {
    write_kv          (h, "name",           name);
    write_kv          (h, "reason",         reason);
    write_kv_longlong (h, "ts_ms",          filetime_to_unix_ms());
    write_kv          (h, "session",        g_session_id);
    write_kv          (h, "device",         g_device_id);
    write_kv          (h, "device_name",    g_device_name);
    write_kv          (h, "device_version", g_device_version);
}

// ---- Exception filter -------------------------------------------------------

static LONG WINAPI handle_exception(EXCEPTION_POINTERS* ep) {
    // Reentrancy guard: if we crash inside our own handler, chain immediately.
    bool expected = false;
    if (!g_in_handler.compare_exchange_strong(expected, true)) {
        return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;

    // Skip C++ exceptions (the runtime raises 0xE06D7363 to implement throw)
    // and breakpoints — those are not crashes we want to capture.
    if (code == 0xE06D7363u || code == EXCEPTION_BREAKPOINT) {
        g_in_handler.store(false);
        return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    const char* exc_name = exception_code_name(code);
    HANDLE h = open_tombstone(code);
    if (h != INVALID_HANDLE_VALUE) {
        write_common_fields(h, "exception", exc_name);
        write_kv_longlong(h, "thread_id", (long long)GetCurrentThreadId());
        if (ep && ep->ExceptionRecord) {
            char code_buf[16];
            std::snprintf(code_buf, sizeof(code_buf), "0x%08lX",
                          static_cast<unsigned long>(code));
            write_kv(h, "exception_code", code_buf);
            // POTENTIAL ENHANCEMENT: add a backtrace here via StackWalk64 /
            // RtlCaptureStackBackTrace — write frame addresses as hex lines.
        }
        CloseHandle(h);
    }

    g_in_handler.store(false);
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// ---- Terminate handler ------------------------------------------------------

static void handle_terminate() {
    // std::terminate runs after unwinding, so heap / std::string are safe here.
    const char* what = "std::terminate";
    std::string msg;
    try {
        std::exception_ptr ep = std::current_exception();
        if (ep) std::rethrow_exception(ep);
    } catch (const std::exception& ex) {
        msg  = std::string("std::exception: ") + ex.what();
        what = msg.c_str();
    } catch (...) {
        what = "unknown C++ exception";
    }

    if (g_tombstones_dir_w[0] != L'\0') {
        // Build a "terminate" tombstone (no exception code, use 0).
        HANDLE h = open_tombstone(0xFFFFFFFFu);
        if (h != INVALID_HANDLE_VALUE) {
            write_common_fields(h, "terminate", what);
            write_kv_longlong(h, "thread_id", (long long)GetCurrentThreadId());
            CloseHandle(h);
        }
    }

    if (g_prev_terminate) {
        g_prev_terminate();
    } else {
        std::abort();
    }
}

// ---- Public install / uninstall ---------------------------------------------

void install_crash_handlers(const std::string& tombstones_dir,
                             const std::string& session_id,
                             const std::string& device_id,
                             const std::string& device_name,
                             const std::string& device_version) {
    // Atomically mark installed first; the exchange result tells us whether
    // we need to actually install the filter (first call) or just refresh
    // the metadata buffers (subsequent calls).
    const bool already = g_installed.exchange(true);

    // Refresh metadata unconditionally so device_name/version changes stick.
    copy_safe(g_tombstones_dir, sizeof(g_tombstones_dir), tombstones_dir.c_str());
    copy_safe(g_session_id,     sizeof(g_session_id),     session_id.c_str());
    copy_safe(g_device_id,      sizeof(g_device_id),      device_id.c_str());
    copy_safe(g_device_name,    sizeof(g_device_name),    device_name.c_str());
    copy_safe(g_device_version, sizeof(g_device_version), device_version.c_str());

    // Pre-convert the tombstone directory path to wide so the handler never
    // has to do so.  The path arrives as the system ANSI code page (CP_ACP)
    // because it comes from std::filesystem::path::string() on Windows.
    MultiByteToWideChar(CP_ACP, 0,
                        g_tombstones_dir, -1,
                        g_tombstones_dir_w, ARRAYSIZE(g_tombstones_dir_w));

    if (already) return;  // just refreshed metadata; filter already registered

    g_prev_filter    = SetUnhandledExceptionFilter(handle_exception);
    g_prev_terminate = std::set_terminate(handle_terminate);
}

void uninstall_crash_handlers() {
    if (!g_installed.exchange(false)) return;
    SetUnhandledExceptionFilter(g_prev_filter);
    g_prev_filter = nullptr;
    if (g_prev_terminate) {
        std::set_terminate(g_prev_terminate);
        g_prev_terminate = nullptr;
    } else {
        std::set_terminate(nullptr);
    }
}

}}  // namespace bz::telemetry
