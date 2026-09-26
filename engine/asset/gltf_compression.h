#pragma once

#include <optional>
#include <string>

namespace tinygltf
{
class Model;
}

namespace me
{

// EXT_meshopt_compression and KHR_meshopt_compression. A compressed file's fallback buffer has no
// uri and no data (only readers without the extension would need it), which tinygltf refuses to
// parse. Returns the glTF JSON with each such buffer given one byte of inline data, or nullopt when
// it has none. The fallback is never read: DecodeMeshoptBufferViews points every view off it.
std::optional<std::string> ReplaceMeshoptFallbackBuffers(const std::string& json);

// Decodes every meshopt-compressed buffer view (attributes, triangles or indices, then its octahedral,
// quaternion, exponential or colour filter) into a buffer of its own and points the view at it, so
// accessors read it as plain data. Throws std::runtime_error on data that does not decode.
void DecodeMeshoptBufferViews(tinygltf::Model& model);

// KHR_draco_mesh_compression. Decodes each compressed primitive and gives its attribute accessors
// and index accessor buffer views holding the decoded data: attributes in the accessor's own
// component type (float ones dequantized by draco, integer ones left for the accessor's normalized
// flag), indices as 32-bit. Throws std::runtime_error on data that does not decode.
void DecodeDracoPrimitives(tinygltf::Model& model);
}
