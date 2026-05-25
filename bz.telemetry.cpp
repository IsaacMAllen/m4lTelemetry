// bz.telemetry.cpp
// -----------------------------------------------------------------------------
// Max external [bz.telemetry]
//
// One drop-in object that turns any bugbytz Max for Live device into an
// opt-in, error-reporting / analytics-emitting client.  The actual
// framework lives in telemetry_core.* (so other C++ externals in this
// repo can use it directly without going through a Max object); this
// file is just the user-facing Max wrapper.
//
// Typical M4L wiring
// ------------------
//   [bz.telemetry @device "livesaver" @version "2.0.4"]
//        |  |  |  |
//        |  |  |  +-- last_error  (symbol)  for status/troubleshooting
//        |  |  +----- queue_status (list: <pending> <sent> <dropped>)
//        |  +-------- online       (int 0/1)
//        +----------- consent      (symbol: unknown / granted / denied)
//
// Boot-time installer prompt
// --------------------------
//   On load, the external emits its current consent state out outlet 0.
//   The device's M4L patcher should:
//     - listen for `consent unknown` and show its installer dialog
//     - send `consent_grant` or `consent_revoke` to inlet 0 in response
//     - listen for `consent granted` to enable the rest of the device
//   Once consent is recorded, the answer persists across all bugbytz
//   devices on the machine, so the user is only ever asked once.
//
// Logging API (messages on inlet 0)
// ---------------------------------
//   log_event   <name> [key value]...
//   log_error   <name> <message> [key value]...
//   log_metric  <name> <number>  [unit] [key value]...
//   report_crash <name> <message> [key value]...
//   set_user    <user_id>           // optional, e.g. an email
//   flush                            // force an upload attempt now
//   clear_queue                      // drop pending events
//   status                           // re-emit all status outlets
//
// Lifecycle is shared via a singleton (bz::telemetry::client::get()) so
// you can drop multiple [bz.telemetry] objects into different devices
// without spawning multiple uploader threads or duplicate queues.
// -----------------------------------------------------------------------------

#include "c74_min.h"
#include "telemetry_core.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

using namespace c74::min;

class bz_telemetry : public object<bz_telemetry> {
public:
    MIN_DESCRIPTION { "Opt-in error reporting & analytics framework for bugbytz Max for Live devices." };
    MIN_TAGS        { "telemetry, framework" };
    MIN_AUTHOR      { "bugbytz" };
    MIN_RELATED     { "live_save, livesaver, counterbugbytz" };

    inlet<>  in_main         { this, "(messages) log / consent / control" };
    outlet<> out_consent     { this, "(symbol) consent status (unknown/granted/denied)" };
    outlet<> out_online      { this, "(int) 1 if last upload succeeded, else 0" };
    outlet<> out_queue       { this, "(list) pending sent dropped" };
    outlet<> out_status      { this, "(symbol/list) last error / device id / etc." };

    // ------------------------------------------------------------------
    // Attributes
    // ------------------------------------------------------------------
    attribute<symbol> a_endpoint { this, "endpoint", "",
        title       { "Endpoint URL" },
        description { "REST endpoint that receives telemetry POSTs.  "
                      "If empty the device queues to disk only." },
        setter { MIN_FUNCTION {
            push_config_to_client();
            return args;
        }}
    };

    attribute<symbol> a_token { this, "token", "",
        title       { "Endpoint Token" },
        description { "Optional bearer token sent in the Authorization header." },
        setter { MIN_FUNCTION {
            push_config_to_client();
            return args;
        }}
    };

    attribute<symbol> a_vendor { this, "vendor", "bugbytz",
        title       { "Vendor" },
        description { "Top-level grouping under which all events are stamped.  "
                      "Also names the storage subdirectory under "
                      "~/Library/Application Support/." },
        setter { MIN_FUNCTION {
            push_config_to_client();
            return args;
        }}
    };

