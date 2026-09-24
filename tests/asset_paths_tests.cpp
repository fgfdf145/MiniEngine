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
    // Spelled differently from the folder; backslashes only separate on Windows.
#ifdef _WIN32
    const char* file = "C:\\assets\\Tree\\textures\\bark.png";
#else
    const char* file = "C:/assets/Tree/./textures//bark.png";
#endif
    const std::optional<std::filesystem::path> rebased = AssetPaths::Rebase(file, "C:/assets/Tree", "C:/assets/Oak");
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

void RenameEditsOnlyTheName()
{
    const AssetPaths::RenameableName model = AssetPaths::SplitRenameableName("tree.glb", false);
    Require(model.editable == "tree" && model.suffix == ".glb", "a model's extension is editable");

    const AssetPaths::RenameableName material = AssetPaths::SplitRenameableName("tree_0.material.yaml", false);
    Require(
        material.editable == "tree_0" && material.suffix == ".material.yaml",
        "a material definition's compound suffix is editable");

    const AssetPaths::RenameableName dotted = AssetPaths::SplitRenameableName("oak.v2.png", false);
    Require(dotted.editable == "oak.v2" && dotted.suffix == ".png", "only the last extension is kept");

    const AssetPaths::RenameableName folder = AssetPaths::SplitRenameableName("my.textures", true);
    Require(folder.editable == "my.textures" && folder.suffix.empty(), "a folder name lost its dotted part");

    const AssetPaths::RenameableName hidden = AssetPaths::SplitRenameableName(".gitignore", false);
    Require(hidden.editable == ".gitignore" && hidden.suffix.empty(), "a dot file was split into an empty name");
}

void CaseOnlyRenameIsNotAClash()
{
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "miniengine_asset_paths_clobber";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / "Tree.glb") << "x";
    std::ofstream(dir / "Oak.glb") << "x";

    const bool free = AssetPaths::RenameWouldClobber(dir / "Tree.glb", dir / "Pine.glb");
    const bool other = AssetPaths::RenameWouldClobber(dir / "Tree.glb", dir / "Oak.glb");
    const bool caseOnly = AssetPaths::RenameWouldClobber(dir / "Tree.glb", dir / "tree.glb");
    std::filesystem::remove_all(dir, ec);

    Require(!free, "a free name counted as a clash");
    Require(other, "renaming onto another file was allowed");
    Require(!caseOnly, "a case-only rename was refused");
}

void UniqueCopyPathSkipsTakenNames()
{
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "miniengine_asset_paths_tests";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / "bark.png") << "x";
    std::ofstream(dir / "bark_copy.png") << "x";

    std::ofstream(dir / "tree_0.material.yaml") << "x";
    std::filesystem::create_directories(dir / "my.textures", ec);

    const std::filesystem::path copy = AssetPaths::UniqueCopyPath(dir / "bark.png");
    const std::filesystem::path materialCopy = AssetPaths::UniqueCopyPath(dir / "tree_0.material.yaml");
    const std::filesystem::path folderCopy = AssetPaths::UniqueCopyPath(dir / "my.textures");
    std::filesystem::remove_all(dir, ec);
    Require(copy == dir / "bark_copy2.png", "the copy did not take the first free name");
    Require(materialCopy == dir / "tree_0_copy.material.yaml", "a material definition copy broke its suffix");
    Require(folderCopy == dir / "my.textures_copy", "a dotted folder copy was split like a file");
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
        RenameEditsOnlyTheName();
        CaseOnlyRenameIsNotAClash();
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
