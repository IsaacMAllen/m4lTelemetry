// telemetry_crash.mm
// -----------------------------------------------------------------------------
// macOS crash-capture hooks for bz.telemetry.
//
// What this module captures
//   - Fatal POSIX signals on the calling thread:
//       SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT
//     We re-raise after writing the tombstone so the OS can still produce a
//     crash report at ~/Library/Logs/DiagnosticReports/ — important: this
//     module is *additive*, it must not silently swallow crashes.
//
//   - Uncaught C++ exceptions via std::set_terminate.  We chain back to the
//     previously-installed handler so we don't break Apple's terminate path.
//
// What we DON'T do
//   - We don't unwind the stack ourselves.  That requires either DWARF
//     parsing or a third-party library (Crashpad / Backtrace).  For the
//     framework's first version, the crash *fact* + signal name + thread
//     is enough to triage in production.  Adding a backtrace can come
//     later by linking libunwind in this same .mm file and emitting frame
//     addresses into the tombstone — there's a comment in handle_signal()
//     marking the right spot.
//
// Async-signal-safety
//   - Inside the signal handler we call ONLY:
//       open(2) / write(2) / close(2) / signal(2) / sigaction(2) / raise(3)
//     and tiny stdio formatting via snprintf to a stack buffer (snprintf
//     is async-signal-safe per POSIX.1-2008 TC1).
//   - We do NOT touch malloc / NSString / std::string / dispatch / autorelease
//     pools / Foundation / AX / locks of any kind.
//
// Reentrancy
//   - A bool flag prevents the handler from running twice if a follow-on
//     signal fires during the write; in that case we just re-raise.
// -----------------------------------------------------------------------------