    attribute<symbol> a_device { this, "device", "",
        title       { "Device Name" },
        description { "Name of the Max device hosting this telemetry instance, "
                      "e.g. \"livesaver\".  Stamped on every event." },
        setter { MIN_FUNCTION {
            push_config_to_client();
            return args;
        }}
    };

    attribute<symbol> a_version { this, "version", "",
        title       { "Device Version" },
        description { "Semantic version of the host device, e.g. \"2.0.4\".  "
                      "Lets you correlate errors with releases." },
        setter { MIN_FUNCTION {
            push_config_to_client();
            return args;
        }}
    };

    attribute<int> a_flush_interval { this, "flush_interval", 30,
        title       { "Flush Interval (seconds)" },
        description { "How often the worker thread tries to upload pending events." },
        setter { MIN_FUNCTION {
            push_config_to_client();
            return args;
        }}
    };

    attribute<bool> a_dry_run { this, "dry_run", false,
        title       { "Dry Run" },
        description { "If on, queue and serialize events but never POST them.  "
                      "Use during development." },
        setter { MIN_FUNCTION {
            push_config_to_client();
            return args;
        }}
    };

    attribute<bool> a_crash_capture { this, "crash_capture", true,
        title       { "Capture Crashes" },
        description { "If on, install signal + std::terminate handlers that "
                      "write tombstone files for upload on the next launch." },
        setter { MIN_FUNCTION {
            push_config_to_client();
            return args;
        }}
    };

    // ------------------------------------------------------------------
    // Construction / destruction
    //
    // The framework is a process-wide singleton.  Multiple [bz.telemetry]
    // boxes (one per device) all share its uploader thread and queue, so
    // the destructor here is intentionally minimal — we let the OS reap
    // the singleton at process exit, at which point client::~client()
    // flushes and joins the worker.
    // ------------------------------------------------------------------
    bz_telemetry(const atoms& args = {}) { (void)args; }
    ~bz_telemetry()                      {}

    // ------------------------------------------------------------------
    // Boot — fires after the patcher has fully loaded AND every @attribute
    // argument has been applied.  This is the right place to read the
    // attributes (in `setup` they're still defaults) and to start the
    // periodic status poll.
    // ------------------------------------------------------------------
    message<> m_loadbang { this, "loadbang",
        MIN_FUNCTION {
            push_config_to_client();
            emit_status(true);

            std::unordered_map<std::string,std::string> p;
            p["build"] = __DATE__ " " __TIME__;
            bz::telemetry::client::get().log_event("device_loaded", p);

            // Start the periodic status refresh so the M4L UI's
            // "online / N pending" indicator stays fresh.
            m_poll.delay(2000.0);
            return {};
        }
    };

    timer<> m_poll { this,
        MIN_FUNCTION {
            emit_status(false);
            m_poll.delay(2000.0);
            return {};
        }
    };

    // ------------------------------------------------------------------
    // Consent management
    // ------------------------------------------------------------------
    message<> m_consent_grant { this, "consent_grant",
        "User opted in.  Persists across all bugbytz devices on this machine.",
        MIN_FUNCTION {
            bz::telemetry::client::get().set_consent(bz::telemetry::consent_status::granted);
            cout << "[bz.telemetry] consent: granted" << endl;
            emit_status(true);
            std::unordered_map<std::string,std::string> p;
            p["from_device"] = device_name();
            bz::telemetry::client::get().log_event("consent_granted", p);
            return {};
        }
    };

    message<> m_consent_revoke { this, "consent_revoke",
        "User opted out.  Pending queue is dropped immediately.",
        MIN_FUNCTION {
            // Log first (will only land if previously consented), THEN flip.
            std::unordered_map<std::string,std::string> p;
            p["from_device"] = device_name();
            bz::telemetry::client::get().log_event("consent_revoked", p);
            bz::telemetry::client::get().flush();
            bz::telemetry::client::get().set_consent(bz::telemetry::consent_status::denied);
            cout << "[bz.telemetry] consent: denied" << endl;
            emit_status(true);
            return {};
        }
    };

