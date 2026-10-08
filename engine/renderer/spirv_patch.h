#pragma once

#include <cstdint>
#include <vector>

namespace me
{

// Sets the default of the 32-bit float specialization constant with SpecId specId in a SPIR-V
// module, for a pipeline built by code that passes no VkSpecializationInfo (ImGui's Vulkan backend).
// False when the module has no such constant (it is then unchanged).
bool PatchSpecConstantFloat(std::vector<uint32_t>& spirv, uint32_t specId, float value);
}
