// The vertex and instance index as Vulkan counts them: VertexIndex and InstanceIndex include the
// draw's first vertex (or vertex offset) and first instance, which the engine indexes per-draw data
// with (VulkanDrawItem::motionSlot). Direct3D's SV_VertexID and SV_InstanceID do not, so the DXIL build
// (MINIENGINE_DXIL, tools/render_ab/build_all.py --dxil) adds SV_StartVertexLocation and
// SV_StartInstanceLocation (shader model 6.8). Macros rather than a struct with an Index() method, so
// the Vulkan build compiles to the same SPIR-V as with the plain system values.
//
// DRAW_VERTEX_INDEX_PARAMETERS: vertex shader parameters; DRAW_VERTEX_INDEX: the index they give.
// DRAW_INSTANCE_INDEX_FIELDS: fields of a vertex input struct; DRAW_INSTANCE_INDEX(input): the index.

#ifndef DRAW_PARAMETERS_SLANG
#define DRAW_PARAMETERS_SLANG

#ifdef MINIENGINE_DXIL
#define DRAW_VERTEX_INDEX_PARAMETERS uint drawVertexId : SV_VertexID, uint drawStartVertex : SV_StartVertexLocation
#define DRAW_VERTEX_INDEX (drawVertexId + drawStartVertex)
#define DRAW_INSTANCE_INDEX_FIELDS     uint drawInstanceId : SV_InstanceID;     uint drawStartInstance : SV_StartInstanceLocation
#define DRAW_INSTANCE_INDEX(input) ((input).drawInstanceId + (input).drawStartInstance)
#else
#define DRAW_VERTEX_INDEX_PARAMETERS uint drawVertexIndex : SV_VulkanVertexID
#define DRAW_VERTEX_INDEX drawVertexIndex
#define DRAW_INSTANCE_INDEX_FIELDS uint drawInstanceIndex : SV_VulkanInstanceID
#define DRAW_INSTANCE_INDEX(input) ((input).drawInstanceIndex)
#endif

#endif
