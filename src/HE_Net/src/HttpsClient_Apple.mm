#include "Net/HttpsClient.h"

#include "NetLog.h"

#import <Foundation/Foundation.h>

#include <string>
#include <vector>

// ─── Apple TLS backend — NSURLSession ────────────────────────────────────────
// Certificate validation, the system trust store, ATS policy, proxy settings and
// redirects all come from the OS. Nothing here touches TLS itself.

namespace HE::Net {

bool httpsAvailable() { return true; }

const char* httpsBackendName() { return "NSURLSession"; }

HttpsResponse httpsRequest(const std::string& url, const std::string& method,
                           const std::vector<std::string>& extraHeaders,
                           const std::string& body, int timeoutMs) {
    HttpsResponse out;

    @autoreleasepool {
        NSString* urlStr = [NSString stringWithUTF8String:url.c_str()];
        NSURL*    nsUrl  = [NSURL URLWithString:urlStr];
        if (!nsUrl || !nsUrl.host) {
            HE_LOG_ERROR(Net, "HTTPS: invalid url \"%s\"", url.c_str());
            out.error = "invalid url";
            return out;
        }
        // Host and method only — the path can carry a session id and the body
        // can carry the management token.
        HE_LOG_DEBUG(Net, "HTTPS: %s %s", method.c_str(), nsUrl.host.UTF8String);

        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsUrl];
        req.HTTPMethod = [NSString stringWithUTF8String:method.c_str()];
        req.timeoutInterval = static_cast<NSTimeInterval>(timeoutMs) / 1000.0;

        for (const auto& h : extraHeaders) {
            const std::size_t colon = h.find(':');
            if (colon == std::string::npos) continue;
            std::string name  = h.substr(0, colon);
            std::string value = h.substr(colon + 1);
            // Trim leading spaces from the value.
            const std::size_t b = value.find_first_not_of(" \t");
            value = (b == std::string::npos) ? std::string{} : value.substr(b);
            [req setValue:[NSString stringWithUTF8String:value.c_str()]
                 forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];
        }

        if (!body.empty()) {
            req.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
        }

        // NSURLSession is asynchronous; this API is synchronous by contract (and
        // documented as worker-thread only), so block on a semaphore. The
        // completion handler runs on the session's own delegate queue, never the
        // calling thread, so this cannot deadlock against itself.
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block std::string  bodyOut;
        __block int          status = 0;
        __block std::string  errOut;

        NSURLSessionConfiguration* cfg =
            [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = static_cast<NSTimeInterval>(timeoutMs) / 1000.0;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg];

        NSURLSessionDataTask* task = [session
            dataTaskWithRequest:req
              completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                  if (error) {
                      errOut = error.localizedDescription.UTF8String
                             ? error.localizedDescription.UTF8String
                             : "request failed";
                  } else {
                      if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                          status = static_cast<int>(
                              ((NSHTTPURLResponse*)response).statusCode);
                      }
                      if (data.length > 0) {
                          bodyOut.assign(static_cast<const char*>(data.bytes), data.length);
                      }
                  }
                  dispatch_semaphore_signal(done);
              }];
        [task resume];

        // Hard ceiling slightly above the request timeout, so a wedged session
        // cannot hang the worker thread forever.
        const dispatch_time_t deadline =
            dispatch_time(DISPATCH_TIME_NOW,
                          static_cast<int64_t>(timeoutMs + 2000) * NSEC_PER_MSEC);
        if (dispatch_semaphore_wait(done, deadline) != 0) {
            HE_LOG_ERROR(Net, "HTTPS: request to %s wedged past %d ms — cancelled",
                         nsUrl.host.UTF8String, timeoutMs + 2000);
            [task cancel];
            [session invalidateAndCancel];
            out.error = "timeout";
            return out;
        }
        [session finishTasksAndInvalidate];

        if (!errOut.empty()) {
            // TLS failures (bad certificate, hostname mismatch, untrusted chain)
            // arrive here as NSError — they must surface as a failure, never as
            // an empty-but-successful response.
            HE_LOG_ERROR(Net, "HTTPS: request to %s failed — %s",
                         nsUrl.host.UTF8String, errOut.c_str());
            out.error = errOut;
            return out;
        }

        HE_LOG_DEBUG(Net, "HTTPS: %s answered HTTP %d (%s)", nsUrl.host.UTF8String,
                     status, HE::Net::detail::logBytes(bodyOut.size()).c_str());
        out.ok         = true;
        out.statusCode = status;
        out.body       = std::move(bodyOut);
    }

    return out;
}

} // namespace HE::Net