    message<> m_consent_reset { this, "consent_reset",
        "Reset consent to unknown so the installer prompt is shown again.",
        MIN_FUNCTION {
            bz::telemetry::client::get().set_consent(bz::telemetry::consent_status::unknown);
            cout << "[bz.telemetry] consent: unknown (will re-prompt)" << endl;
            emit_status(true);
            return {};
        }
    };

    // ------------------------------------------------------------------
    // Logging messages
    // ------------------------------------------------------------------
    // NOTE: keep description strings free of '<' and '>' — Min's auto-generated
    // bz.telemetry.maxref.xml only HTML-escapes '&', not the angle brackets,
    // so any '<foo>' placeholder syntax breaks Max's XML doc parser.
    message<> m_log_event { this, "log_event",
        "log_event name [key value]...  Log a custom analytics event.",
        MIN_FUNCTION {
            if (args.empty()) {
                cerr << "[bz.telemetry] log_event requires a name" << endl;
                return {};
            }
            auto name  = std::string(args[0]);
            auto props = atoms_to_props(args, 1);
            bz::telemetry::client::get().log_event(name, props);
            emit_status(false);
            return {};
        }
    };

    message<> m_log_error { this, "log_error",
        "log_error name message [key value]...  Log a non-fatal error.",
        MIN_FUNCTION {
            if (args.size() < 2) {
                cerr << "[bz.telemetry] log_error requires <name> <message>" << endl;
                return {};
            }
            auto name = std::string(args[0]);
            auto msg  = std::string(args[1]);
            auto p    = atoms_to_props(args, 2);
            bz::telemetry::client::get().log_error(name, msg, p);
            emit_status(false);
            return {};
        }
    };

    message<> m_log_metric { this, "log_metric",
        "log_metric name number [unit] [key value]...  Log a numeric metric.",
        MIN_FUNCTION {
            if (args.size() < 2) {
                cerr << "[bz.telemetry] log_metric requires <name> <number>" << endl;
                return {};
            }
            auto name  = std::string(args[0]);
            double v   = static_cast<double>(args[1]);
            std::string unit;
            size_t props_start = 2;
            // If args[2] is a symbol AND args.size() is even, treat it as
            // the unit; otherwise treat args[2] onward as key/value props.
            if (args.size() >= 3 && (args.size() - 2) % 2 == 1) {
                unit = std::string(args[2]);
                props_start = 3;
            }
            auto p = atoms_to_props(args, props_start);
            bz::telemetry::client::get().log_metric(name, v, unit, p);
            emit_status(false);
            return {};
        }
    };

    message<> m_report_crash { this, "report_crash",
        "report_crash name message [key value]...  Manually report a fatal "
        "condition (e.g. an exception caught in script code).",
        MIN_FUNCTION {
            if (args.size() < 2) {
                cerr << "[bz.telemetry] report_crash requires <name> <message>" << endl;
                return {};
            }
            auto name = std::string(args[0]);
            auto msg  = std::string(args[1]);
            auto p    = atoms_to_props(args, 2);
            bz::telemetry::client::get().log_crash(name, msg, p);
            emit_status(false);
            return {};
        }
    };

    message<> m_set_user { this, "set_user",
        "set_user id  Attach an optional user identifier (e.g. email) to "
        "subsequent events.  Pass an empty symbol to clear.",
        MIN_FUNCTION {
            std::string u = args.empty() ? std::string() : std::string(args[0]);
            bz::telemetry::client::get().set_user_id(u);
            return {};
        }
    };

    message<> m_flush { this, "flush",
        "Force an immediate upload attempt.",
        MIN_FUNCTION {
            bz::telemetry::client::get().flush();
            emit_status(true);
            return {};
        }
    };

    message<> m_clear_queue { this, "clear_queue",
        "Drop all pending events from disk without uploading.",
        MIN_FUNCTION {
            bz::telemetry::client::get().clear_queue();
            emit_status(true);
            return {};
        }
    };

    message<> m_status { this, "status",
        "Re-emit consent + queue status out the outlets.",
        MIN_FUNCTION {
            emit_status(true);
            return {};
        }
    };

