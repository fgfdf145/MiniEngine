#include "material_graph_runtime.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <unordered_map>

namespace
{
constexpr const char* kSurfaceOutputSlot = "surface";
constexpr const char* kTextureOutputSlot = "texture";
constexpr const char* kScalarOutputSlot = "value";
constexpr const char* kColorOutputSlot = "color";

const MaterialShaderNode* FindNodeById(const MaterialShaderGraph& graph, uint32_t id)
{
    const auto iterator = std::find_if(graph.nodes.begin(), graph.nodes.end(), [id](const MaterialShaderNode& node)
    {
        return node.id == id;
    });
    return iterator != graph.nodes.end() ? &(*iterator) : nullptr;
}

MaterialShaderNode* FindNodeById(MaterialShaderGraph& graph, uint32_t id)
{
    const auto iterator = std::find_if(graph.nodes.begin(), graph.nodes.end(), [id](const MaterialShaderNode& node)
    {
        return node.id == id;
    });
    return iterator != graph.nodes.end() ? &(*iterator) : nullptr;
}

MaterialShaderNode* AddNode(
    MaterialShaderGraph& graph,
    MaterialShaderNodeType type,
    const char* name,
    const MaterialGraphNodePosition& position
)
{
    MaterialShaderNode node{};
    node.id = graph.nextNodeId++;
    node.type = type;
    node.name = name == nullptr ? std::string{} : std::string(name);
    node.position = position;
    graph.nodes.push_back(node);
    return &graph.nodes.back();
}

void AddLink(
    MaterialShaderGraph& graph,
    uint32_t fromNodeId,
    const char* fromSlot,
    uint32_t toNodeId,
    const char* toSlot
)
{
    if (fromSlot == nullptr || toSlot == nullptr)
    {
        return;
    }

    MaterialShaderLink link{};
    link.id = graph.nextLinkId++;
    link.fromNodeId = fromNodeId;
    link.fromSlot = fromSlot;
    link.toNodeId = toNodeId;
    link.toSlot = toSlot;
    graph.links.push_back(link);
}

MaterialShaderNodeLayout BuildFallbackLegacyLayout()
{
    return MaterialShaderNodeLayout{};
}

float ReadFloatOrFallback(const YAML::Node& node, float fallback)
{
    return node ? node.as<float>(fallback) : fallback;
}

void ReadFloatSequence(const YAML::Node& node, float* values, size_t count)
{
    if (!node || !node.IsSequence() || node.size() != count)
    {
        return;
    }

    for (size_t index = 0; index < count; ++index)
    {
        values[index] = node[index].as<float>(values[index]);
    }
}

std::optional<MaterialShaderNodeLayout> ReadLegacyLayout(const YAML::Node& shaderGraphNode)
{
    if (!shaderGraphNode || !shaderGraphNode.IsMap() || shaderGraphNode["nodes"])
    {
        return std::nullopt;
    }

    MaterialShaderNodeLayout layout{};
    const auto readPosition = [&shaderGraphNode](const char* key, MaterialGraphNodePosition& position)
    {
        const YAML::Node positionNode = shaderGraphNode[key];
        if (!positionNode || !positionNode.IsSequence() || positionNode.size() != 2)
        {
            return;
        }

        position.x = positionNode[0].as<float>(position.x);
        position.y = positionNode[1].as<float>(position.y);
    };

    readPosition("primary_surface_node", layout.primarySurfaceNode);
    readPosition("blend_node", layout.blendNode);
    readPosition("secondary_surface_node", layout.secondarySurfaceNode);
    readPosition("output_node", layout.outputNode);
    return layout;
}

std::string BuildTextureNodeName(const char* label)
{
    return std::string(label == nullptr ? "Texture" : label) + " Texture";
}

MaterialGraphNodePosition OffsetPosition(const MaterialGraphNodePosition& base, float offsetX, float offsetY)
{
    return MaterialGraphNodePosition{ base.x + offsetX, base.y + offsetY };
}

const MaterialShaderLink* FindIncomingLink(
    const MaterialShaderGraph& graph,
    uint32_t nodeId,
    std::string_view slot
)
{
    const auto iterator = std::find_if(graph.links.begin(), graph.links.end(), [&](const MaterialShaderLink& link)
    {
        return link.toNodeId == nodeId && link.toSlot == slot;
    });
    return iterator != graph.links.end() ? &(*iterator) : nullptr;
}

const MaterialShaderNode* ResolveUpstreamNode(
    const MaterialShaderGraph& graph,
    uint32_t nodeId,
    std::string_view slot,
    std::string_view expectedOutputSlot = {}
)
{
    const MaterialShaderLink* link = FindIncomingLink(graph, nodeId, slot);
    if (link == nullptr)
    {
        return nullptr;
    }
    if (!expectedOutputSlot.empty() && link->fromSlot != expectedOutputSlot)
    {
        return nullptr;
    }

    return FindNodeById(graph, link->fromNodeId);
}

void CompileSurfaceTextures(
    const MaterialShaderGraph& graph,
    const MaterialShaderNode* surfaceNode,
    bool secondaryLayer,
    ModelImportedMaterialInfo& material
)
{
    if (surfaceNode == nullptr || surfaceNode->type != MaterialShaderNodeType::Surface)
    {
        return;
    }

    auto resolveTexturePath = [&](std::string_view slot) -> std::string
    {
        const MaterialShaderNode* textureNode = ResolveUpstreamNode(graph, surfaceNode->id, slot, kTextureOutputSlot);
        return textureNode != nullptr && textureNode->type == MaterialShaderNodeType::Texture
            ? textureNode->texturePath
            : std::string{};
    };

    if (!secondaryLayer)
    {
        material.baseColorTexturePath = resolveTexturePath("base_color");
        material.normalTexturePath = resolveTexturePath("normal");
        material.metallicTexturePath = resolveTexturePath("metallic");
        material.roughnessTexturePath = resolveTexturePath("roughness");
        material.occlusionTexturePath = resolveTexturePath("occlusion");
        material.emissiveTexturePath = resolveTexturePath("emissive");
        return;
    }

    material.blendGraph.secondaryBaseColorTexturePath = resolveTexturePath("base_color");
    material.blendGraph.secondaryNormalTexturePath = resolveTexturePath("normal");
    material.blendGraph.secondaryMetallicTexturePath = resolveTexturePath("metallic");
    material.blendGraph.secondaryRoughnessTexturePath = resolveTexturePath("roughness");
    material.blendGraph.secondaryOcclusionTexturePath = resolveTexturePath("occlusion");
    material.blendGraph.secondaryEmissiveTexturePath = resolveTexturePath("emissive");
}

const MaterialShaderNode* CollapseToSurfaceNode(
    const MaterialShaderGraph& graph,
    const MaterialShaderNode* node
)
{
    const MaterialShaderNode* current = node;
    for (int depth = 0; depth < 8 && current != nullptr; ++depth)
    {
        if (current->type == MaterialShaderNodeType::Surface)
        {
            return current;
        }
        if (current->type != MaterialShaderNodeType::Blend)
        {
            return nullptr;
        }

        current = ResolveUpstreamNode(graph, current->id, "surface_a", kSurfaceOutputSlot);
    }

    return nullptr;
}

std::string BuildCompileMessage(const MaterialShaderGraph& graph, bool blendEnabled)
{
    std::ostringstream stream;
    stream << "Compiled " << graph.nodes.size() << " node(s) and " << graph.links.size() << " link(s)";
    stream << (blendEnabled ? " with layered blend output." : " into a single surface output.");
    return stream.str();
}
}

