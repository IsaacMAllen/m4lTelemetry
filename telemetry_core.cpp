// telemetry_core.cpp
// -----------------------------------------------------------------------------
// Implementation of bz::telemetry::client.
//
// Architecture
//   - One singleton client; one std::thread that wakes on a condition variable.
//   - Logging from any thread serializes the event to a JSON file under
//     pending/ and notifies the worker.  The worker scans pending/ in
//     mtime order, batches up to cfg.batch_size events, hands them to
//     http_post_ndjson(), and on success deletes the files it just sent.
//   - On startup the worker also drains tombstones/ — files written by the
//     crash handlers in telemetry_crash.mm during the previous run — and
//     converts each one into a crash event.
//
// The JSON is hand-rolled.  We keep this dependency-free on purpose so the
// framework can be embedded in any external in this repo without a vendor
// drop-in.  The schema is strict but small (string keys, string + number
// values, no nesting beyond `props`), so a 30-line escaper is enough.
// -----------------------------------------------------------------------------

#include "telemetry_core.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace bz { namespace telemetry {

    // -------------------------------------------------------------------------
    // JSON helpers.  Minimal — string + number values only, no embedded
    // structures (we flatten props to a {string: string} map).
    // -------------------------------------------------------------------------
    namespace json {

        static std::string escape(const std::string& s) {
            std::string out;
            out.reserve(s.size() + 2);
            for (char c : s) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out += c;
                        }
                }
            }
            return out;
        }

        static std::string number(double v) {
            // %.17g round-trips IEEE-754 doubles losslessly.  We strip a
            // trailing ".0" because the receiving side often parses ints.
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", v);
            return buf;
        }

        // Look up a top-level "key": "value" or "key": value pair from a
        // JSON document.  Cheap & permissive — fine for the trivial config
        // file we own, NOT a general-purpose parser.
        static std::string find_string_field(const std::string& doc, const std::string& key) {
            std::string needle = "\"" + key + "\"";
            auto pos = doc.find(needle);
            if (pos == std::string::npos) return {};
            pos = doc.find(':', pos + needle.size());
            if (pos == std::string::npos) return {};
            ++pos;
            while (pos < doc.size() && (doc[pos] == ' ' || doc[pos] == '\t')) ++pos;
            if (pos >= doc.size() || doc[pos] != '"') return {};
            ++pos;
            std::string out;
            while (pos < doc.size() && doc[pos] != '"') {
                if (doc[pos] == '\\' && pos + 1 < doc.size()) {
                    char n = doc[pos + 1];
                    switch (n) {
                        case '"':  out += '"';  break;
                        case '\\': out += '\\'; break;
                        case 'n':  out += '\n'; break;
                        case 't':  out += '\t'; break;
                        default:   out += n;    break;
                    }
                    pos += 2;
                } else {
                    out += doc[pos++];
                }
            }
            return out;
        }

    }  // namespace json

    // -------------------------------------------------------------------------
    // Time helpers
    // -------------------------------------------------------------------------
    static int64_t now_unix_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    static std::string iso8601_utc(int64_t unix_ms) {
        time_t secs = static_cast<time_t>(unix_ms / 1000);
        int    ms   = static_cast<int>(unix_ms % 1000);
        if (ms < 0) { ms += 1000; secs -= 1; }
        struct tm gm;
#if defined(_WIN32)
        gmtime_s(&gm, &secs);
#else
        gmtime_r(&secs, &gm);
#endif
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      gm.tm_year + 1900, gm.tm_mon + 1, gm.tm_mday,
                      gm.tm_hour, gm.tm_min, gm.tm_sec, ms);
        return buf;
    }

    // -------------------------------------------------------------------------
    // Event -> JSON line
    // -------------------------------------------------------------------------
    static const char* kind_str(event_kind k) {
        switch (k) {
            case event_kind::error:  return "error";
            case event_kind::metric: return "metric";
            case event_kind::crash:  return "crash";
            case event_kind::event:
            default:                 return "event";
        }
    }
    static const char* level_str(log_level l) {
        switch (l) {
            case log_level::warning: return "warning";
            case log_level::error:   return "error";
            case log_level::fatal:   return "fatal";
            case log_level::info:
            default:                 return "info";
        }
    }

    // Serialize a single event (without envelope) to JSON.  Envelope fields
    // (vendor / device_id / session_id / etc.) are added by the worker just
    // before upload, because they may not yet be known at log time.
    static std::string serialize_event(const event& e) {
        std::ostringstream os;
        os << "{";
        os << "\"type\":\""  << kind_str(e.kind)   << "\",";
        os << "\"level\":\"" << level_str(e.level) << "\",";
        os << "\"name\":\""  << json::escape(e.name) << "\",";
        os << "\"ts\":\""    << iso8601_utc(e.ts_unix_ms) << "\",";
        os << "\"ts_ms\":"   << e.ts_unix_ms;
        if (!e.message.empty()) {
            os << ",\"message\":\"" << json::escape(e.message) << "\"";
        }
        if (e.kind == event_kind::metric) {
            os << ",\"value\":" << json::number(e.numeric_value);
            if (!e.unit.empty()) {
                os << ",\"unit\":\"" << json::escape(e.unit) << "\"";
            }
        }
        if (!e.props.empty()) {
            os << ",\"props\":{";
            bool first = true;
            for (const auto& kv : e.props) {
                if (!first) os << ",";
                first = false;
                os << "\"" << json::escape(kv.first) << "\":\""
                   << json::escape(kv.second) << "\"";
            }
            os << "}";
        }
        os << "}";
        return os.str();
    }

    // -------------------------------------------------------------------------
    // Implementation struct
    // -------------------------------------------------------------------------
    struct client::impl {
        // Configuration (mutable, guarded by cfg_mutex_).
        mutable std::mutex cfg_mutex_;
        config             cfg;
        std::atomic<bool>  initialized { false };

        // Consent.
        mutable std::mutex     consent_mutex_;
        consent_status         consent { consent_status::unknown };

        // Identifiers.
        std::string device_id;
        std::string session_id;

        // Filesystem layout.
        fs::path root_dir;       // ~/Library/Application Support/bugbytz/telemetry
        fs::path config_path;    // .../config.json
        fs::path pending_dir;    // .../pending
        fs::path tombstones_dir; // .../tombstones

        // Worker thread.
        std::thread             worker;
        std::mutex              worker_mutex;
        std::condition_variable worker_cv;
        std::atomic<bool>       worker_should_exit { false };
        std::atomic<bool>       worker_flush_now   { false };

        // Counters / status (atomics so status() is lock-free).
        std::atomic<int>        pending_count { 0 };
        std::atomic<int>        sent_count    { 0 };
        std::atomic<int>        dropped_count { 0 };
        std::atomic<bool>       uploader_online { false };

        // Last error string (mutex-guarded so we can copy it).
        mutable std::mutex      last_error_mutex_;
        std::string             last_error;

        // Console logger callback.
        console_logger          logger_fn { nullptr };
        void*                   logger_user { nullptr };
        std::mutex              logger_mutex;

        // -----------------------------------------------------------------
        // Helpers
        // -----------------------------------------------------------------
        void log(const std::string& msg, log_level lvl) {
            console_logger fn = nullptr;
            void* user = nullptr;
            {
                std::lock_guard<std::mutex> g(logger_mutex);
                fn = logger_fn;
                user = logger_user;
            }
            if (fn) {
                fn(msg.c_str(), lvl, user);
            } else {
                std::fprintf(stderr, "[bz.telemetry] %s\n", msg.c_str());
            }
        }

        void set_last_error(const std::string& s) {
            std::lock_guard<std::mutex> g(last_error_mutex_);
            last_error = s;
        }

        std::string get_last_error() const {
            std::lock_guard<std::mutex> g(last_error_mutex_);
            return last_error;
        }

        config snapshot_cfg() const {
            std::lock_guard<std::mutex> g(cfg_mutex_);
            return cfg;
        }

        // -----------------------------------------------------------------
        // Filesystem setup + config persistence
        // -----------------------------------------------------------------
        void init_storage(const std::string& vendor) {
            std::string app_support = platform_app_support_dir();
            if (app_support.empty()) {
                // Fallback for unusual environments.
                const char* home = std::getenv("HOME");
                app_support = home ? std::string(home) + "/.bugbytz" : std::string(".");
            }
            root_dir       = fs::path(app_support) / vendor / "telemetry";
            config_path    = root_dir / "config.json";
            pending_dir    = root_dir / "pending";
            tombstones_dir = root_dir / "tombstones";

            std::error_code ec;
            fs::create_directories(pending_dir,    ec);
            fs::create_directories(tombstones_dir, ec);
        }

        // Load consent + device_id from config.json, creating it if missing.
        // The JSON is small (~200 bytes) so a hand-rolled parser is plenty.
        void load_or_create_config() {
            std::ifstream in(config_path);
            if (in) {
                std::stringstream ss;
                ss << in.rdbuf();
                std::string doc = ss.str();
                std::string c   = json::find_string_field(doc, "consent");
                std::string d   = json::find_string_field(doc, "device_id");
                if      (c == "granted") consent = consent_status::granted;
                else if (c == "denied")  consent = consent_status::denied;
                else                     consent = consent_status::unknown;
                if (!d.empty()) device_id = d;
            }
            if (device_id.empty()) {
                device_id = platform_uuid_v4();
                save_config();
            }
        }

        // Atomic write: write to .tmp, rename over the real file.
        void save_config() {
            fs::path tmp = config_path;
            tmp += ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out) return;
                const char* c = "unknown";
                {
                    std::lock_guard<std::mutex> g(consent_mutex_);
                    if      (consent == consent_status::granted) c = "granted";
                    else if (consent == consent_status::denied)  c = "denied";
                }
                out << "{\n"
                    << "  \"consent\": \""   << c           << "\",\n"
                    << "  \"device_id\": \"" << device_id   << "\",\n"
                    << "  \"version\": 1\n"
                    << "}\n";
            }
            std::error_code ec;
            fs::rename(tmp, config_path, ec);
            if (ec) {
                // Fall back to copy + remove if rename failed cross-device.
                fs::copy_file(tmp, config_path, fs::copy_options::overwrite_existing, ec);
                fs::remove(tmp, ec);
            }
        }

        // Recompute pending_count from the filesystem.  Used at startup and
        // periodically by the worker.
        void recount_pending() {
            int n = 0;
            std::error_code ec;
            for (auto it = fs::directory_iterator(pending_dir, ec);
                 !ec && it != fs::directory_iterator();
                 it.increment(ec)) {
                if (it->is_regular_file()) ++n;
            }
            pending_count.store(n, std::memory_order_relaxed);
        }

        // -----------------------------------------------------------------
        // Enqueue: serialize one event to pending/<ts>-<rand>.json
        // -----------------------------------------------------------------
        // This runs on the caller's thread.  Cheap (a few hundred bytes
        // written to disk).  The worker will pick it up via inotify-like
        // polling on its next wakeup (CV signalled here).
        bool enqueue(const event& e) {
            // Honour consent.  Errors / crashes are dropped silently when
            // consent != granted — that's the whole point of opt-in.
            {
                std::lock_guard<std::mutex> g(consent_mutex_);
                if (consent != consent_status::granted) {
                    dropped_count.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
            }

            // Soft cap.  If we're way over budget, drop the new event rather
            // than letting the disk fill.
            int max_files = 5000;
            {
                std::lock_guard<std::mutex> g(cfg_mutex_);
                max_files = cfg.max_queue_files;
            }
            if (pending_count.load(std::memory_order_relaxed) > max_files) {
                dropped_count.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            std::string body = serialize_event(e);
            // Use ts_ms-<uuid> so the lex-sorted directory listing is in
            // chronological order (helps batching + debuggability).
            char fname[96];
            std::string uid = platform_uuid_v4();
            // First 12 chars of UUID is plenty to disambiguate within a ms.
            std::snprintf(fname, sizeof(fname), "%020lld-%.12s.json",
                          static_cast<long long>(e.ts_unix_ms),
                          uid.c_str());
            fs::path file = pending_dir / fname;
            fs::path tmp  = file;
            tmp += ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out) {
                    set_last_error("failed to open pending tmp file");
                    dropped_count.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                out << body;
            }
            std::error_code ec;
            fs::rename(tmp, file, ec);
            if (ec) {
                set_last_error(std::string("rename pending failed: ") + ec.message());
                fs::remove(tmp, ec);
                dropped_count.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            pending_count.fetch_add(1, std::memory_order_relaxed);

            // Wake worker.
            worker_cv.notify_all();
            return true;
        }

        // -----------------------------------------------------------------
        // Crash tombstone drain.  Tombstones are written by signal handlers
        // (telemetry_crash.mm) using only async-signal-safe calls — they're
        // tiny text files of the form:
        //     reason=SIGSEGV
        //     name=signal:SIGSEGV
        //     session=<id>
        //     device=<id>
        //     device_name=<name>
        //     device_version=<version>
        //     ts_ms=<int>
        // We convert each one to a crash event and delete the file.
        // -----------------------------------------------------------------
        void drain_tombstones() {
            std::error_code ec;
            for (auto it = fs::directory_iterator(tombstones_dir, ec);
                 !ec && it != fs::directory_iterator();
                 it.increment(ec)) {
                if (!it->is_regular_file()) continue;
                fs::path p = it->path();
                std::ifstream in(p);
                if (!in) continue;
                std::stringstream ss;
                ss << in.rdbuf();
                in.close();

                std::string body = ss.str();
                event ev;
                ev.kind  = event_kind::crash;
                ev.level = log_level::fatal;
                ev.name  = "crash";
                ev.ts_unix_ms = now_unix_ms();

                // Parse simple key=value lines.
                size_t pos = 0;
                while (pos < body.size()) {
                    size_t eol = body.find('\n', pos);
                    std::string line = body.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
                    pos = (eol == std::string::npos) ? body.size() : eol + 1;
                    auto eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    std::string k = line.substr(0, eq);
                    std::string v = line.substr(eq + 1);
                    if (k == "reason")          ev.message = v;
                    else if (k == "name")       ev.name    = v;
                    else if (k == "ts_ms") {
                        try { ev.ts_unix_ms = std::stoll(v); } catch (...) {}
                    }
                    else if (!k.empty())        ev.props[k] = v;
                }

                // Re-enqueue as a regular pending event (consent allowing).
                if (!enqueue(ev)) {
                    // Consent might have flipped to denied — leave the
                    // tombstone in place so a future opt-in can still
                    // retrieve it.  But cap at ~50 to avoid pile-up.
                    int count = 0;
                    std::error_code ec2;
                    for (auto it2 = fs::directory_iterator(tombstones_dir, ec2);
                         !ec2 && it2 != fs::directory_iterator();
                         it2.increment(ec2)) ++count;
                    if (count > 50) fs::remove(p, ec);
                    continue;
                }
                fs::remove(p, ec);
            }
        }

        // -----------------------------------------------------------------
        // Worker thread loop
        // -----------------------------------------------------------------
        void worker_main() {
            // First action: drain any tombstones from a previous run.  These
            // become crash events and join the regular upload queue.
            drain_tombstones();
            recount_pending();

            for (;;) {
                if (worker_should_exit.load()) return;

                // Wait for a flush signal or for the configured interval.
                {
                    std::unique_lock<std::mutex> lk(worker_mutex);
                    auto interval = std::chrono::seconds(
                        std::max(5, snapshot_cfg().flush_interval_seconds));
                    worker_cv.wait_for(lk, interval, [&] {
                        return worker_should_exit.load() || worker_flush_now.exchange(false);
                    });
                }
                if (worker_should_exit.load()) return;

                try_upload_batch();
            }
        }

        // Read up to N pending files, build an ndjson body, POST it, on
        // success delete the files.
        void try_upload_batch() {
            config snap = snapshot_cfg();

            // Refresh tombstones in case a sibling external just crashed.
            drain_tombstones();

            // Gather candidate files (lex-sorted == time-sorted given our
            // filename prefix).
            std::vector<fs::path> files;
            {
                std::error_code ec;
                for (auto it = fs::directory_iterator(pending_dir, ec);
                     !ec && it != fs::directory_iterator();
                     it.increment(ec)) {
                    if (it->is_regular_file() && it->path().extension() == ".json") {
                        files.push_back(it->path());
                    }
                }
            }
            std::sort(files.begin(), files.end());
            recount_pending();
            if (files.empty()) return;

            // Don't upload without consent.  We still process tombstones
            // above so that a later opt-in can pick them up, but we won't
            // POST anything without explicit consent.
            {
                std::lock_guard<std::mutex> g(consent_mutex_);
                if (consent != consent_status::granted) return;
            }

            // Need an endpoint unless we're in dry-run mode.
            if (!snap.dry_run && snap.endpoint.empty()) {
                set_last_error("no endpoint configured");
                uploader_online.store(false);
                return;
            }

            const size_t batch_n = static_cast<size_t>(std::max(1, snap.batch_size));
            for (size_t off = 0; off < files.size(); off += batch_n) {
                size_t end = std::min(files.size(), off + batch_n);

                std::ostringstream body;
                std::vector<fs::path> in_flight;
                in_flight.reserve(end - off);
                bool first = true;

                // Build the ndjson body.  Each line is
                // {<envelope fields>, ...event fields}.
                for (size_t i = off; i < end; ++i) {
                    std::ifstream in(files[i]);
                    if (!in) continue;
                    std::stringstream ss;
                    ss << in.rdbuf();
                    std::string ev_json = ss.str();
                    // Strip surrounding {} and inject envelope fields.
                    if (ev_json.size() < 2 || ev_json.front() != '{' || ev_json.back() != '}') {
                        // Malformed — drop it so it doesn't poison the queue.
                        std::error_code ec; fs::remove(files[i], ec);
                        continue;
                    }
                    std::string inner = ev_json.substr(1, ev_json.size() - 2);
                    if (!first) body << "\n";
                    first = false;
                    body << "{"
                         << "\"vendor\":\""           << json::escape(snap.vendor)              << "\","
                         << "\"device_name\":\""      << json::escape(snap.device_name)         << "\","
                         << "\"device_version\":\""   << json::escape(snap.device_version)      << "\","
                         << "\"device_id\":\""        << json::escape(device_id)                << "\","
                         << "\"session_id\":\""       << json::escape(session_id)               << "\","
                         << "\"user_id\":\""          << json::escape(snap.user_id)             << "\","
                         << "\"platform\":\""         << json::escape(platform_os_string())     << "\","
                         << "\"max_version\":\""      << json::escape(platform_max_version_hint()) << "\","
                         << inner << "}";
                    in_flight.push_back(files[i]);
                }
                if (in_flight.empty()) continue;

                if (snap.dry_run) {
                    log("dry_run: would POST " + std::to_string(in_flight.size())
                        + " events (" + std::to_string(body.str().size()) + " bytes)",
                        log_level::info);
                    for (auto& p : in_flight) {
                        std::error_code ec;
                        fs::remove(p, ec);
                    }
                    sent_count.fetch_add(static_cast<int>(in_flight.size()),
                                         std::memory_order_relaxed);
                    pending_count.fetch_sub(static_cast<int>(in_flight.size()),
                                            std::memory_order_relaxed);
                    uploader_online.store(true);
                    set_last_error("");
                    continue;
                }

                std::string err;
                bool ok = http_post_ndjson(snap.endpoint, snap.endpoint_token,
                                           body.str(), err);
                if (ok) {
                    for (auto& p : in_flight) {
                        std::error_code ec;
                        fs::remove(p, ec);
                    }
                    sent_count.fetch_add(static_cast<int>(in_flight.size()),
                                         std::memory_order_relaxed);
                    pending_count.fetch_sub(static_cast<int>(in_flight.size()),
                                            std::memory_order_relaxed);
                    uploader_online.store(true);
                    set_last_error("");
                } else {
                    set_last_error(err);
                    uploader_online.store(false);
                    log("upload failed: " + err + " (" +
                        std::to_string(in_flight.size()) +
                        " events kept for retry)", log_level::warning);
                    // Stop trying further batches this cycle — save bandwidth.
                    return;
                }
            }
        }
    };

    // -------------------------------------------------------------------------
    // client static accessor + thin forwarders
    // -------------------------------------------------------------------------
    client& client::get() {
        static client instance;
        return instance;
    }

    client::client() : d(new impl()) {
        d->session_id = platform_uuid_v4();
    }

    client::~client() {
        stop();
        delete d;
    }

    bool client::start(const config& cfg) {
        bool first_init = !d->initialized.exchange(true);

        {
            std::lock_guard<std::mutex> g(d->cfg_mutex_);
            // Merge: later device registrations should not overwrite the
            // previously-configured endpoint with an empty one.
            if (!cfg.endpoint.empty())             d->cfg.endpoint            = cfg.endpoint;
            if (!cfg.endpoint_token.empty())       d->cfg.endpoint_token      = cfg.endpoint_token;
            if (!cfg.vendor.empty())               d->cfg.vendor              = cfg.vendor;
            if (!cfg.device_name.empty())          d->cfg.device_name         = cfg.device_name;
            if (!cfg.device_version.empty())       d->cfg.device_version      = cfg.device_version;
            if (!cfg.user_id.empty())              d->cfg.user_id             = cfg.user_id;
            if (cfg.flush_interval_seconds > 0)    d->cfg.flush_interval_seconds = cfg.flush_interval_seconds;
            if (cfg.max_queue_files > 0)           d->cfg.max_queue_files     = cfg.max_queue_files;
            if (cfg.batch_size > 0)                d->cfg.batch_size          = cfg.batch_size;
            d->cfg.dry_run            = cfg.dry_run;
            d->cfg.crash_capture      = cfg.crash_capture;
            d->cfg.log_to_max_console = cfg.log_to_max_console;
        }

        if (first_init) {
            d->init_storage(cfg.vendor.empty() ? std::string("bugbytz") : cfg.vendor);
            d->load_or_create_config();
            d->recount_pending();

            // Crash hooks run on the main thread.  Skip if disabled.
            if (cfg.crash_capture) {
                install_crash_handlers(d->tombstones_dir.string(),
                                       d->session_id,
                                       d->device_id,
                                       cfg.device_name,
                                       cfg.device_version);
            }

            d->worker_should_exit.store(false);
            d->worker = std::thread(&impl::worker_main, d);
        }

        // Always wake worker to drain anything that's waiting.
        d->worker_flush_now.store(true);
        d->worker_cv.notify_all();
        return true;
    }

    void client::stop() {
        if (!d->initialized.load()) return;
        d->worker_should_exit.store(true);
        d->worker_cv.notify_all();
        if (d->worker.joinable()) d->worker.join();
        uninstall_crash_handlers();
        d->initialized.store(false);
    }

    void client::set_active_device(const std::string& name, const std::string& version) {
        std::lock_guard<std::mutex> g(d->cfg_mutex_);
        if (!name.empty())    d->cfg.device_name    = name;
        if (!version.empty()) d->cfg.device_version = version;
    }

    void client::set_user_id(const std::string& user_id) {
        std::lock_guard<std::mutex> g(d->cfg_mutex_);
        d->cfg.user_id = user_id;
    }

    void client::set_consent(consent_status status) {
        {
            std::lock_guard<std::mutex> g(d->consent_mutex_);
            if (d->consent == status) return;
            d->consent = status;
        }
        d->save_config();
        // If we just turned consent off, drop everything queued so far —
        // the user said no, we honour it immediately.
        if (status == consent_status::denied) {
            clear_queue();
        }
        // Wake the uploader so the new state takes effect right away.
        d->worker_flush_now.store(true);
        d->worker_cv.notify_all();
    }

    consent_status client::get_consent() const {
        std::lock_guard<std::mutex> g(d->consent_mutex_);
        return d->consent;
    }

    static void stamp(event& e) {
        if (e.ts_unix_ms == 0) e.ts_unix_ms = now_unix_ms();
    }

    bool client::log_event(const std::string& name,
                           const std::unordered_map<std::string,std::string>& props) {
        event e;
        e.kind  = event_kind::event;
        e.level = log_level::info;
        e.name  = name;
        e.props = props;
        stamp(e);
        return d->enqueue(e);
    }

    bool client::log_error(const std::string& name, const std::string& message,
                           const std::unordered_map<std::string,std::string>& props) {
        event e;
        e.kind    = event_kind::error;
        e.level   = log_level::error;
        e.name    = name;
        e.message = message;
        e.props   = props;
        stamp(e);
        return d->enqueue(e);
    }

    bool client::log_metric(const std::string& name, double value,
                            const std::string& unit,
                            const std::unordered_map<std::string,std::string>& props) {
        event e;
        e.kind          = event_kind::metric;
        e.level         = log_level::info;
        e.name          = name;
        e.numeric_value = value;
        e.unit          = unit;
        e.props         = props;
        stamp(e);
        return d->enqueue(e);
    }

    bool client::log_crash(const std::string& name, const std::string& message,
                           const std::unordered_map<std::string,std::string>& props) {
        event e;
        e.kind    = event_kind::crash;
        e.level   = log_level::fatal;
        e.name    = name;
        e.message = message;
        e.props   = props;
        stamp(e);
        return d->enqueue(e);
    }

    void client::flush() {
        d->worker_flush_now.store(true);
        d->worker_cv.notify_all();
    }

    void client::clear_queue() {
        std::error_code ec;
        for (auto it = fs::directory_iterator(d->pending_dir, ec);
             !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            std::error_code ec2;
            fs::remove(it->path(), ec2);
        }
        d->pending_count.store(0, std::memory_order_relaxed);
    }

    status_snapshot client::status() const {
        status_snapshot s;
        s.consent         = get_consent();
        s.initialized     = d->initialized.load();
        s.uploader_online = d->uploader_online.load();
        s.pending_count   = d->pending_count.load();
        s.sent_count      = d->sent_count.load();
        s.dropped_count   = d->dropped_count.load();
        s.last_error      = d->get_last_error();
        s.device_id       = d->device_id;
        s.session_id      = d->session_id;
        return s;
    }

    void client::set_console_logger(console_logger fn, void* user) {
        std::lock_guard<std::mutex> g(d->logger_mutex);
        d->logger_fn   = fn;
        d->logger_user = user;
    }

    std::string client::storage_dir() const {
        return d->root_dir.string();
    }

    // -------------------------------------------------------------------------
    // Default (non-Apple) stubs.  The real platform implementations live in
    // telemetry_http.mm and telemetry_crash.mm; on macOS they replace these
    // weak symbols at link time.  On other platforms the framework still
    // builds: it persists events to disk and counts them, just doesn't POST
    // anything or capture crashes.
    // -------------------------------------------------------------------------
#if !defined(__APPLE__)
    bool http_post_ndjson(const std::string&, const std::string&,
                          const std::string&, std::string& error_out) {
        error_out = "no http transport on this platform";
        return false;
    }
    void install_crash_handlers(const std::string&, const std::string&,
                                const std::string&, const std::string&,
                                const std::string&) {}
    void uninstall_crash_handlers() {}

    std::string platform_app_support_dir() {
        const char* h = std::getenv("HOME");
        return h ? std::string(h) + "/.local/share" : std::string(".");
    }
    std::string platform_os_string()         { return "unknown"; }
    std::string platform_max_version_hint()  { return ""; }
    std::string platform_uuid_v4() {
        // Cheap, NOT cryptographically secure — fine as a unique tag.
        static std::atomic<uint64_t> ctr { 0 };
        uint64_t a = static_cast<uint64_t>(now_unix_ms());
        uint64_t b = ctr.fetch_add(1) ^ static_cast<uint64_t>(std::rand());
        char buf[40];
        std::snprintf(buf, sizeof(buf),
                      "%08llx-%04llx-4%03llx-%04llx-%012llx",
                      static_cast<unsigned long long>((a >> 32) & 0xffffffff),
                      static_cast<unsigned long long>((a >> 16) & 0xffff),
                      static_cast<unsigned long long>(a & 0xfff),
                      static_cast<unsigned long long>((b >> 48) & 0x3fff) | 0x8000ull,
                      static_cast<unsigned long long>(b & 0xffffffffffffull));
        return buf;
    }
#endif

}}  // namespace bz::telemetry
