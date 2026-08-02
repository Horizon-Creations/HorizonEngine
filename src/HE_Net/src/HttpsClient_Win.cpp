#include "Net/HttpsClient.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// ─── Windows TLS backend — WinHTTP ───────────────────────────────────────────
// Schannel does the TLS: system trust store, revocation policy, and the user's
// proxy configuration (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY) all come from the OS.

namespace HE::Net {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                           static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), need);
    return out;
}

// RAII for the three WinHTTP handle types, so every early return closes them in
// the right order.
struct WinHttpHandle {
    HINTERNET h = nullptr;
    explicit WinHttpHandle(HINTERNET handle = nullptr) : h(handle) {}
    ~WinHttpHandle() { if (h) ::WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&)            = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

} // namespace

bool httpsAvailable() { return true; }

const char* httpsBackendName() { return "WinHTTP"; }

HttpsResponse httpsRequest(const std::string& url, const std::string& method,
                           const std::vector<std::string>& extraHeaders,
                           const std::string& body, int timeoutMs) {
    HttpsResponse out;

    const std::wstring wideUrl = widen(url);

    // Split the URL with WinHTTP's own parser rather than hand-rolling it.
    URL_COMPONENTS uc{};
    uc.dwStructSize      = sizeof(uc);
    uc.dwSchemeLength    = static_cast<DWORD>(-1);
    uc.dwHostNameLength  = static_cast<DWORD>(-1);
    uc.dwUrlPathLength   = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &uc)) {
        out.error = "invalid url";
        return out;
    }

    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    WinHttpHandle session(::WinHttpOpen(L"HorizonEngine/1.0",
                                        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { out.error = "WinHttpOpen failed"; return out; }

    ::WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    WinHttpHandle connect(::WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!connect) { out.error = "WinHttpConnect failed"; return out; }

    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    WinHttpHandle request(::WinHttpOpenRequest(
        connect.h, widen(method).c_str(), path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) { out.error = "WinHttpOpenRequest failed"; return out; }

    std::wstring headerBlock;
    for (const auto& h : extraHeaders) headerBlock += widen(h) + L"\r\n";

    if (!::WinHttpSendRequest(
            request.h,
            headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
            headerBlock.empty() ? 0 : static_cast<DWORD>(headerBlock.size()),
            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()), 0)) {
        // A failed certificate check lands here (ERROR_WINHTTP_SECURE_FAILURE)
        // and must stay a failure — never fall through to an empty success.
        const DWORD err = ::GetLastError();
        out.error = (err == ERROR_WINHTTP_SECURE_FAILURE)
                        ? "TLS certificate validation failed"
                        : "WinHttpSendRequest failed";
        return out;
    }

    if (!::WinHttpReceiveResponse(request.h, nullptr)) {
        out.error = "WinHttpReceiveResponse failed";
        return out;
    }

    DWORD status = 0, statusSize = sizeof(status);
    ::WinHttpQueryHeaders(request.h,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                          WINHTTP_NO_HEADER_INDEX);
    out.statusCode = static_cast<int>(status);

    for (;;) {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.h, &available)) break;
        if (available == 0) break;

        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!::WinHttpReadData(request.h, chunk.data(), available, &read)) break;
        if (read == 0) break;
        chunk.resize(read);
        out.body += chunk;
    }

    out.ok = true;
    return out;
}

} // namespace HE::Net