const char* ToString(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return "texture";
    case MaterialShaderNodeType::Scalar:
        return "scalar";
    case MaterialShaderNodeType::Color:
        return "color";
    case MaterialShaderNodeType::Surface:
        return "surface";
    case MaterialShaderNodeType::Blend:
        return "blend";
    case MaterialShaderNodeType::Output:
        return "output";
    default:
        return "texture";
    }
}

bool TryParseMaterialShaderNodeType(std::string_view value, MaterialShaderNodeType& type)
{
    if (value == "texture")
    {
        type = MaterialShaderNodeType::Texture;
        return true;
    }
    if (value == "scalar")
    {
        type = MaterialShaderNodeType::Scalar;
        return true;
    }
    if (value == "color")
    {
        type = MaterialShaderNodeType::Color;
        return true;
    }
    if (value == "surface")
    {
        type = MaterialShaderNodeType::Surface;
        return true;
    }
    if (value == "blend")
    {
        type = MaterialShaderNodeType::Blend;
        return true;
    }
    if (value == "output")
    {
        type = MaterialShaderNodeType::Output;
        return true;
    }
    return false;
}

MaterialShaderGraph BuildDefaultMaterialShaderGraph(
    const std::string& materialName,
    const std::string& baseColorTexturePath,
    const std::string& normalTexturePath,
    const std::string& metallicTexturePath,
    const std::string& roughnessTexturePath,
    const std::string& occlusionTexturePath,
    const std::string& emissiveTexturePath,
    const MaterialPbrSurfaceSettings& pbr,
    const MaterialTextureBlendGraph& blendGraph,
    const std::optional<MaterialShaderNodeLayout>& legacyLayout
)
{
    const MaterialShaderNodeLayout layout = legacyLayout.value_or(BuildFallbackLegacyLayout());
    MaterialShaderGraph graph{};
    const std::string outputNodeName =
        materialName.empty() ? std::string("Material Output") : materialName + " Output";

    MaterialShaderNode* outputNode = AddNode(
        graph,
        MaterialShaderNodeType::Output,
        outputNodeName.c_str(),
        layout.outputNode
    );
    outputNode->pbr = pbr;

    MaterialShaderNode* primarySurfaceNode = AddNode(
        graph,
        MaterialShaderNodeType::Surface,
        "Primary Surface",
        layout.primarySurfaceNode
    );

    auto addTextureChain = [&](const std::string& path,
                               const char* label,
                               const char* toSlot,
                               const MaterialGraphNodePosition& position,
                               uint32_t targetNodeId)
    {
        if (path.empty())
        {
            return;
        }

        MaterialShaderNode* textureNode = AddNode(
            graph,
            MaterialShaderNodeType::Texture,
            BuildTextureNodeName(label).c_str(),
            position
        );
        textureNode->texturePath = path;
        AddLink(graph, textureNode->id, kTextureOutputSlot, targetNodeId, toSlot);
    };

    const std::array<std::pair<const char*, std::string>, 6> primaryTextures = { {
        { "base_color", baseColorTexturePath },
        { "normal", normalTexturePath },
        { "metallic", metallicTexturePath },
        { "roughness", roughnessTexturePath },
        { "occlusion", occlusionTexturePath },
        { "emissive", emissiveTexturePath }
    } };
    const std::array<const char*, 6> textureLabels = {
        "Base Color", "Normal", "Metallic", "Roughness", "Occlusion", "Emissive"
    };
    for (size_t index = 0; index < primaryTextures.size(); ++index)
    {
        addTextureChain(
            primaryTextures[index].second,
            textureLabels[index],
            primaryTextures[index].first,
            OffsetPosition(layout.primarySurfaceNode, -320.0f, static_cast<float>(index) * 92.0f - 60.0f),
            primarySurfaceNode->id
        );
    }

    const bool hasSecondaryLayer =
        blendGraph.enabled ||
        !blendGraph.blendMaskTexturePath.empty() ||
        !blendGraph.secondaryBaseColorTexturePath.empty() ||
        !blendGraph.secondaryNormalTexturePath.empty() ||
        !blendGraph.secondaryMetallicTexturePath.empty() ||
        !blendGraph.secondaryRoughnessTexturePath.empty() ||
        !blendGraph.secondaryOcclusionTexturePath.empty() ||
        !blendGraph.secondaryEmissiveTexturePath.empty();

    if (!hasSecondaryLayer)
    {
        AddLink(graph, primarySurfaceNode->id, kSurfaceOutputSlot, outputNode->id, "surface");
        return graph;
    }

    MaterialShaderNode* secondarySurfaceNode = AddNode(
        graph,
        MaterialShaderNodeType::Surface,
        "Secondary Surface",
        layout.secondarySurfaceNode
    );
    MaterialShaderNode* blendNode = AddNode(
        graph,
        MaterialShaderNodeType::Blend,
        "Blend",
        layout.blendNode
    );
    blendNode->scalarValue = blendGraph.blendFactor;

    AddLink(graph, primarySurfaceNode->id, kSurfaceOutputSlot, blendNode->id, "surface_a");
    AddLink(graph, secondarySurfaceNode->id, kSurfaceOutputSlot, blendNode->id, "surface_b");
    AddLink(graph, blendNode->id, kSurfaceOutputSlot, outputNode->id, "surface");

    const std::array<std::pair<const char*, std::string>, 6> secondaryTextures = { {
        { "base_color", blendGraph.secondaryBaseColorTexturePath },
        { "normal", blendGraph.secondaryNormalTexturePath },
        { "metallic", blendGraph.secondaryMetallicTexturePath },
        { "roughness", blendGraph.secondaryRoughnessTexturePath },
        { "occlusion", blendGraph.secondaryOcclusionTexturePath },
        { "emissive", blendGraph.secondaryEmissiveTexturePath }
    } };
    for (size_t index = 0; index < secondaryTextures.size(); ++index)
    {
        addTextureChain(
            secondaryTextures[index].second,
            textureLabels[index],
            secondaryTextures[index].first,
            OffsetPosition(layout.secondarySurfaceNode, -320.0f, static_cast<float>(index) * 92.0f - 60.0f),
            secondarySurfaceNode->id
        );
    }

    if (!blendGraph.blendMaskTexturePath.empty())
    {
        MaterialShaderNode* maskTextureNode = AddNode(
            graph,
            MaterialShaderNodeType::Texture,
            "Blend Mask Texture",
            OffsetPosition(layout.blendNode, -280.0f, 28.0f)
        );
        maskTextureNode->texturePath = blendGraph.blendMaskTexturePath;
        AddLink(graph, maskTextureNode->id, kTextureOutputSlot, blendNode->id, "mask");
    }

    return graph;
}

