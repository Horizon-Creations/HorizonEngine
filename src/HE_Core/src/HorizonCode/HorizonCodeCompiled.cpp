#include "HorizonCode/HorizonCodeCompiled.h"

namespace HorizonCode {

// Out-of-line key function: anchors the vtable in HorizonCore so every
// generated dylib shares one RTTI/vtable instance across the ABI boundary.
CompiledInstance::~CompiledInstance() = default;

} // namespace HorizonCode
