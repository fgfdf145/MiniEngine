#include "tyre_library.h"

#include "ac_car_data.h"
#include "asset_registry.h"

#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/core/text/ascii.h>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace me::TyreLibrary
{

namespace
{
constexpr int kFormatVersion = 1;

// The shortest text that reads back as the same float.
std::string Number(float value)
{
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

void EmitCurve(YAML::Emitter& out, const std::vector<glm::vec2>& points)
{
    out << YAML::Flow << YAML::BeginSeq;
    for (const glm::vec2& point : points)
    {
        out << YAML::Flow << YAML::BeginSeq << Number(point.x) << Number(point.y) << YAML::EndSeq;
    }
    out << YAML::EndSeq;
}

std::vector<glm::vec2> ReadCurve(const YAML::Node& node)
{
    std::vector<glm::vec2> points;
    if (!node.IsSequence())
    {
        return points;
    }
    for (const YAML::Node& point : node)
    {
        if (point.IsSequence() && point.size() == 2)
        {
            points.emplace_back(point[0].as<float>(), point[1].as<float>());
        }
    }
    return points;
}

bool SameTyre(tyre::TyreSpec a, tyre::TyreSpec b)
{
    a.source.clear();
    b.source.clear();
    return a == b;
}

std::string RefPath(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::weakly_canonical(path, ec);
    const std::filesystem::path root = std::filesystem::weakly_canonical(EnginePaths::ProjectRoot(), ec);
    const std::filesystem::path relative = absolute.lexically_relative(root);
    if (!relative.empty() && *relative.begin() != "..")
    {
        return relative.generic_string();
    }
    return absolute.generic_string();
}
}

bool IsTyreFile(const std::filesystem::path& path)
{
    const std::string name = ToLowerAscii(path.filename().string());
    return name.size() > kSuffix.size() && name.ends_with(kSuffix);
}

std::filesystem::path Root()
{
    return EnginePaths::AssetsRoot() / "tyres";
}

std::string ToYaml(const tyre::TyreSpec& spec)
{
    // The table hands out its fields by reference.
    tyre::TyreSpec mutableSpec = spec;
    YAML::Emitter out;
    out << YAML::BeginMap << YAML::Key << "tyre" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "version" << YAML::Value << kFormatVersion;
    out << YAML::Key << "name" << YAML::Value << YAML::DoubleQuoted << spec.name;
    out << YAML::Key << "short_name" << YAML::Value << YAML::DoubleQuoted << spec.shortName;
    out << YAML::Key << "source" << YAML::Value << YAML::DoubleQuoted << spec.source;

    // The groups in the table's order, each with its scalars then its curves.
    std::vector<std::string> groups;
    for (const tyre::TyreSpecField& field : tyre::TyreSpecFields())
    {
        if (std::find(groups.begin(), groups.end(), field.group) == groups.end())
        {
            groups.emplace_back(field.group);
        }
    }
    for (const std::string& group : groups)
    {
        bool open = false;
        const auto begin = [&]
        {
            if (!open)
            {
                out << YAML::Key << group << YAML::Value << YAML::BeginMap;
                open = true;
            }
        };
        for (const tyre::TyreSpecField& field : tyre::TyreSpecFields())
        {
            if (group == field.group && field.field(mutableSpec).has_value())
            {
                begin();
                out << YAML::Key << field.key << YAML::Value << Number(*field.field(mutableSpec));
            }
        }
        for (const tyre::TyreSpecCurve& curve : tyre::TyreSpecCurves())
        {
            if (group == curve.group && !curve.field(mutableSpec).empty())
            {
                begin();
                out << YAML::Key << curve.key << YAML::Value;
                EmitCurve(out, curve.field(mutableSpec));
            }
        }
        if (open)
        {
            out << YAML::EndMap;
        }
    }
    if (!spec.extraValues.empty())
    {
        out << YAML::Key << "extra_values" << YAML::Value << YAML::BeginMap;
        for (const auto& [key, value] : spec.extraValues)
        {
            out << YAML::Key << key << YAML::Value << Number(value);
        }
        out << YAML::EndMap;
    }
    if (!spec.extraCurves.empty())
    {
        out << YAML::Key << "extra_curves" << YAML::Value << YAML::BeginMap;
        for (const auto& [key, points] : spec.extraCurves)
        {
            out << YAML::Key << key << YAML::Value;
            EmitCurve(out, points);
        }
        out << YAML::EndMap;
    }
    out << YAML::EndMap << YAML::EndMap;
    return std::string(out.c_str()) + "\n";
}

std::optional<tyre::TyreSpec> FromYaml(const std::string& text, std::string* problem)
{
    try
    {
        const YAML::Node root = YAML::Load(text);
        const YAML::Node node = root["tyre"];
        if (!node || !node.IsMap())
        {
            if (problem != nullptr)
            {
                *problem = "no 'tyre' map";
            }
            return std::nullopt;
        }
        if (node["version"] && node["version"].as<int>() > kFormatVersion)
        {
            if (problem != nullptr)
            {
                *problem = "written by a newer version (" + node["version"].as<std::string>() + ")";
            }
            return std::nullopt;
        }
        tyre::TyreSpec spec;
        spec.name = node["name"] ? node["name"].as<std::string>() : "";
        spec.shortName = node["short_name"] ? node["short_name"].as<std::string>() : "";
        spec.source = node["source"] ? node["source"].as<std::string>() : "";
        for (const tyre::TyreSpecField& field : tyre::TyreSpecFields())
        {
            const YAML::Node group = node[field.group];
            if (group && group.IsMap() && group[field.key])
            {
                field.field(spec) = group[field.key].as<float>();
            }
        }
        for (const tyre::TyreSpecCurve& curve : tyre::TyreSpecCurves())
        {
            const YAML::Node group = node[curve.group];
            if (group && group.IsMap() && group[curve.key])
            {
                curve.field(spec) = ReadCurve(group[curve.key]);
            }
        }
        if (const YAML::Node extra = node["extra_values"]; extra && extra.IsMap())
        {
            for (const auto& item : extra)
            {
                spec.extraValues[item.first.as<std::string>()] = item.second.as<float>();
            }
        }
        if (const YAML::Node extra = node["extra_curves"]; extra && extra.IsMap())
        {
            for (const auto& item : extra)
            {
                spec.extraCurves[item.first.as<std::string>()] = ReadCurve(item.second);
            }
        }
        return spec;
    }
    catch (const std::exception& error)
    {
        if (problem != nullptr)
        {
            *problem = error.what();
        }
        return std::nullopt;
    }
}

std::optional<tyre::TyreSpec> Load(const std::filesystem::path& path, std::string* problem)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        if (problem != nullptr)
        {
            *problem = "cannot open '" + path.string() + "'";
        }
        return std::nullopt;
    }
    std::stringstream text;
    text << file.rdbuf();
    return FromYaml(text.str(), problem);
}

