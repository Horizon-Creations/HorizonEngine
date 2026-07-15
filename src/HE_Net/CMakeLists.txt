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
	src/LoopbackTransport.cpp
	src/NetSession.cpp
)

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
