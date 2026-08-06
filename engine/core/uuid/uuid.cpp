#include "uuid.h"

#include <chrono>
#include <cstdio>
#include <random>

namespace Uuid
{
std::string GenerateV4()
{
    thread_local std::mt19937_64 rng = []
    {
        std::random_device device;
        std::seed_seq seed{
            device(), device(), device(), device(),
            static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())};
        return std::mt19937_64(seed);
    }();

    uint64_t hi = rng();
    uint64_t lo = rng();
    hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

    char buffer[37];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%08llx-%04llx-%04llx-%04llx-%012llx",
        static_cast<unsigned long long>(hi >> 32),
        static_cast<unsigned long long>((hi >> 16) & 0xFFFFull),
        static_cast<unsigned long long>(hi & 0xFFFFull),
        static_cast<unsigned long long>(lo >> 48),
        static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
    return std::string(buffer);
}
}
