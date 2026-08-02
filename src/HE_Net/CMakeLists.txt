# ─── HorizonNet ──────────────────────────────────────────────────────────────
# The engine's networking layer. Layered so gameplay replication (Layer 3a) and
# editor collaboration / source-control presence+locks (Layer 3b) share one
# transport + serialization core:
#   Layer 0  Platform sockets      (shared with the SC HTTP stack; later checkpoint)
#   Layer 1  ITransport / Session   (LoopbackTransport here; GNS + WebSocket later)
#   Layer 2  BitStream / messages   (bit-packed, quantized serialization)
#
# Deliberately dependency-light for now: the loopback transport needs nothing but
# the STL, so this builds and tests everywhere without pulling GameNetworkingSockets.
add_library(HorizonNet SHARED
	src/Socket.cpp
	src/LoopbackTransport.cpp
	src/TcpTransport.cpp
	src/SecureTransport.cpp
	src/HttpClient.cpp
	src/PortMapper.cpp
	src/SessionDirectory.cpp
	src/NetSession.cpp
)

# ── TLS comes from the operating system, never from us ────────────────────────
# The session directory is an https:// endpoint, so it needs certificate
# validation: hostname matching, chain building, expiry, revocation, the system
# trust store and corporate proxies. That is precisely where hand-written TLS
# fails silently, so each platform delegates to its own stack:
#   macOS   NSURLSession (Foundation)
#   Windows WinHTTP      (Schannel underneath)
#   Linux   libcurl      (distro TLS + CA bundle)
# HorizonCore only links the crypto *primitives* (OpenSSL::Crypto / mbedcrypto);
# none of that is a TLS implementation.
if(APPLE)
	target_sources(HorizonNet PRIVATE src/HttpsClient_Apple.mm)
	target_link_libraries(HorizonNet PRIVATE "-framework Foundation")
elseif(WIN32)
	target_sources(HorizonNet PRIVATE src/HttpsClient_Win.cpp)
	target_link_libraries(HorizonNet PRIVATE winhttp)
else()
	target_sources(HorizonNet PRIVATE src/HttpsClient_Curl.cpp)
	find_package(CURL QUIET)
	if(CURL_FOUND)
		message(STATUS "HorizonNet: libcurl found — session directory (HTTPS) enabled")
		target_link_libraries(HorizonNet PRIVATE CURL::libcurl)
		target_compile_definitions(HorizonNet PRIVATE HE_HAVE_CURL)
	else()
		# Build stays green, but httpsAvailable() reports false and every request
		# fails with an explicit message instead of quietly never connecting.
		message(WARNING "HorizonNet: libcurl NOT found — the collaboration session "
		                "directory will be unavailable. Install libcurl development "
		                "headers and reconfigure.")
	endif()
endif()

# nlohmann/json for the directory's JSON payloads (header-only).
target_link_libraries(HorizonNet PRIVATE nlohmann_json::nlohmann_json)

# HorizonCore supplies the crypto primitives the session handshake reuses rather
# than reimplementing: Hpak::aesGcm* (AEAD, backed by OpenSSL/mbedTLS),
# Hpak::randomBytes (CSPRNG) and KeyDerivation::hmac (HMAC-SHA256).
target_link_libraries(HorizonNet PRIVATE HorizonCore)

# Winsock (Windows only; POSIX sockets need no extra link library).
if(WIN32)
	target_link_libraries(HorizonNet PRIVATE ws2_32)
endif()

target_include_directories(HorizonNet
	PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(HorizonNet PUBLIC cxx_std_20)

# Explicit export macro (HE_NET_API), same discipline as HorizonCore — keeps the
# ABI surface deliberate and satisfies the Windows-CI portability rules.
target_compile_definitions(HorizonNet
	PRIVATE HE_NET_BUILD_DLL
	PUBLIC  HE_NET_DLL
)

he_deploy_dll(HorizonNet)
