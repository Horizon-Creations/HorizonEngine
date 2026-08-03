#include "Net/HttpsClient.h"

#include "NetLog.h"

#include <string>
#include <vector>

// ─── Linux TLS backend — libcurl ─────────────────────────────────────────────
// libcurl brings the distro's TLS library and CA bundle, so certificate
// validation, proxy environment variables and redirects follow system policy.
//
// When libcurl is absent at configure time this file still compiles, but every
// request fails with a clear message — the session directory is then simply
// unavailable, which the UI must say out loud rather than silently never
// connecting.

#ifdef HE_HAVE_CURL
  #include <curl/curl.h>
#endif

namespace HE::Net {

#ifdef HE_HAVE_CURL

namespace {

std::size_t writeCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

bool httpsAvailable() { return true; }

const char* httpsBackendName() { return "libcurl"; }

HttpsResponse httpsRequest(const std::string& url, const std::string& method,
                           const std::vector<std::string>& extraHeaders,
                           const std::string& body, int timeoutMs) {
    HttpsResponse out;

    CURL* curl = ::curl_easy_init();
    if (!curl) {
        HE_LOG_ERROR(Net, "HTTPS: curl_easy_init failed");
        out.error = "curl_easy_init failed";
        return out;
    }

    struct curl_slist* headers = nullptr;
    for (const auto& h : extraHeaders) headers = ::curl_slist_append(headers, h.c_str());

    ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    ::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);   // safe on threads
    // Explicit, even though these are the defaults: certificate and hostname
    // verification must never be switched off here.
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
    if (headers) ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (!body.empty()) {
        ::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        ::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    const CURLcode rc = ::curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long status = 0;
        ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out.statusCode = static_cast<int>(status);
        out.ok = true;
    } else {
        // libcurl reports certificate problems here too — the whole reason TLS
        // is delegated to the system rather than hand-rolled.
        HE_LOG_ERROR(Net, "HTTPS: libcurl request failed — %s", ::curl_easy_strerror(rc));
        out.error = ::curl_easy_strerror(rc);
        out.body.clear();
    }

    if (headers) ::curl_slist_free_all(headers);
    ::curl_easy_cleanup(curl);
    return out;
}

#else   // no libcurl at configure time

bool httpsAvailable() { return false; }

const char* httpsBackendName() { return "none"; }

HttpsResponse httpsRequest(const std::string&, const std::string&,
                           const std::vector<std::string>&, const std::string&, int) {
    HttpsResponse out;
    HE_LOG_ERROR(Net, "HTTPS: this build has no TLS backend (libcurl was missing when it "
                      "was configured) — the session directory is unavailable");
    out.error = "no TLS backend: libcurl was not found at build time "
                "(install libcurl development headers and reconfigure)";
    return out;
}

#endif

} // namespace HE::Net