void Save(const std::filesystem::path& path, const tyre::TyreSpec& spec)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        throw std::runtime_error("Cannot write '" + path.string() + "'");
    }
    const std::string text = ToYaml(spec);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file)
    {
        throw std::runtime_error("Cannot write '" + path.string() + "'");
    }
}

VehicleTyreRef RefFor(const std::filesystem::path& path)
{
    VehicleTyreRef ref;
    ref.uuid = AssetRegistry::GetOrCreateUuid(path);
    ref.path = RefPath(path);
    return ref;
}

VehicleTyreRef Store(const tyre::TyreSpec& spec, const std::filesystem::path& folder, const std::string& stem)
{
    for (const Entry& entry : List())
    {
        if (SameTyre(entry.spec, spec))
        {
            return entry.ref;
        }
    }
    const std::filesystem::path directory = folder.is_absolute() ? folder : Root() / folder;
    const std::string base = stem.empty() ? std::string("tyre") : stem;
    for (int attempt = 1;; ++attempt)
    {
        const std::filesystem::path path =
            directory / ((attempt == 1 ? base : base + "_" + std::to_string(attempt)) + std::string(kSuffix));
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
        {
            // The same tyre from the same source read again (the game's data read more fully): updated in place,
            // its uuid kept. Otherwise another tyre (an equal one would have been found above), or one that
            // cannot be read.
            const std::optional<tyre::TyreSpec> there = Load(path);
            if (!there.has_value() || spec.source.empty() || there->source != spec.source)
            {
                continue;
            }
        }
        Save(path, spec);
        return RefFor(path);
    }
}