    message<> m_storage_dir { this, "storage_dir",
        "Print the on-disk storage directory to the Max console.",
        MIN_FUNCTION {
            cout << "[bz.telemetry] storage: "
                 << bz::telemetry::client::get().storage_dir() << endl;
            return {};
        }
    };

    // bang == status
    message<> m_bang { this, "bang",
        MIN_FUNCTION {
            emit_status(true);
            return {};
        }
    };

private:
    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------
    // Read an attribute<symbol> as a std::string.  We need static_cast to
    // `const symbol&` (not `symbol`) because attribute<T> exposes BOTH
    // `operator const T&() const` and `operator T&()`, which makes a plain
    // copy-cast ambiguous.
    static std::string sym_str(const attribute<symbol>& a) {
        return std::string(static_cast<const symbol&>(a));
    }

    std::string device_name() const {
        auto s = sym_str(a_device);
        return s.empty() ? std::string("unknown") : s;
    }

    // Forward "[name=value]" pairs from atoms into a string-keyed map.
    static std::unordered_map<std::string,std::string>
    atoms_to_props(const atoms& as, size_t start_index) {
        std::unordered_map<std::string,std::string> out;
        for (size_t i = start_index; i + 1 < as.size(); i += 2) {
            std::string k = std::string(as[i]);
            std::string v = std::string(as[i + 1]);
            if (!k.empty()) out[k] = v;
        }
        return out;
    }

    // Re-push every attribute into the singleton.  Idempotent.
    //
    // CRITICAL: this is guarded on object_base::initialized().  Min applies
    // each attribute's default value during the attribute's own constructor,
    // which fires the setter BEFORE the *other* attributes have been
    // constructed.  If we read those uninitialized members we crash inside
    // ext_main with a wild-pointer SIGSEGV.  Min flips initialized() to
    // true once the whole min-object has been postinitialized, AT WHICH
    // POINT it's safe to read every attribute member — and importantly,
    // attr_args_process (which applies the @attr arguments from the box
    // text) and the periodic setter calls all fire AFTER that flip, so
    // we don't lose any config updates.
    void push_config_to_client() {
        if (!this->initialized()) {
            return;
        }
        bz::telemetry::config cfg;
        cfg.endpoint               = sym_str(a_endpoint);
        cfg.endpoint_token         = sym_str(a_token);
        cfg.vendor                 = sym_str(a_vendor);
        if (cfg.vendor.empty()) cfg.vendor = "bugbytz";
        cfg.device_name            = sym_str(a_device);
        cfg.device_version         = sym_str(a_version);
        cfg.flush_interval_seconds = static_cast<int>(a_flush_interval);
        cfg.dry_run                = static_cast<bool>(a_dry_run);
        cfg.crash_capture          = static_cast<bool>(a_crash_capture);
        bz::telemetry::client::get().start(cfg);
        bz::telemetry::client::get().set_active_device(cfg.device_name, cfg.device_version);
    }

    // Push the current snapshot out our outlets.  When `verbose` is true
    // we also print the device id + last error to the Max console (used
    // on consent changes / user requests / boot).  In the periodic poll
    // path we keep it quiet to avoid spamming the console.
    void emit_status(bool verbose) {
        auto s = bz::telemetry::client::get().status();

        symbol cs = "unknown";
        if (s.consent == bz::telemetry::consent_status::granted) cs = "granted";
        if (s.consent == bz::telemetry::consent_status::denied)  cs = "denied";
        out_consent.send(cs);
        out_online.send(s.uploader_online ? 1 : 0);
        out_queue.send("queue", s.pending_count, s.sent_count, s.dropped_count);
        if (!s.last_error.empty()) {
            out_status.send("last_error", symbol(s.last_error));
        }
        if (verbose) {
            out_status.send("device_id", symbol(s.device_id));
            out_status.send("session_id", symbol(s.session_id));
            out_status.send("storage", symbol(bz::telemetry::client::get().storage_dir()));
        }
    }

};

MIN_EXTERNAL(bz_telemetry);
