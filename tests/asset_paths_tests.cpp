#include <engine/asset/asset_paths.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <system_error>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void SameOrInside()
{
    Require(AssetPaths::IsSameOrInside("C:/assets/Tree", "C:/assets/Tree"), "a folder is not inside itself");
    Require(AssetPaths::IsSameOrInside("C:/assets/Tree/leaves", "C:/assets/Tree"), "a child folder is not inside");
    Require(!AssetPaths::IsSameOrInside("C:/assets/Trees", "C:/assets/Tree"), "a name prefix counted as inside");
    Require(!AssetPaths::IsSameOrInside("C:/assets", "C:/assets/Tree"), "a parent counted as inside");
#ifdef _WIN32
    Require(AssetPaths::IsSameOrInside("C:/Assets/tree/a.glb", "c:/assets/Tree"), "comparison is case-sensitive");
#endif
}

void RebaseRenamedFile()
{
    const std::optional<std::filesystem::path> rebased =
        AssetPaths::Rebase("C:/assets/Tree/tree.glb", "C:/assets/Tree/tree.glb", "C:/assets/Tree/oak.glb");
    Require(rebased.has_value(), "the renamed file itself was not rebased");
    Require(*rebased == std::filesystem::path("C:/assets/Tree/oak.glb"), "the renamed file got the wrong path");
}

void RebaseFileInsideRenamedFolder()
{
    const std::optional<std::filesystem::path> rebased =
        AssetPaths::Rebase("C:\\assets\\Tree\\textures\\bark.png", "C:/assets/Tree", "C:/assets/Oak");
    Require(rebased.has_value(), "a file inside the renamed folder was not rebased");
    Require(
        *rebased == std::filesystem::path("C:/assets/Oak/textures/bark.png").lexically_normal(),
        "a file inside the renamed folder got the wrong path");
}

void RebaseLeavesUnrelatedPaths()
{
    Require(
        !AssetPaths::Rebase("C:/assets/Trees/a.glb", "C:/assets/Tree", "C:/assets/Oak").has_value(),
        "a sibling sharing a name prefix was rebased");
    Require(!AssetPaths::Rebase("", "C:/assets/Tree", "C:/assets/Oak").has_value(), "an empty path was rebased");
}

void UniqueCopyPathSkipsTakenNames()
{
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "miniengine_asset_paths_tests";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / "bark.png") << "x";
    std::ofstream(dir / "bark_copy.png") << "x";

    const std::filesystem::path copy = AssetPaths::UniqueCopyPath(dir / "bark.png");
    std::filesystem::remove_all(dir, ec);
    Require(copy == dir / "bark_copy2.png", "the copy did not take the first free name");
}
}

int main()
{
    try
    {
        SameOrInside();
        RebaseRenamedFile();
        RebaseFileInsideRenamedFolder();
        RebaseLeavesUnrelatedPaths();
        UniqueCopyPathSkipsTakenNames();

        std::cout << "asset paths tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "asset paths tests failed: " << error.what() << '\n';
        return 1;
    }
}
