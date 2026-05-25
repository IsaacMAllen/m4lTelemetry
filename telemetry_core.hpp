// telemetry_core.hpp
// -----------------------------------------------------------------------------
// Reusable telemetry framework for bugbytz Max for Live devices.
//
// This file declares the public API used by the [bz.telemetry] Max external
// AND, optionally, by other C++ externals that want to log directly without
// going through a Max object.
//
// Design goals
// ------------
//   1. Audio-thread-safe.  No I/O, no allocation hot-paths, no locks held
//      across syscalls.  Logging from any thread enqueues onto a lock-free-ish
//      mutex-protected std::deque and returns immediately.  All disk + network
//      work happens on a single background worker thread.
//
    //   2. Privacy-respecting opt-in.  Nothing is uploaded until the user has
    //      explicitly granted consent (status == granted).  Consent is stored
    //      per-user in the platform app-support directory:
    //        macOS   : ~/Library/Application Support/bugbytz/telemetry/config.json
    //        Windows : %APPDATA%\bugbytz\telemetry\config.json
    //      The file is shared across every bugbytz device on the machine, so
    //      the user only ever has to answer the installer prompt once.
    //
    //   3. Survives offline / crashed sessions.  Pending events are persisted to
    //      disk as one file per event under .../bugbytz/telemetry/pending/ and
    //      drain on the next launch.  Crash tombstones live in a sibling
    //      tombstones/ folder and are converted to events on the next launch.
