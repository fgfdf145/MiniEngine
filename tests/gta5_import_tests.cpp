#include <engine/asset/gta5_game_data.h>
#include <engine/asset/gta5_importer.h>
#include <engine/asset/gta5_resource.h>
#include <engine/asset/model_loader.h>
#include <engine/asset/rage_resource.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void RequireThrows(Function&& function, const std::string& what)
{
    try
    {
        function();
    }
    catch (const std::exception&)
    {
        return;
    }
    throw std::runtime_error(what + " did not throw");
}

class ScopedDirectory
{
  public:
    ScopedDirectory()
    {
        std::random_device randomDevice;
        m_path = std::filesystem::temp_directory_path() /
                 ("miniengine_gta5_import_" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "_" +
                  std::to_string(randomDevice()));
        std::filesystem::create_directories(m_path);
    }

    ~ScopedDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

void TestJenkinsHash()
{
    // The game's model hash of the Adder, as scripts know it (0xB779A091), and case-blind.
    Require(JenkinsHash("adder") == 0xB779A091u, "JenkinsHash(adder)");
    Require(JenkinsHash("ADDER") == 0xB779A091u, "JenkinsHash ignores case");
    Require(JenkinsHash("DiffuseTex") == 3370697346u, "JenkinsHash(DiffuseTex)");
}

void TestPageSizes()
{
    // adder.yft's system flags and adder.ytd's graphics flags.
    Require(RageResource::PagesSize(0xA1180332u) == 1982464, "system pages of adder.yft");
    Require(RageResource::PagesSize(0x50000040u) == 131072, "graphics pages of adder.ytd");
}

void TestResourceBounds()
{
    std::vector<std::uint8_t> system(64, 0);
    system[16] = 'h';
    system[17] = 'i';
    const RageResource resource = RageResource::FromPages(171, system, {}, "test");
    Require(resource.ReadString(0x50000010) == "hi", "ReadString");
    Require(resource.ReadString(0).empty(), "ReadString of null");
    RequireThrows([&] { resource.Read<std::uint64_t>(0x5000003C); }, "a read past the system pages");
    RequireThrows([&] { resource.Read<std::uint32_t>(0x60000000); }, "a read of absent graphics pages");
    RequireThrows([&] { resource.Read<std::uint32_t>(0x12345678); }, "a read of a non-resource address");
    RequireThrows([] { RageResource::FromFile({'R', 'S', 'C', '8'}, "bad"); }, "a file without the RSC7 magic");
}

void TestPaintSlots()
{
    Require(Gta5Importer::PaintSlot({2.0f, 1.0f, 1.0f}) == 1, "primary");
    Require(Gta5Importer::PaintSlot({2.0f, 2.0f, 2.0f}) == 2, "secondary");
    Require(Gta5Importer::PaintSlot({2.0f, 4.0f, 4.0f}) == 4, "wheels");
    Require(Gta5Importer::PaintSlot({2.0f, 5.0f, 5.0f}) == 0, "5 is no colour");
    Require(Gta5Importer::PaintSlot({1.0f, 1.0f, 1.0f}) == 0, "a plain tint is no slot");
    Require(Gta5Importer::PaintSlot({}) == 0, "absent");
    Require(Gta5Importer::GlossToRoughness(1.0f) < Gta5Importer::GlossToRoughness(0.0f), "gloss lowers roughness");
}

void TestPalette()
{
    Require(Gta5PaletteEntry(0).has_value() && Gta5PaletteEntry(0)->name == "Metallic Black", "palette 0");
    Require(Gta5PaletteEntry(120)->finish == Gta5PaintFinish::Chrome, "palette 120 is chrome");
    Require(Gta5PaletteEntry(160).has_value() && !Gta5PaletteEntry(161).has_value(), "161 colours");
    const glm::vec3 white = Gta5PaletteLinear(134);
    Require(std::abs(white.r - 1.0f) < 1e-5f && std::abs(white.b - 1.0f) < 1e-5f, "pure white is linear 1");
}

void TestXml()
{
    const Gta5GameDataRules::XmlElement root = Gta5GameDataRules::ParseXml(
        "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!-- c -->\n<CHandlingDataMgr><HandlingData><Item type=\"CHandlingData\">"
        "<handlingName>ADDER</handlingName><fMass value=\"1800.000000\" /><vecCentreOfMassOffset x=\"0\" y=\"0.1\" z=\"-0.2\"/>"
        "<!-- inner --></Item></HandlingData></CHandlingDataMgr>");
    Require(root.name == "CHandlingDataMgr", "root name");
    const Gta5GameDataRules::XmlElement* item = root.Child("HandlingData")->Child("Item");
    Require(item != nullptr && item->attributes.at("type") == "CHandlingData", "item");
    Require(item->ChildText("handlingName") == "ADDER", "text");
    Require(item->ChildValue("fMass").value_or(0.0f) == 1800.0f, "value attribute");
    RequireThrows([] { Gta5GameDataRules::ParseXml("<a><b></a>"); }, "mismatched tags");
}

void TestMetaPriority()
{
    using Gta5GameDataRules::MetaPriority;
    const int base = MetaPriority("base/meta/common/data/levels/gta5/vehicles.meta");
    const int dlc = MetaPriority("dlc/mpheist/meta/dlc/common/data/levels/gta5/vehicles.meta");
    const int patch = MetaPriority("dlc/patch2023_01/meta/dlc/common/data/levels/gta5/vehicles.meta");
    const int update = MetaPriority("base/meta/update/common/data/levels/gta5/vehicles.meta");
    const int updatePatch = MetaPriority("base/meta/update/dlc_patch/mpheist/common/data/levels/gta5/vehicles.meta");
    Require(base < dlc && dlc < patch && patch < update && update < updatePatch, "meta priorities");
}

// The real thing, when the extracted game files are there: MINIENGINE_GTA5_CARS names the extraction
// root (the folder with base/ and dlc/).
void TestImportFromGameFiles()
{
    const char* root = std::getenv("MINIENGINE_GTA5_CARS");
    if (root == nullptr || *root == '\0')
    {
        std::cout << "  (MINIENGINE_GTA5_CARS not set: skipping the import of real vehicles)\n";
        return;
    }
    // A base game car (no carvariations.meta: the fallback colours) and a DLC one.
    for (const char* file : {"base/vehicles/adder.yft", "dlc/mp2024_01/vehicles/coquette5.yft"})
    {
        const std::filesystem::path source = std::filesystem::path(root) / file;
        ScopedDirectory directory;
        const auto start = std::chrono::steady_clock::now();
        const Gta5ImportReport report = Gta5Importer::ConvertToGltf(source, directory.Path());
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "  " << file << ": " << report.triangles << " triangles, " << report.meshes << " meshes, " << report.wheels
                  << " wheels, " << report.materials << " materials, " << report.images << " images, " << report.colorVariants
                  << " colour sets, " << report.missingTextures.size() << " missing textures, " << seconds << " s\n";
        Require(report.fragmentPath.filename().string().find("_hi") != std::string::npos, std::string(file) + ": the _hi fragment");
        Require(report.wheels == 4, std::string(file) + ": four wheels");
        Require(report.triangles > 10000, std::string(file) + ": a detailed model");

        const LoadedModelData model = ModelLoader::LoadModel(report.gltfPath.string());
        Require(model.wheelRig.has_value(), std::string(file) + ": the engine finds the wheel rig");
        Require(model.steeringWheel.has_value(), std::string(file) + ": the engine finds the steering wheel");
        Require(model.materialVariants.size() > 1, std::string(file) + ": colour variants");
        const ModelWheelRig& rig = *model.wheelRig;
        // Left wheels are at -X in GTA and in glTF; the front ones at -Z (glTF's forward).
        Require(rig.corners[0].center.x < 0.0f && rig.corners[1].center.x > 0.0f, std::string(file) + ": left and right");
        Require(rig.corners[0].center.z < rig.corners[2].center.z, std::string(file) + ": front ahead of rear");
        Require(rig.corners[0].radius > 0.2f && rig.corners[0].radius < 0.6f, std::string(file) + ": a wheel's radius");
        Require(std::abs(model.minBounds.y) < 0.02f, std::string(file) + ": the tyres on the ground");
        Require(model.maxBounds.y - model.minBounds.y > 0.8f && model.maxBounds.y - model.minBounds.y < 3.0f,
                std::string(file) + ": a car's height");
    }
}
}

int main()
{
    const std::pair<const char*, void (*)()> tests[] = {
        {"JenkinsHash", TestJenkinsHash},
        {"PageSizes", TestPageSizes},
        {"ResourceBounds", TestResourceBounds},
        {"PaintSlots", TestPaintSlots},
        {"Palette", TestPalette},
        {"Xml", TestXml},
        {"MetaPriority", TestMetaPriority},
        {"ImportFromGameFiles", TestImportFromGameFiles},
    };
    int failures = 0;
    for (const auto& [name, test] : tests)
    {
        try
        {
            test();
            std::cout << "[ OK ] " << name << "\n";
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cout << "[FAIL] " << name << ": " << error.what() << "\n";
        }
    }
    return failures == 0 ? 0 : 1;
}