void EnsureMaterialShaderGraph(
    const std::string& materialName,
    const std::optional<MaterialShaderNodeLayout>& legacyLayout,
    ModelImportedMaterialInfo& material
)
{
    if (!material.shaderGraph.IsEmpty())
    {
        return;
    }

    material.shaderGraph = BuildDefaultMaterialShaderGraph(
        materialName,
        material.baseColorTexturePath,
        material.normalTexturePath,
        material.metallicTexturePath,
        material.roughnessTexturePath,
        material.occlusionTexturePath,
        material.emissiveTexturePath,
        material.pbr,
        material.blendGraph,
        legacyLayout
    );
}

YAML::Node SerializeMaterialShaderGraph(const MaterialShaderGraph& graph)
{
    YAML::Node root(YAML::NodeType::Map);
    root["version"] = 2;
    root["next_node_id"] = graph.nextNodeId;
    root["next_link_id"] = graph.nextLinkId;

    YAML::Node nodesNode(YAML::NodeType::Sequence);
    for (const MaterialShaderNode& node : graph.nodes)
    {
        YAML::Node nodeMap(YAML::NodeType::Map);
        nodeMap["id"] = node.id;
        nodeMap["type"] = ToString(node.type);
        nodeMap["name"] = node.name;
        YAML::Node position(YAML::NodeType::Sequence);
        position.push_back(node.position.x);
        position.push_back(node.position.y);
        nodeMap["position"] = position;
        nodeMap["texture_path"] = node.texturePath;
        nodeMap["scalar_value"] = node.scalarValue;
        YAML::Node colorValue(YAML::NodeType::Sequence);
        for (float value : node.colorValue)
        {
            colorValue.push_back(value);
        }
        nodeMap["color_value"] = colorValue;

        YAML::Node pbr(YAML::NodeType::Map);
        YAML::Node baseColor(YAML::NodeType::Sequence);
        for (float value : node.pbr.baseColorFactor)
        {
            baseColor.push_back(value);
        }
        YAML::Node emissiveColor(YAML::NodeType::Sequence);
        for (float value : node.pbr.emissiveColor)
        {
            emissiveColor.push_back(value);
        }
        pbr["base_color_factor"] = baseColor;
        pbr["emissive_color"] = emissiveColor;
        pbr["metallic_factor"] = node.pbr.metallicFactor;
        pbr["roughness_factor"] = node.pbr.roughnessFactor;
        pbr["normal_scale"] = node.pbr.normalScale;
        pbr["occlusion_strength"] = node.pbr.occlusionStrength;
        pbr["emissive_intensity"] = node.pbr.emissiveIntensity;
        pbr["opacity"] = node.pbr.opacity;
        nodeMap["pbr"] = pbr;
        nodesNode.push_back(nodeMap);
    }
    root["nodes"] = nodesNode;

    YAML::Node linksNode(YAML::NodeType::Sequence);
    for (const MaterialShaderLink& link : graph.links)
    {
        YAML::Node linkMap(YAML::NodeType::Map);
        linkMap["id"] = link.id;
        linkMap["from_node_id"] = link.fromNodeId;
        linkMap["from_slot"] = link.fromSlot;
        linkMap["to_node_id"] = link.toNodeId;
        linkMap["to_slot"] = link.toSlot;
        linksNode.push_back(linkMap);
    }
    root["links"] = linksNode;
    return root;
}