std::optional<tyre::TyreSpec> Resolve(const VehicleTyreRef& ref, std::filesystem::path* resolvedPath)
{
    if (ref.Empty())
    {
        return std::nullopt;
    }
    const ResolvedAssetReference resolved = AssetRegistry::ResolveReference(ref.uuid, ref.path);
    if (!resolved.resolved)
    {
        return std::nullopt;
    }
    const std::filesystem::path path = EnginePaths::ResolveProjectPath(resolved.path);
    std::string problem;
    std::optional<tyre::TyreSpec> spec = Load(path, &problem);
    if (!spec.has_value())
    {
        LOG_WARN("Tyre '{}' could not be read: {}", path.string(), problem);
        return std::nullopt;
    }
    if (resolvedPath != nullptr)
    {
        *resolvedPath = path;
    }
    return spec;
}

std::vector<Entry> List()
{
    std::vector<Entry> entries;
    const std::filesystem::path root = Root();
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
    {
        return entries;
    }
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec) || !IsTyreFile(it->path()))
        {
            continue;
        }
        std::string problem;
        if (std::optional<tyre::TyreSpec> spec = Load(it->path(), &problem))
        {
            entries.push_back(Entry{it->path(), RefFor(it->path()), std::move(*spec)});
        }
        else
        {
            LOG_WARN("Tyre '{}' could not be read: {}", it->path().string(), problem);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    return entries;
}

std::string StemFor(const std::string& name)
{
    std::string stem;
    for (const char c : ToLowerAscii(name))
    {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (keep)
        {
            stem += c;
        }
        else if (!stem.empty() && stem.back() != '_')
        {
            stem += '_';
        }
    }
    while (!stem.empty() && stem.back() == '_')
    {
        stem.pop_back();
    }
    return stem.empty() ? std::string("tyre") : stem;
}

void AdoptCarTyres(VehicleCarSpec& spec, const std::string& carFolder)
{
    if (spec.tyreCompounds.empty())
    {
        return;
    }
    spec.libraryCompounds.clear();
    for (size_t index = 0; index < spec.tyreCompounds.size(); ++index)
    {
        const VehicleTyreCompound& compound = spec.tyreCompounds[index];
        VehicleTyreCompoundRefs refs;
        refs.name = !compound.front.name.empty() ? compound.front.name : compound.rear.name;
        const tyre::TyreSpec front = tyre::TyreSpecFromAc(compound.front.name, compound.front.shortName, compound.front.values, compound.front.curves);
        const tyre::TyreSpec rear = tyre::TyreSpecFromAc(compound.rear.name, compound.rear.shortName, compound.rear.values, compound.rear.curves);
        const std::string stem = StemFor(refs.name);
        const std::string source = "Assetto Corsa " + carFolder + ", compound " + std::to_string(index) + " " + refs.name;
        if (front == rear)
        {
            // One tyre on both axles (the R34's): one file, named for neither.
            tyre::TyreSpec both = front;
            both.source = source + ", front and rear";
            refs.front = refs.rear = Store(both, carFolder, stem);
        }
        else
        {
            tyre::TyreSpec stored = front;
            stored.source = source + ", front";
            refs.front = Store(stored, carFolder, stem + "_front");
            stored = rear;
            stored.source = source + ", rear";
            refs.rear = Store(stored, carFolder, stem + "_rear");
        }
        spec.libraryCompounds.push_back(std::move(refs));
    }
    const size_t fitted = spec.defaultTyreCompound.has_value() && *spec.defaultTyreCompound >= 0 &&
                                  static_cast<size_t>(*spec.defaultTyreCompound) < spec.libraryCompounds.size()
                              ? static_cast<size_t>(*spec.defaultTyreCompound)
                              : 0;
    for (size_t wheel = 0; wheel < kVehicleWheelCount; ++wheel)
    {
        spec.wheelTyreRefs[wheel] = wheel < 2 ? spec.libraryCompounds[fitted].front : spec.libraryCompounds[fitted].rear;
    }
}

