// Forces the compiler to emit non-inline DLL exports for EventBus and Subscription.
// All methods are defined in the header; including it here (with HE_CORE_BUILD_DLL
// active) generates __declspec(dllexport) stubs that consumers can import.
#include "Events/EventBus.h"