bool DeserializeMaterialShaderGraph(
    const YAML::Node& shaderGraphNode,
    const std::string& materialName,
    const std::optional<MaterialShaderNodeLayout>& legacyLayout,
    ModelImportedMaterialInfo& material
)
{
    material.shaderGraph = {};

    if (!shaderGraphNode)
    {
        EnsureMaterialShaderGraph(materialName, legacyLayout, material);
        return false;
    }

    const std::optional<MaterialShaderNodeLayout> parsedLegacyLayout =
        legacyLayout.has_value() ? legacyLayout : ReadLegacyLayout(shaderGraphNode);
    if (!shaderGraphNode["nodes"])
    {
        EnsureMaterialShaderGraph(materialName, parsedLegacyLayout, material);
        return false;
    }

    const YAML::Node nodesNode = shaderGraphNode["nodes"];
    const YAML::Node linksNode = shaderGraphNode["links"];
    if (!nodesNode.IsSequence() || (linksNode && !linksNode.IsSequence()))
    {
        EnsureMaterialShaderGraph(materialName, parsedLegacyLayout, material);
        return false;
    }

    material.shaderGraph.nextNodeId = shaderGraphNode["next_node_id"].as<uint32_t>(1);
    material.shaderGraph.nextLinkId = shaderGraphNode["next_link_id"].as<uint32_t>(1);

    uint32_t maxNodeId = 0;
    for (const YAML::Node& nodeMap : nodesNode)
    {
        if (!nodeMap.IsMap())
        {
            continue;
        }

        MaterialShaderNode node{};
        node.id = nodeMap["id"].as<uint32_t>(0);
        MaterialShaderNodeType type = MaterialShaderNodeType::Texture;
        if (!TryParseMaterialShaderNodeType(nodeMap["type"].as<std::string>("texture"), type))
        {
            continue;
        }
        node.type = type;
        node.name = nodeMap["name"].as<std::string>(std::string{});
        const YAML::Node positionNode = nodeMap["position"];
        if (positionNode && positionNode.IsSequence() && positionNode.size() == 2)
        {
            node.position.x = positionNode[0].as<float>(node.position.x);
            node.position.y = positionNode[1].as<float>(node.position.y);
        }
        node.texturePath = nodeMap["texture_path"].as<std::string>(std::string{});
        node.scalarValue = nodeMap["scalar_value"].as<float>(node.scalarValue);
        ReadFloatSequence(nodeMap["color_value"], node.colorValue, 4);
        if (const YAML::Node pbrNode = nodeMap["pbr"]; pbrNode && pbrNode.IsMap())
        {
            ReadFloatSequence(pbrNode["base_color_factor"], node.pbr.baseColorFactor, 4);
            ReadFloatSequence(pbrNode["emissive_color"], node.pbr.emissiveColor, 3);
            node.pbr.metallicFactor = ReadFloatOrFallback(pbrNode["metallic_factor"], node.pbr.metallicFactor);
            node.pbr.roughnessFactor = ReadFloatOrFallback(pbrNode["roughness_factor"], node.pbr.roughnessFactor);
            node.pbr.normalScale = ReadFloatOrFallback(pbrNode["normal_scale"], node.pbr.normalScale);
            node.pbr.occlusionStrength = ReadFloatOrFallback(pbrNode["occlusion_strength"], node.pbr.occlusionStrength);
            node.pbr.emissiveIntensity = ReadFloatOrFallback(pbrNode["emissive_intensity"], node.pbr.emissiveIntensity);
            node.pbr.opacity = ReadFloatOrFallback(pbrNode["opacity"], node.pbr.opacity);
        }
        material.shaderGraph.nodes.push_back(node);
        maxNodeId = std::max(maxNodeId, node.id);
    }

    uint32_t maxLinkId = 0;
    for (const YAML::Node& linkMap : linksNode)
    {
        if (!linkMap.IsMap())
        {
            continue;
        }

        MaterialShaderLink link{};
        link.id = linkMap["id"].as<uint32_t>(0);
        link.fromNodeId = linkMap["from_node_id"].as<uint32_t>(0);
        link.fromSlot = linkMap["from_slot"].as<std::string>(std::string{});
        link.toNodeId = linkMap["to_node_id"].as<uint32_t>(0);
        link.toSlot = linkMap["to_slot"].as<std::string>(std::string{});
        if (FindNodeById(material.shaderGraph, link.fromNodeId) == nullptr ||
            FindNodeById(material.shaderGraph, link.toNodeId) == nullptr ||
            link.fromSlot.empty() ||
            link.toSlot.empty())
        {
            continue;
        }
        material.shaderGraph.links.push_back(link);
        maxLinkId = std::max(maxLinkId, link.id);
    }

    material.shaderGraph.nextNodeId = std::max(material.shaderGraph.nextNodeId, maxNodeId + 1);
    material.shaderGraph.nextLinkId = std::max(material.shaderGraph.nextLinkId, maxLinkId + 1);
    EnsureMaterialShaderGraph(materialName, parsedLegacyLayout, material);
    return true;
}