nlohmann::json CarTyresToJson(const VehicleCarSpec& spec)
{
    const bool anyWheel = std::any_of(spec.wheelTyreRefs.begin(), spec.wheelTyreRefs.end(), [](const VehicleTyreRef& ref) { return !ref.Empty(); });
    if (spec.libraryCompounds.empty() && !anyWheel)
    {
        return nullptr;
    }
    const auto ref = [](const VehicleTyreRef& value) { return nlohmann::json{{"uuid", value.uuid}, {"path", value.path}}; };
    nlohmann::json compounds = nlohmann::json::array();
    for (const VehicleTyreCompoundRefs& compound : spec.libraryCompounds)
    {
        compounds.push_back(nlohmann::json{{"name", compound.name}, {"front", ref(compound.front)}, {"rear", ref(compound.rear)}});
    }
    nlohmann::json wheels = nlohmann::json::array();
    for (const VehicleTyreRef& wheel : spec.wheelTyreRefs)
    {
        wheels.push_back(ref(wheel));
    }
    return nlohmann::json{{"compounds", std::move(compounds)}, {"wheels", std::move(wheels)}};
}

void WriteCarTyres(const std::filesystem::path& gltfPath, const VehicleCarSpec& spec)
{
    std::ifstream in(gltfPath, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("Cannot open '" + gltfPath.string() + "'");
    }
    std::stringstream text;
    text << in.rdbuf();
    in.close();
    nlohmann::json document = nlohmann::json::parse(text.str());
    if (!document.contains("extensions") || !document["extensions"].contains("MINIENGINE_vehicle"))
    {
        throw std::runtime_error("'" + gltfPath.string() + "' carries no car data (MINIENGINE_vehicle)");
    }
    nlohmann::json& vehicle = document["extensions"]["MINIENGINE_vehicle"];
    const nlohmann::json tyres = CarTyresToJson(spec);
    if (tyres.is_null())
    {
        vehicle.erase("tyres");
    }
    else
    {
        vehicle["tyres"] = tyres;
    }
    // Written beside it and moved over it, so a failed write leaves the car as it was.
    const std::filesystem::path temporary = gltfPath.string() + ".tyres.tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const std::string dumped = document.dump();
        out.write(dumped.data(), static_cast<std::streamsize>(dumped.size()));
        if (!out)
        {
            throw std::runtime_error("Cannot write '" + temporary.string() + "'");
        }
    }
    std::error_code renamed;
    std::filesystem::rename(temporary, gltfPath, renamed);
    if (renamed)
    {
        // Another program holds the glTF without sharing its deletion (Windows refuses the move then):
        // written over in place instead.
        std::filesystem::copy_file(temporary, gltfPath, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::remove(temporary, renamed);
    }
}

