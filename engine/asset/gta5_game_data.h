#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

// What GTA V's data files say about a vehicle, read from a folder the game's files were copied out
// into: the .meta files (vehicles.meta, carvariations.meta, handling.meta, as XML) and the texture
// dictionaries (.ytd) anywhere below it. The folder is the nearest one above the .yft that holds a
// "base" folder or a "manifest.tsv" (the layout of an extraction of base game, update and DLC packs).

// How a palette colour's paint is finished (carcols' EVehicleModelColorMetallic kinds).
enum class Gta5PaintFinish : std::uint8_t
{
    Metallic,
    Util,   // plain gloss
    Matte,
    Worn,
    Brushed,
    Chrome,
    Gold,
    Satin,
};

// One of the game's 161 numbered vehicle colours (carcols.ymt's colour list).
struct Gta5PaletteColor
{
    std::uint32_t rgb = 0; // 0xRRGGBB, sRGB
    Gta5PaintFinish finish = Gta5PaintFinish::Metallic;
    std::string_view name;
};

// The palette entry for an index; nullopt past the end.
std::optional<Gta5PaletteColor> Gta5PaletteEntry(int index);
// Its colour as linear RGB.
glm::vec3 Gta5PaletteLinear(int index);

// A vehicle's colour set (carvariations' colors item): palette indices for the primary, secondary
// and pearlescent paint, the wheels, the interior trim and the dashboard.
struct Gta5ColorSet
{
    std::array<int, 6> indices{0, 0, 0, 156, 0, 0};
};

// handling.meta's figures for one handlingName, in the game's own units (see the design doc for
// what they mean): fMass, fDriveBiasFront, ... keyed by the element's name; vectors as x, y, z.
struct Gta5Handling
{
    std::string name;
    std::map<std::string, float> values;
    std::map<std::string, glm::vec3> vectors;
    std::string modelFlags;
    std::string handlingFlags;

    float Value(const std::string& key, float fallback) const;
};

struct Gta5VehicleInfo
{
    std::string modelName;
    std::string gameName;
    std::string makeName;
    std::string txdName;
    std::string handlingId;
    std::string vehicleClass;
    std::string type; // VEHICLE_TYPE_CAR, ...
    // vehicles.meta's wheelScale / wheelScaleRear: the tyre's radius over which the mod wheels are scaled.
    float wheelScale = 0.0f;
    float wheelScaleRear = 0.0f;
    std::vector<Gta5ColorSet> colorSets;
    std::optional<Gta5Handling> handling;
};

class Gta5GameData
{
  public:
    // Indexes the extraction root above `anyFile` (see above): every .ytd's path and every vehicle's
    // meta. Throws nothing: a root that is not found leaves the index to the file's own folder.
    static std::shared_ptr<const Gta5GameData> ForFile(const std::filesystem::path& anyFile);

    const std::filesystem::path& Root() const
    {
        return m_root;
    }

    // A model's entry, by its name (ignoring case); nullopt when no vehicles.meta lists it.
    std::optional<Gta5VehicleInfo> Vehicle(const std::string& modelName) const;

    // The texture dictionaries a model's textures may come from, best first: "<txd>+hi", "<txd>", then
    // each parent txd vehicles.meta's txdRelationships names, ending at vehshare. Of the files of one
    // name the one nearest `near` (the same pack) comes first, then the newest patch.
    std::vector<std::filesystem::path> TextureDictionaries(const std::string& txdName, const std::filesystem::path& near) const;

  private:
    std::filesystem::path m_root;
    std::map<std::string, std::vector<std::filesystem::path>> m_dictionaries; // lower-case stem -> files
    std::map<std::string, std::string> m_txdParents;                          // lower-case child -> parent
    std::map<std::string, Gta5VehicleInfo> m_vehicles;                        // lower-case model -> info
    std::map<std::string, Gta5Handling> m_handlings;                          // lower-case handlingName -> data
};

// ---- The rules, exposed for tests --------------------------------------------------------------

namespace Gta5GameDataRules
{
// A minimal XML element tree: what GTA V's .meta files use (elements, attributes, text, comments).
struct XmlElement
{
    std::string name;
    std::map<std::string, std::string> attributes;
    std::string text;
    std::vector<XmlElement> children;

    const XmlElement* Child(std::string_view childName) const;
    std::string ChildText(std::string_view childName) const;
    // A child's "value" attribute as a float.
    std::optional<float> ChildValue(std::string_view childName) const;
};
// Throws std::runtime_error for malformed text.
XmlElement ParseXml(std::string_view text);

// How much a meta file's place in the extraction outranks others for the same vehicle: the base
// game < DLC packs < update < update's DLC patches, later DLC packs before earlier ones by name.
int MetaPriority(const std::filesystem::path& relativePath);
}

}
