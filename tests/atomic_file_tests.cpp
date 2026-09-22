#include <engine/core/file/atomic_file.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
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

class ScopedDir
{
  public:
    explicit ScopedDir(const char* name)
    {
        std::error_code ec;
        const std::filesystem::path base =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        m_path = (ec ? std::filesystem::temp_directory_path() : base) / "miniengine_atomic_file_tests" / name;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
    }

    ~ScopedDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

void CreatesNewFile()
{
    ScopedDir scope("create");
    const std::filesystem::path path = scope.Path() / "scene.yaml";

    Require(AtomicFile::Write(path, "a: 1\n"), "write to a new file failed");
    Require(ReadAll(path) == "a: 1\n", "new file has the wrong content");
    Require(!Exists(scope.Path() / "scene.yaml.tmp"), "temporary left behind after a successful write");
}

void ReplacesExistingFile()
{
    ScopedDir scope("replace");
    const std::filesystem::path path = scope.Path() / "scene.yaml";
    Require(AtomicFile::Write(path, "old content that is longer\n"), "first write failed");

    Require(AtomicFile::Write(path, "new\n"), "overwrite failed");
    Require(ReadAll(path) == "new\n", "overwrite kept stale bytes");
}

// The property the helper exists for: a write that cannot finish leaves what
// was already there.
void FailureKeepsOldFile()
{
    ScopedDir scope("failure");

    // A directory where the file should go: the final rename cannot replace it.
    const std::filesystem::path blocked = scope.Path() / "blocked.yaml";
    std::error_code ec;
    std::filesystem::create_directories(blocked / "child", ec);

    std::string error;
    Require(!AtomicFile::Write(blocked, "data", &error), "write over a directory reported success");
    Require(!error.empty(), "failure did not report an error message");
    Require(std::filesystem::is_directory(blocked / "child", ec), "failed write damaged the existing entry");
    Require(!Exists(scope.Path() / "blocked.yaml.tmp"), "temporary left behind after a failed write");

    // A parent directory that does not exist: nothing can be opened.
    Require(
        !AtomicFile::Write(scope.Path() / "missing" / "scene.yaml", "data"),
        "write into a missing directory reported success");
}
}

int main()
{
    try
    {
        CreatesNewFile();
        ReplacesExistingFile();
        FailureKeepsOldFile();

        std::cout << "atomic file tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "atomic file tests failed: " << error.what() << '\n';
        return 1;
    }
}
