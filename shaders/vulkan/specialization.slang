// Boolean specialization constants (VkSpecializationInfo, MaterialPipelineSetConfig and the toon
// passes' outline variant). DXIL has none, so the DXIL build (MINIENGINE_DXIL) makes each a static
// const, false unless the variant is compiled with -DSPECIALIZATION_<id>=true. The Vulkan expansion
// is the plain [[vk::constant_id]] declaration, so its SPIR-V does not change.
//
//     SPECIALIZATION_BOOL(0, kOutline);

#ifndef SPECIALIZATION_SLANG
#define SPECIALIZATION_SLANG

#ifdef MINIENGINE_DXIL
#ifndef SPECIALIZATION_0
#define SPECIALIZATION_0 false
#endif
#ifndef SPECIALIZATION_1
#define SPECIALIZATION_1 false
#endif
#ifndef SPECIALIZATION_2
#define SPECIALIZATION_2 false
#endif
#ifndef SPECIALIZATION_3
#define SPECIALIZATION_3 false
#endif
#define SPECIALIZATION_BOOL(id, name) static const bool name = SPECIALIZATION_##id
#else
#define SPECIALIZATION_BOOL(id, name) [[vk::constant_id(id)]] const bool name = false
#endif

#endif