#include "telemetry_core.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <exception>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace bz { namespace telemetry {

    // Static state.  All raw C buffers because the signal handler can't
    // touch C++ heap objects safely.  Fixed sizes match what we write into
    // the tombstone: anything longer is truncated.
    static char        g_tombstones_dir[1024]    = {0};
    static char        g_session_id[64]          = {0};
    static char        g_device_id[64]           = {0};
    static char        g_device_name[128]        = {0};
    static char        g_device_version[64]      = {0};
    static std::atomic<bool> g_installed { false };
    static std::atomic<bool> g_in_handler { false };

    static struct sigaction g_prev_sa[NSIG];
    static int               g_signals[]  = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    static const size_t      g_n_signals  = sizeof(g_signals) / sizeof(g_signals[0]);
    static std::terminate_handler g_prev_terminate = nullptr;

    // -------------------------------------------------------------------------
    // Async-signal-safe helpers
    // -------------------------------------------------------------------------
    static void copy_safe(char* dst, size_t cap, const char* src) {
        if (!dst || cap == 0) return;
        if (!src) { dst[0] = 0; return; }
        size_t i = 0;
        while (i + 1 < cap && src[i]) { dst[i] = src[i]; ++i; }
        dst[i] = 0;
    }

    static const char* signal_name(int sig) {
        switch (sig) {
            case SIGSEGV: return "SIGSEGV";
            case SIGBUS:  return "SIGBUS";
            case SIGFPE:  return "SIGFPE";
            case SIGILL:  return "SIGILL";
            case SIGABRT: return "SIGABRT";
            default:      return "SIG?";
        }
    }

    // Build an async-signal-safe filename of the form
    //   <tombstones_dir>/<sec>-<sig>-<tid>.tomb
    // and return an open fd, or -1 on failure.
    static int open_tombstone(int sig) {
        if (g_tombstones_dir[0] == 0) return -1;
        char path[1280];
        // time(NULL) is async-signal-safe.
        long secs = (long)time(NULL);
        unsigned long tid = (unsigned long)pthread_mach_thread_np(pthread_self());
        int n = std::snprintf(path, sizeof(path), "%s/%ld-%d-%lu.tomb",
                              g_tombstones_dir, secs, sig, tid);
        if (n <= 0 || n >= (int)sizeof(path)) return -1;
        // O_CREAT | O_EXCL prevents a follow-on crash from clobbering the
        // first one; if it already exists, we just append a counter.
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            for (int i = 1; i < 16 && fd < 0; ++i) {
                char path2[1320];
                int n2 = std::snprintf(path2, sizeof(path2), "%s.%d", path, i);
                if (n2 <= 0 || n2 >= (int)sizeof(path2)) break;
                fd = open(path2, O_WRONLY | O_CREAT | O_EXCL, 0644);
            }
        }
        return fd;
    }

    static void write_kv(int fd, const char* key, const char* value) {
        if (fd < 0) return;
        if (!key || !value) return;
        // We could format with snprintf, but writing piecewise is faster and
        // avoids one more stack buffer.
        write(fd, key, std::strlen(key));
        write(fd, "=", 1);
        write(fd, value, std::strlen(value));
        write(fd, "\n", 1);
    }

    static void write_kv_long(int fd, const char* key, long long value) {
        if (fd < 0) return;
        char buf[40];
        int n = std::snprintf(buf, sizeof(buf), "%lld", value);
        if (n <= 0) return;
        write(fd, key, std::strlen(key));
        write(fd, "=", 1);
        write(fd, buf, (size_t)n);
        write(fd, "\n", 1);
    }

    // -------------------------------------------------------------------------
    // The actual signal handler.
    // -------------------------------------------------------------------------
    static void handle_signal(int sig, siginfo_t* info, void* uctx) {
        // Reentrancy guard: if we crash inside our own handler, just chain.
        bool expected = false;
        if (!g_in_handler.compare_exchange_strong(expected, true)) {
            // Re-raise after restoring the previous action.
            if (sig >= 0 && sig < NSIG) {
                sigaction(sig, &g_prev_sa[sig], nullptr);
            }
            raise(sig);
            return;
        }

        int fd = open_tombstone(sig);
        if (fd >= 0) {
            const char* sname = signal_name(sig);
            write_kv     (fd, "name",           "signal");
            write_kv     (fd, "reason",         sname);
            write_kv_long(fd, "ts_ms",          (long long)time(NULL) * 1000LL);
            write_kv     (fd, "session",        g_session_id);
            write_kv     (fd, "device",         g_device_id);
            write_kv     (fd, "device_name",    g_device_name);
            write_kv     (fd, "device_version", g_device_version);
            write_kv_long(fd, "signal_number",  (long long)sig);
            if (info) {
                write_kv_long(fd, "si_code",      (long long)info->si_code);
                write_kv_long(fd, "si_addr",      (long long)(uintptr_t)info->si_addr);
            }
            // POTENTIAL ENHANCEMENT: emit a backtrace here using libunwind.
            // Keep it short (16 frames) to stay within signal-stack budgets.
            (void)uctx;
            close(fd);
        }

        // Restore the previous handler and re-raise so the OS still produces
        // its standard crash report.  This is the critical "additive" step.
        if (sig >= 0 && sig < NSIG) {
            sigaction(sig, &g_prev_sa[sig], nullptr);
        }
        raise(sig);
    }

    // -------------------------------------------------------------------------
    // C++ unhandled-exception path
    // -------------------------------------------------------------------------
    static void handle_terminate() {
        // We DO have heap + std::string here — std::terminate runs on the
        // crashing thread *after* unwinding, so it's allowed to allocate.
        // We still write via raw POSIX I/O for symmetry with the signal path.
        const char* what = "std::terminate";
        std::string msg;
        try {
            std::exception_ptr ep = std::current_exception();
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& ex) {
            msg = std::string("std::exception: ") + ex.what();
            what = msg.c_str();
        } catch (...) {
            what = "unknown C++ exception";
        }

        if (g_tombstones_dir[0] != 0) {
            char path[1280];
            long secs = (long)time(NULL);
            std::snprintf(path, sizeof(path), "%s/%ld-terminate.tomb",
                          g_tombstones_dir, secs);
            int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd >= 0) {
                write_kv     (fd, "name",           "terminate");
                write_kv     (fd, "reason",         what);
                write_kv_long(fd, "ts_ms",          (long long)time(NULL) * 1000LL);
                write_kv     (fd, "session",        g_session_id);
                write_kv     (fd, "device",         g_device_id);
                write_kv     (fd, "device_name",    g_device_name);
                write_kv     (fd, "device_version", g_device_version);
                close(fd);
            }
        }

        if (g_prev_terminate) {
            g_prev_terminate();
        } else {
            std::abort();
        }
    }

    // -------------------------------------------------------------------------
    // Public install / uninstall
    // -------------------------------------------------------------------------
    void install_crash_handlers(const std::string& tombstones_dir,
                                const std::string& session_id,
                                const std::string& device_id,
                                const std::string& device_name,
                                const std::string& device_version) {
        bool already = g_installed.exchange(true);
        copy_safe(g_tombstones_dir, sizeof(g_tombstones_dir), tombstones_dir.c_str());
        copy_safe(g_session_id,     sizeof(g_session_id),     session_id.c_str());
        copy_safe(g_device_id,      sizeof(g_device_id),      device_id.c_str());
        copy_safe(g_device_name,    sizeof(g_device_name),    device_name.c_str());
        copy_safe(g_device_version, sizeof(g_device_version), device_version.c_str());
        if (already) return;  // installed once, just refresh the metadata

        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        sa.sa_sigaction = handle_signal;
        sigemptyset(&sa.sa_mask);
        for (size_t i = 0; i < g_n_signals; ++i) {
            int s = g_signals[i];
            std::memset(&g_prev_sa[s], 0, sizeof(g_prev_sa[s]));
            sigaction(s, &sa, &g_prev_sa[s]);
        }

        g_prev_terminate = std::set_terminate(handle_terminate);
    }

    void uninstall_crash_handlers() {
        if (!g_installed.exchange(false)) return;
        for (size_t i = 0; i < g_n_signals; ++i) {
            int s = g_signals[i];
            sigaction(s, &g_prev_sa[s], nullptr);
        }
        if (g_prev_terminate) {
            std::set_terminate(g_prev_terminate);
            g_prev_terminate = nullptr;
        } else {
            std::set_terminate(nullptr);
        }
    }

}}  // namespace bz::telemetry