//
//   4. Crashes themselves are captured asynchronously via std::set_terminate
//      and a small set of opt-in signal handlers that only do
//      async-signal-safe writes to a tombstone file before chaining to the
//      previous handler.  No Mach IPC, no malloc, no Foundation calls inside
//      the signal handler.
//
//   5. Endpoint contract is a single REST POST of newline-delimited JSON
//      events.  See telemetry_http.mm for the on-the-wire schema.
//
// macOS and Windows are fully supported.  Other platforms compile the core
// but use stub no-ops for the HTTP transport and crash hooks.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bz { namespace telemetry {

    // ---------------------------------------------------------------------
    // Public types
    // ---------------------------------------------------------------------

    enum class consent_status {
        unknown = 0,    // user has never been asked
        granted = 1,    // user opted in
        denied  = 2,    // user opted out
    };

    enum class event_kind {
        event   = 0,    // generic analytic event (load, parameter change, ...)
        error   = 1,    // recoverable error (bad input, failed file op, ...)
        metric  = 2,    // numeric metric with optional unit
        crash   = 3,    // captured signal / unhandled exception
    };

    enum class log_level {
        info    = 0,
        warning = 1,
        error   = 2,
        fatal   = 3,
    };

    // A single telemetry event.  Held in memory and serialized to disk +
    // network as JSON.  Keep this POD-ish so it's cheap to copy across the
    // worker thread boundary.
    struct event {
        event_kind kind { event_kind::event };
        log_level  level { log_level::info };
        std::string  name;                                  // e.g. "device_loaded"
        std::string  message;                               // free-form text (errors / crashes)
        double       numeric_value { 0.0 };                 // for metrics
        std::string  unit;                                  // for metrics ("ms", "count", ...)
        std::unordered_map<std::string, std::string> props; // arbitrary string properties
        int64_t      ts_unix_ms { 0 };                      // capture time, set automatically
    };

    // Status snapshot returned by client::status() and pushed to the Max
    // external's outlets every time something interesting changes.
    struct status_snapshot {
        consent_status consent { consent_status::unknown };
        bool           initialized { false };
        bool           uploader_online { false };  // true if last upload attempt succeeded
        int            pending_count { 0 };
        int            sent_count { 0 };
        int            dropped_count { 0 };
        std::string    last_error;                 // last network / I/O error, if any
        std::string    device_id;                  // anonymous, persistent UUID
        std::string    session_id;                 // re-rolled per process
    };

    // ---------------------------------------------------------------------
    // Configuration passed to client::start()
    // ---------------------------------------------------------------------
    struct config {
        std::string endpoint;             // e.g. https://telemetry.bugbytz.com/v1/events
        std::string endpoint_token;       // optional bearer token
        std::string vendor { "bugbytz" }; // top-level grouping
        std::string device_name;          // e.g. "livesaver2_0_4"
        std::string device_version;       // e.g. "2.0.4"
        std::string user_id;              // optional user-provided identifier (e.g. email)
        int   flush_interval_seconds { 30 };
        int   max_queue_files { 5000 };   // soft cap; older files dropped past this
        int   batch_size { 50 };          // events per HTTP POST
        bool  dry_run { false };          // log to disk but never POST (development)
        bool  crash_capture { true };     // install signal + terminate handlers
        bool  log_to_max_console { true };// echo events to the Max console (debugging)
    };

    // ---------------------------------------------------------------------
    // Console logging callback
    // ---------------------------------------------------------------------
    // The framework cannot link against c74::min directly from this header
    // (we don't want to force every consumer to drag in the Min headers), so
    // the Max external installs a small callback that forwards strings to its
    // logger.  Optional — if unset, messages go to stderr.
    using console_logger = void (*)(const char* message, log_level level, void* user);

    // ---------------------------------------------------------------------
    // The client.  Singleton.  Multiple Max externals can call get() and
    // they all share the same uploader thread, queue and consent state.
    // ---------------------------------------------------------------------
    class client {
    public:
        static client& get();

        // Start the framework.  Idempotent — calling twice merges the new
        // config (mostly used so different devices can register their own
        // device_name + device_version while still sharing the queue).
        // Returns true on success.
        bool start(const config& cfg);

        // Stop the worker thread and flush as much as possible (best effort
        // with a small timeout).  Called from atexit / object destructor.
        void stop();

        // Tell us about the device that's currently logging.  This is used
        // to stamp every event with the right device_name/version.
        void set_active_device(const std::string& name, const std::string& version);

        // Set / clear the optional user-supplied identifier (e.g. email).
        void set_user_id(const std::string& user_id);

        // Consent management.  These calls persist to disk synchronously
        // (small JSON write, microseconds-fast).
        void set_consent(consent_status status);
        consent_status get_consent() const;

        // Logging.  All thread-safe.  Drops on the floor if consent != granted
        // OR if the queue is full.  Returns true if the event was queued.
        bool log_event (const std::string& name, const std::unordered_map<std::string,std::string>& props = {});
        bool log_error (const std::string& name, const std::string& message,
                        const std::unordered_map<std::string,std::string>& props = {});
        bool log_metric(const std::string& name, double value, const std::string& unit = {},
                        const std::unordered_map<std::string,std::string>& props = {});
        bool log_crash (const std::string& name, const std::string& message,
                        const std::unordered_map<std::string,std::string>& props = {});

        // Force an immediate flush attempt on the worker thread.  Returns
        // immediately (the actual upload is async).
        void flush();

        // Drop everything currently pending (does NOT touch sent counts).
        // Honoured even when consent != granted.
        void clear_queue();

        // Snapshot of the current state.  Cheap, lock-free read of atomics +
        // a small string copy.
        status_snapshot status() const;

        // Install a callback that receives short status strings (used by the
        // Max external to write to the Max console).  Pass nullptr to unset.
        void set_console_logger(console_logger fn, void* user);

        // Filesystem location used for queue / config / tombstones.  Useful
        // for debugging and for the help patcher's "open in Finder" button.
        std::string storage_dir() const;

    private:
        client();
        ~client();
        client(const client&) = delete;
        client& operator=(const client&) = delete;

        struct impl;
        impl* d;
    };

    // ---------------------------------------------------------------------
    // Functions implemented in telemetry_http.mm and telemetry_crash.mm.
    // Declared here so the core .cpp can call them without dragging in any
    // platform headers.
    // ---------------------------------------------------------------------

    // Synchronous HTTP POST of an ndjson body.  Returns true on 2xx, false
    // otherwise.  On failure, error_out is filled with a short reason.
    // Implemented in telemetry_http.mm on Apple, returns false elsewhere.
    bool http_post_ndjson(const std::string& endpoint,
                          const std::string& bearer_token,
                          const std::string& ndjson_body,
                          std::string&       error_out);

    // Crash-capture install/uninstall.  No-op on non-Apple platforms.
    // The handler writes a tombstone file (relative path inside storage_dir)
    // using only async-signal-safe calls.
    void install_crash_handlers (const std::string& tombstones_dir,
                                 const std::string& session_id,
                                 const std::string& device_id,
                                 const std::string& device_name,
                                 const std::string& device_version);
    void uninstall_crash_handlers();

    // Platform helpers used by the core but easier to write in Objective-C.
    // (App Support directory resolution + macOS version string.)
    std::string platform_app_support_dir();    // ~/Library/Application Support (macOS) / %APPDATA% (Windows)
    std::string platform_os_string();          // "macOS 14.4" / "Windows 10" / etc.
    std::string platform_max_version_hint();   // best-effort host process version
    std::string platform_uuid_v4();            // RFC4122 v4 UUID, lowercase

}}  // namespace bz::telemetry
