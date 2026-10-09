// Direct3D registers for the shaders' resources. Slang places a resource on D3D by its register
// semantic and ignores [[vk::binding]], so every declaration carries both, and they agree: binding b of
// set s is register <class>b in space s, which is what NVRHI's binding layouts give on D3D12 with zero
// binding offsets and registerSpaceIsDescriptorSet (registerSpace = the set). The class is the
// resource's: t (textures, read-only buffers, acceleration structures), u (read-write), s (samplers),
// b (constant buffers). Push constants are b0 in the space of the binding layout that carries them
// (NVRHI's PushConstants(0) becomes root constants there). tools/render_ab/build_all.py --dxil checks
// all of it against the SPIR-V bindings.
//
// D3D_REGISTER spells the register when the set (or the class) is a macro, which a literal register
// cannot name: D3D_REGISTER(t, 3, RAY_SCENE_SET). Its arguments must expand to plain tokens.

#ifndef D3D_REGISTER_SLANG
#define D3D_REGISTER_SLANG

#define D3D_REGISTER_PASTE2(a, b) a##b
#define D3D_REGISTER_PASTE(a, b) D3D_REGISTER_PASTE2(a, b)
#define D3D_REGISTER(cls, binding, set) register(D3D_REGISTER_PASTE(cls, binding), D3D_REGISTER_PASTE(space, set))

// A push constant block's register, b0 in space set: `ConstantBuffer<T> name D3D_PUSH_CONSTANTS(1);`.
// Only in the DXIL build: for SPIR-V Slang reads a register on a push constant block as an implicit
// binding and warns that it overlaps the set's binding 0.
#ifdef MINIENGINE_DXIL
#define D3D_PUSH_CONSTANTS(set) : register(b0, D3D_REGISTER_PASTE(space, set))
#else
#define D3D_PUSH_CONSTANTS(set)
#endif

#endif