MaterialGraphCompileResult CompileMaterialShaderGraph(ModelImportedMaterialInfo& material)
{
    EnsureMaterialShaderGraph(material.name, std::nullopt, material);

    material.baseColorTexturePath.clear();
    material.normalTexturePath.clear();
    material.metallicTexturePath.clear();
    material.roughnessTexturePath.clear();
    material.occlusionTexturePath.clear();
    material.emissiveTexturePath.clear();
    material.blendGraph = MaterialTextureBlendGraph{};

    const auto outputIterator = std::find_if(
        material.shaderGraph.nodes.begin(),
        material.shaderGraph.nodes.end(),
        [](const MaterialShaderNode& node)
        {
            return node.type == MaterialShaderNodeType::Output;
        }
    );
    if (outputIterator == material.shaderGraph.nodes.end())
    {
        return MaterialGraphCompileResult{ false, "Material graph is missing an Output node." };
    }

    const MaterialShaderNode& outputNode = *outputIterator;
    material.pbr = outputNode.pbr;

    if (const MaterialShaderNode* baseFactorNode =
            ResolveUpstreamNode(material.shaderGraph, outputNode.id, "base_factor", kColorOutputSlot);
        baseFactorNode != nullptr && baseFactorNode->type == MaterialShaderNodeType::Color)
    {
        for (size_t index = 0; index < 4; ++index)
        {
            material.pbr.baseColorFactor[index] = baseFactorNode->colorValue[index];
        }
    }
    if (const MaterialShaderNode* emissiveColorNode =
            ResolveUpstreamNode(material.shaderGraph, outputNode.id, "emissive_color", kColorOutputSlot);
        emissiveColorNode != nullptr && emissiveColorNode->type == MaterialShaderNodeType::Color)
    {
        for (size_t index = 0; index < 3; ++index)
        {
            material.pbr.emissiveColor[index] = emissiveColorNode->colorValue[index];
        }
    }

    const auto applyScalarInput = [&](std::string_view slot, float& target)
    {
        if (const MaterialShaderNode* scalarNode =
                ResolveUpstreamNode(material.shaderGraph, outputNode.id, slot, kScalarOutputSlot);
            scalarNode != nullptr && scalarNode->type == MaterialShaderNodeType::Scalar)
        {
            target = scalarNode->scalarValue;
        }
    };
    applyScalarInput("metallic_factor", material.pbr.metallicFactor);
    applyScalarInput("roughness_factor", material.pbr.roughnessFactor);
    applyScalarInput("normal_scale", material.pbr.normalScale);
    applyScalarInput("ao_strength", material.pbr.occlusionStrength);
    applyScalarInput("emissive_intensity", material.pbr.emissiveIntensity);
    applyScalarInput("opacity", material.pbr.opacity);

    const MaterialShaderNode* surfaceSource =
        ResolveUpstreamNode(material.shaderGraph, outputNode.id, "surface", kSurfaceOutputSlot);
    if (surfaceSource == nullptr)
    {
        return MaterialGraphCompileResult{
            true,
            BuildCompileMessage(material.shaderGraph, false)
        };
    }

    bool blendEnabled = false;
    if (surfaceSource->type == MaterialShaderNodeType::Blend)
    {
        const MaterialShaderNode* primarySurface = CollapseToSurfaceNode(
            material.shaderGraph,
            ResolveUpstreamNode(material.shaderGraph, surfaceSource->id, "surface_a", kSurfaceOutputSlot)
        );
        const MaterialShaderNode* secondarySurface = CollapseToSurfaceNode(
            material.shaderGraph,
            ResolveUpstreamNode(material.shaderGraph, surfaceSource->id, "surface_b", kSurfaceOutputSlot)
        );

        CompileSurfaceTextures(material.shaderGraph, primarySurface, false, material);
        CompileSurfaceTextures(material.shaderGraph, secondarySurface, true, material);

        material.blendGraph.enabled = secondarySurface != nullptr;
        material.blendGraph.blendFactor = surfaceSource->scalarValue;
        if (const MaterialShaderNode* factorNode =
                ResolveUpstreamNode(material.shaderGraph, surfaceSource->id, "factor", kScalarOutputSlot);
            factorNode != nullptr && factorNode->type == MaterialShaderNodeType::Scalar)
        {
            material.blendGraph.blendFactor = factorNode->scalarValue;
        }
        if (const MaterialShaderNode* maskNode =
                ResolveUpstreamNode(material.shaderGraph, surfaceSource->id, "mask", kTextureOutputSlot);
            maskNode != nullptr && maskNode->type == MaterialShaderNodeType::Texture)
        {
            material.blendGraph.blendMaskTexturePath = maskNode->texturePath;
        }
        blendEnabled = material.blendGraph.enabled;
    }
    else
    {
        const MaterialShaderNode* compiledSurface = CollapseToSurfaceNode(material.shaderGraph, surfaceSource);
        CompileSurfaceTextures(material.shaderGraph, compiledSurface, false, material);
    }

    return MaterialGraphCompileResult{
        true,
        BuildCompileMessage(material.shaderGraph, blendEnabled)
    };
}