size_t AdoptGltfCarTyres(const std::filesystem::path& gltfPath, const std::string& carFolder)
{
    // The game's own car folder: its compounds read afresh from its data.acd (or data/).
    std::error_code ec;
    if (std::filesystem::is_directory(carFolder, ec))
    {
        std::string problem;
        std::optional<VehicleCarSpec> game = AcCarData::ReadCarFolder(carFolder, &problem);
        if (!game.has_value() || game->tyreCompounds.empty())
        {
            throw std::runtime_error("'" + carFolder + "' has no tyres to read: " + problem);
        }
        VehicleCarSpec spec;
        spec.tyreCompounds = std::move(game->tyreCompounds);
        spec.defaultTyreCompound = game->defaultTyreCompound;
        AdoptCarTyres(spec, std::filesystem::path(carFolder).filename().string());
        WriteCarTyres(gltfPath, spec);
        return 2 * spec.libraryCompounds.size();
    }
    std::ifstream in(gltfPath, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("Cannot open '" + gltfPath.string() + "'");
    }
    std::stringstream text;
    text << in.rdbuf();
    const nlohmann::json document = nlohmann::json::parse(text.str());
    const nlohmann::json* vehicle = nullptr;
    if (document.contains("extensions") && document["extensions"].contains("MINIENGINE_vehicle"))
    {
        vehicle = &document["extensions"]["MINIENGINE_vehicle"];
    }
    if (vehicle == nullptr || !vehicle->contains("tyreCompounds") || !(*vehicle)["tyreCompounds"].is_array())
    {
        throw std::runtime_error("'" + gltfPath.string() + "' carries no tyre compounds (MINIENGINE_vehicle.tyreCompounds)");
    }
    // As the kn5 import writes them: {"front": {"name", "shortName", "values": {key: n}, "curves": {key: [[x, y]]}}, "rear": ...}.
    const auto axle = [](const nlohmann::json& compound, const char* key)
    {
        VehicleTyreData data;
        if (!compound.contains(key) || !compound[key].is_object())
        {
            return data;
        }
        const nlohmann::json& object = compound[key];
        data.name = object.value("name", std::string());
        data.shortName = object.value("shortName", std::string());
        if (object.contains("values") && object["values"].is_object())
        {
            for (const auto& [name, value] : object["values"].items())
            {
                if (value.is_number())
                {
                    data.values[name] = value.get<float>();
                }
            }
        }
        if (object.contains("curves") && object["curves"].is_object())
        {
            for (const auto& [name, points] : object["curves"].items())
            {
                std::vector<glm::vec2>& curve = data.curves[name];
                for (const nlohmann::json& point : points)
                {
                    if (point.is_array() && point.size() == 2 && point[0].is_number() && point[1].is_number())
                    {
                        curve.emplace_back(point[0].get<float>(), point[1].get<float>());
                    }
                }
            }
        }
        return data;
    };
    VehicleCarSpec spec;
    for (const nlohmann::json& compound : (*vehicle)["tyreCompounds"])
    {
        spec.tyreCompounds.push_back(VehicleTyreCompound{axle(compound, "front"), axle(compound, "rear")});
    }
    if (vehicle->contains("defaultTyreCompound") && (*vehicle)["defaultTyreCompound"].is_number())
    {
        spec.defaultTyreCompound = (*vehicle)["defaultTyreCompound"].get<int>();
    }
    in.close();
    AdoptCarTyres(spec, carFolder);
    WriteCarTyres(gltfPath, spec);
    return 2 * spec.libraryCompounds.size();
}

int ResolveWheelTyres(VehicleCarSpec& spec)
{
    const std::array<std::optional<tyre::TyreSpec>, kVehicleWheelCount> compound = DefaultWheelTyres(spec);
    int fallbacks = 0;
    for (size_t wheel = 0; wheel < kVehicleWheelCount; ++wheel)
    {
        spec.wheelTyres[wheel] = Resolve(spec.wheelTyreRefs[wheel]);
        if (!spec.wheelTyres[wheel].has_value())
        {
            if (!spec.wheelTyreRefs[wheel].Empty())
            {
                ++fallbacks;
            }
            spec.wheelTyres[wheel] = compound[wheel];
        }
    }
    return fallbacks;
}
}
