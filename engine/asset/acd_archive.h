#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace me
{

// Assetto Corsa's data.acd: the archive a car's physics files (car.ini, engine.ini, power.lut,
// suspensions.ini, ...) ship in. Each byte is stored as a 32-bit word with a key added, and the key
// is a string of eight numbers joined by dashes that the game derives from the car's folder name.
//
// The derivation is community reverse-engineered and not fully reproduced here: seven of the eight
// numbers are computed from the name (checked against every car of the base game), the seventh is
// found by trying all 256 values and keeping the one that decrypts the archive's text files to text.
// A renamed folder therefore fails, as it does in the game.
namespace AcdArchive
{
// The files by lower-case name, decrypted.
using Files = std::map<std::string, std::string>;

// The key for a folder name and the archive's seventh number: "6-105-61-232-126-93-15-108".
std::string MakeKey(const std::string& folderName, int seventh);

// Splits an archive into its files, each still encrypted, without looking at the key. Throws
// std::runtime_error, naming `source`, for anything that is not an archive.
struct RawEntry
{
    std::string name;
    std::vector<std::uint8_t> bytes;
};
std::vector<RawEntry> Split(const std::vector<std::uint8_t>& archive, const std::string& source);

// Decrypts an archive that came from a folder called `folderName` (the exact spelling first, then
// lower case). Throws std::runtime_error when the archive is unreadable or no key makes its text
// files text.
Files Parse(const std::vector<std::uint8_t>& archive, const std::string& folderName, const std::string& source);

// Reads a data.acd from disk; the folder name is its parent folder's.
Files Load(const std::filesystem::path& archivePath);
}
}
