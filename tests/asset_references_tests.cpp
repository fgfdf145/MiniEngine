#include <engine/asset/asset_references.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

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

class ScopedTree
{
  public:
    explicit ScopedTree(const char* name)
    {
        std::error_code ec;
        const std::filesystem::path base =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        m_root = (ec ? std::filesystem::temp_directory_path() : base) /
                 "miniengine_asset_references_tests" / name;
        std::filesystem::remove_all(m_root, ec);
        std::filesystem::create_directories(m_root, ec);
    }

    ~ScopedTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    ScopedTree(const ScopedTree&) = delete;
    ScopedTree& operator=(const ScopedTree&) = delete;

    const std::filesystem::path& Root() const
    {
        return m_root;
    }

    void Write(const std::string& relativePath, const std::string& contents) const
    {
        const std::filesystem::path full = m_root / relativePath;
        std::error_code ec;
        std::filesystem::create_directories(full.parent_path(), ec);
        std::ofstream out(full, std::ios::binary | std::ios::trunc);
        out << contents;
    }

  private:
    std::filesystem::path m_root;
};

void FindsAMention()
{
    ScopedTree tree("mention");
    tree.Write("mat_0.material.yaml", "material:\n  base_color_texture_path: textures/tex.png\n");
    tree.Write("unrelated.material.yaml", "material:\n  base_color_texture_path: textures/other.png\n");

    const std::vector<AssetReference> found = FindReferencesTo(tree.Root(), {"tex.png"}, {});
    Require(found.size() == 1, "expected exactly one reference to tex.png");
    Require(found[0].referencedName == "tex.png", "reported the wrong referenced name");
    Require(found[0].referencedBy == "mat_0.material.yaml", "reported the wrong referencing document");
}

void SkipsSidecarsAndExclusions()
{
    ScopedTree tree("skips");
    // A uuid sidecar names its own asset by design; that is not a reference.
    tree.Write("tex.png.miniengine_asset.yaml", "asset:\n  uuid: x\n  file: tex.png\n");
    tree.Write("excluded.material.yaml", "material:\n  base_color_texture_path: tex.png\n");

    const std::unordered_set<std::string> excluded{
        (tree.Root() / "excluded.material.yaml").lexically_normal().string()};

    const std::vector<AssetReference> found = FindReferencesTo(tree.Root(), {"tex.png"}, excluded);
    Require(found.empty(), "a uuid sidecar or an excluded path was reported as a reference");
}

// A document whose last_write_time has not moved must be answered from the
// index rather than re-read. Proven by changing the content while restoring
// the timestamp: the stale answer coming back is the evidence.
void IndexIsReused()
{
    ScopedTree tree("index_reuse");
    tree.Write("doc.material.yaml", "material:\n  base_color_texture_path: tex.png\n");

    const std::filesystem::path doc = tree.Root() / "doc.material.yaml";
    std::error_code ec;
    const std::filesystem::file_time_type original = std::filesystem::last_write_time(doc, ec);
    Require(!ec, "could not read the document's last_write_time");

    Require(FindReferencesTo(tree.Root(), {"tex.png"}, {}).size() == 1, "first scan missed the reference");

    // Content no longer mentions tex.png, but the timestamp says unchanged.
    tree.Write("doc.material.yaml", "material:\n  base_color_texture_path: nothing.png\n");
    std::filesystem::last_write_time(doc, original, ec);
    Require(!ec, "could not restore the document's last_write_time");

    Require(
        FindReferencesTo(tree.Root(), {"tex.png"}, {}).size() == 1,
        "the index was not reused: the document was re-read despite an unchanged timestamp");
}

void IndexIsInvalidated()
{
    ScopedTree tree("index_invalidate");
    tree.Write("doc.material.yaml", "material:\n  base_color_texture_path: tex.png\n");
    Require(FindReferencesTo(tree.Root(), {"tex.png"}, {}).size() == 1, "first scan missed the reference");

    // Rewrite and let the timestamp advance normally.
    tree.Write("doc.material.yaml", "material:\n  base_color_texture_path: nothing.png\n");
    std::error_code ec;
    std::filesystem::last_write_time(
        tree.Root() / "doc.material.yaml",
        std::filesystem::file_time_type::clock::now(),
        ec);

    Require(
        FindReferencesTo(tree.Root(), {"tex.png"}, {}).empty(),
        "the index was not invalidated by a changed timestamp");
}
}

int main()
{
    try
    {
        FindsAMention();
        SkipsSidecarsAndExclusions();
        IndexIsReused();
        IndexIsInvalidated();

        std::cout << "asset references tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "asset references tests failed: " << error.what() << '\n';
        return 1;
    }
}
