// telemetry_http.mm
// -----------------------------------------------------------------------------
// macOS HTTP transport + platform helpers for bz.telemetry.
//
// We use NSURLSession because:
//   - It does the right thing on flaky networks (TCP keepalive, retries on
//     resolver flakiness, cellular vs Wi-Fi distinctions, etc.).
//   - It honours system-wide HTTP proxies + cert overrides, which matters
//     for users on corporate networks.
//   - It does NOT depend on libcurl, which we'd otherwise have to vendor.
//
// The function is invoked from the worker thread.  We use a synchronous
// dispatch_semaphore_wait because the worker thread is already in a wait /
// flush loop; we want it to block on the request and only return once we
// have a definitive success / failure.
//
// Network mistakes we explicitly avoid
//   - We set a finite timeout (default 30 s) so a stuck connection can't
//     hold the worker forever.
//   - We treat ALL non-2xx as failures and bubble the body up so the user
//     can see "missing API key" etc. in the [bz.telemetry] outlet.
//   - We never log the bearer token to the Max console.
// -----------------------------------------------------------------------------

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#include <sys/utsname.h>
#include <string>

#include "telemetry_core.hpp"

namespace bz { namespace telemetry {

    bool http_post_ndjson(const std::string& endpoint,
                          const std::string& bearer_token,
                          const std::string& ndjson_body,
                          std::string&       error_out) {
        if (endpoint.empty()) {
            error_out = "endpoint not configured";
            return false;
        }

        @autoreleasepool {
            NSString* urlStr = [NSString stringWithUTF8String:endpoint.c_str()];
            NSURL* url = [NSURL URLWithString:urlStr];
            if (url == nil) {
                error_out = "invalid endpoint URL";
                return false;
            }

            NSMutableURLRequest* req =
                [NSMutableURLRequest requestWithURL:url
                                        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                    timeoutInterval:30.0];
            [req setHTTPMethod:@"POST"];
            // application/x-ndjson is the de-facto MIME for newline-delimited
            // JSON.  Some receivers prefer text/plain — set both via Accept.
            [req setValue:@"application/x-ndjson" forHTTPHeaderField:@"Content-Type"];
            [req setValue:@"application/json"     forHTTPHeaderField:@"Accept"];
            [req setValue:@"bz.telemetry/1.0 (Max-for-Live; macOS)"
                forHTTPHeaderField:@"User-Agent"];
            if (!bearer_token.empty()) {
                NSString* h = [NSString stringWithFormat:@"Bearer %s", bearer_token.c_str()];
                [req setValue:h forHTTPHeaderField:@"Authorization"];
            }

            NSData* body = [NSData dataWithBytes:ndjson_body.data()
                                          length:ndjson_body.size()];
            [req setHTTPBody:body];

            __block bool        ok      = false;
            __block std::string err_msg;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);

            // Use a one-off ephemeral session.  Cookies / caches are
            // irrelevant for telemetry POSTs and an ephemeral session avoids
            // littering ~/Library/Caches with anything user-visible.
            NSURLSessionConfiguration* cfg =
                [NSURLSessionConfiguration ephemeralSessionConfiguration];
            cfg.timeoutIntervalForRequest  = 30.0;
            cfg.timeoutIntervalForResource = 60.0;
            // Use a serial delegate queue so completion isn't multiplexed.
            NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg];

            NSURLSessionDataTask* task = [session
                dataTaskWithRequest:req
                  completionHandler:^(NSData* data, NSURLResponse* resp, NSError* error) {
                      if (error != nil) {
                          err_msg = std::string("network error: ")
                                  + [[error localizedDescription] UTF8String];
                          dispatch_semaphore_signal(done);
                          return;
                      }
                      NSHTTPURLResponse* http = (NSHTTPURLResponse*)resp;
                      NSInteger status = [http statusCode];
                      if (status >= 200 && status < 300) {
                          ok = true;
                          dispatch_semaphore_signal(done);
                          return;
                      }
                      // Pull a short snippet of the body to help diagnose
                      // 401 / 403 / 422 / 500 in the user's outlet.
                      NSString* preview = nil;
                      if (data != nil && [data length] > 0) {
                          NSUInteger n = MIN([data length], (NSUInteger)256);
                          preview = [[NSString alloc] initWithData:[data subdataWithRange:NSMakeRange(0, n)]
                                                          encoding:NSUTF8StringEncoding];
                      }
                      char buf[64];
                      std::snprintf(buf, sizeof(buf), "HTTP %ld", (long)status);
                      err_msg = buf;
                      if (preview != nil) {
                          err_msg += ": ";
                          err_msg += [preview UTF8String];
                      }
                      dispatch_semaphore_signal(done);
                  }];
            [task resume];

            // Wait with a generous safety timeout (slightly longer than the
            // request timeout) so we can never block the worker forever.
            dispatch_time_t hard = dispatch_time(DISPATCH_TIME_NOW, (int64_t)(70.0 * NSEC_PER_SEC));
            if (dispatch_semaphore_wait(done, hard) != 0) {
                [task cancel];
                err_msg = "request hard-timeout (70s)";
                ok = false;
            }
            [session finishTasksAndInvalidate];

            if (!ok) error_out = err_msg;
            return ok;
        }
    }

    // -------------------------------------------------------------------------
    // Platform helpers
    // -------------------------------------------------------------------------

    std::string platform_app_support_dir() {
        @autoreleasepool {
            NSArray* paths = NSSearchPathForDirectoriesInDomains(
                NSApplicationSupportDirectory, NSUserDomainMask, YES);
            if (paths.count == 0) return std::string();
            return std::string([paths.firstObject UTF8String]);
        }
    }

    std::string platform_os_string() {
        @autoreleasepool {
            NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
            char buf[64];
            std::snprintf(buf, sizeof(buf), "macOS %ld.%ld.%ld",
                          (long)v.majorVersion, (long)v.minorVersion, (long)v.patchVersion);
            return buf;
        }
    }

    std::string platform_max_version_hint() {
        @autoreleasepool {
            // Best effort: read the host process's CFBundleShortVersionString
            // (works for Live, Max, etc.).  Falls back to executable name.
            NSBundle* main = [NSBundle mainBundle];
            NSString* name = nil;
            NSString* version = nil;
            if (main != nil) {
                NSDictionary* info = [main infoDictionary];
                name    = info[@"CFBundleName"]
                       ?: info[@"CFBundleExecutable"];
                version = info[@"CFBundleShortVersionString"]
                       ?: info[@"CFBundleVersion"];
            }
            std::string out;
            if (name)    out += [name UTF8String];
            if (version) {
                if (!out.empty()) out += " ";
                out += [version UTF8String];
            }
            return out;
        }
    }

    std::string platform_uuid_v4() {
        @autoreleasepool {
            NSUUID* u = [NSUUID UUID];
            NSString* s = [[u UUIDString] lowercaseString];
            return std::string([s UTF8String]);
        }
    }

}}  // namespace bz::telemetry
