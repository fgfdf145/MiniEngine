#include "tyre_tir_file.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace me::tyre
{

namespace
{
using Field = double MagicFormulaParameters::*;
using ScalingField = double MagicFormulaParameters::Scaling::*;

const std::unordered_map<std::string, Field>& Fields()
{
    using P = MagicFormulaParameters;
    static const std::unordered_map<std::string, Field> fields = {
        {"UNLOADED_RADIUS", &P::unloadedRadius},
        {"FNOMIN", &P::nominalLoad},
        {"LONGVL", &P::referenceVelocity},
        {"PCX1", &P::pCx1},
        {"PDX1", &P::pDx1},
        {"PDX2", &P::pDx2},
        {"PEX1", &P::pEx1},
        {"PEX2", &P::pEx2},
        {"PEX3", &P::pEx3},
        {"PEX4", &P::pEx4},
        {"PKX1", &P::pKx1},
        {"PKX2", &P::pKx2},
        {"PKX3", &P::pKx3},
        {"PHX1", &P::pHx1},
        {"PHX2", &P::pHx2},
        {"PVX1", &P::pVx1},
        {"PVX2", &P::pVx2},
        {"RBX1", &P::rBx1},
        {"RBX2", &P::rBx2},
        {"RBX3", &P::rBx3},
        {"RCX1", &P::rCx1},
        {"REX1", &P::rEx1},
        {"REX2", &P::rEx2},
        {"RHX1", &P::rHx1},
        {"PCY1", &P::pCy1},
        {"PDY1", &P::pDy1},
        {"PDY2", &P::pDy2},
        {"PDY3", &P::pDy3},
        {"PEY1", &P::pEy1},
        {"PEY2", &P::pEy2},
        {"PEY3", &P::pEy3},
        {"PEY4", &P::pEy4},
        {"PEY5", &P::pEy5},
        {"PKY1", &P::pKy1},
        {"PKY2", &P::pKy2},
        {"PKY3", &P::pKy3},
        {"PKY4", &P::pKy4},
        {"PKY5", &P::pKy5},
        {"PKY6", &P::pKy6},
        {"PKY7", &P::pKy7},
        {"PHY1", &P::pHy1},
        {"PHY2", &P::pHy2},
        {"PVY1", &P::pVy1},
        {"PVY2", &P::pVy2},
        {"PVY3", &P::pVy3},
        {"PVY4", &P::pVy4},
        {"RBY1", &P::rBy1},
        {"RBY2", &P::rBy2},
        {"RBY3", &P::rBy3},
        {"RBY4", &P::rBy4},
        {"RCY1", &P::rCy1},
        {"REY1", &P::rEy1},
        {"REY2", &P::rEy2},
        {"RHY1", &P::rHy1},
        {"RHY2", &P::rHy2},
        {"RVY1", &P::rVy1},
        {"RVY2", &P::rVy2},
        {"RVY3", &P::rVy3},
        {"RVY4", &P::rVy4},
        {"RVY5", &P::rVy5},
        {"RVY6", &P::rVy6},
        {"QBZ1", &P::qBz1},
        {"QBZ2", &P::qBz2},
        {"QBZ3", &P::qBz3},
        {"QBZ5", &P::qBz5},
        {"QBZ6", &P::qBz6},
        {"QBZ9", &P::qBz9},
        {"QBZ10", &P::qBz10},
        {"QCZ1", &P::qCz1},
        {"QDZ1", &P::qDz1},
        {"QDZ2", &P::qDz2},
        {"QDZ3", &P::qDz3},
        {"QDZ4", &P::qDz4},
        {"QDZ6", &P::qDz6},
        {"QDZ7", &P::qDz7},
        {"QDZ8", &P::qDz8},
        {"QDZ9", &P::qDz9},
        {"QDZ10", &P::qDz10},
        {"QDZ11", &P::qDz11},
        {"QEZ1", &P::qEz1},
        {"QEZ2", &P::qEz2},
        {"QEZ3", &P::qEz3},
        {"QEZ4", &P::qEz4},
        {"QEZ5", &P::qEz5},
        {"QHZ1", &P::qHz1},
        {"QHZ2", &P::qHz2},
        {"QHZ3", &P::qHz3},
        {"QHZ4", &P::qHz4},
        {"SSZ1", &P::sSz1},
        {"SSZ2", &P::sSz2},
        {"SSZ3", &P::sSz3},
        {"SSZ4", &P::sSz4},
        {"QSX1", &P::qSx1},
        {"QSX2", &P::qSx2},
        {"QSX3", &P::qSx3},
        {"QSY1", &P::qSy1},
        {"QSY2", &P::qSy2},
    };
    return fields;
}

// MF 5.2 files name the camber stiffness factors LGAY and LGAZ, MF 6.x files LKYC and LKZC.
const std::unordered_map<std::string, ScalingField>& ScalingFields()
{
    using S = MagicFormulaParameters::Scaling;
    static const std::unordered_map<std::string, ScalingField> fields = {
        {"LFZO", &S::Fz0},
        {"LCX", &S::Cx},
        {"LMUX", &S::muX},
        {"LEX", &S::Ex},
        {"LKX", &S::Kx},
        {"LHX", &S::Hx},
        {"LVX", &S::Vx},
        {"LCY", &S::Cy},
        {"LMUY", &S::muY},
        {"LEY", &S::Ey},
        {"LKY", &S::Ky},
        {"LKYC", &S::Kygamma},
        {"LGAY", &S::Kygamma},
        {"LKZC", &S::Kzgamma},
        {"LGAZ", &S::Kzgamma},
        {"LHY", &S::Hy},
        {"LVY", &S::Vy},
        {"LTR", &S::trail},
        {"LRES", &S::residualTorque},
        {"LXAL", &S::xAlpha},
        {"LYKA", &S::yKappa},
        {"LVYKA", &S::VyKappa},
        {"LS", &S::s},
        {"LMX", &S::Mx},
        {"LMY", &S::My},
        {"LMUV", &S::muV},
    };
    return fields;
}

std::string Trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

bool ParseNumber(const std::string& text, double& value)
{
    const char* begin = text.data();
    const char* end = begin + text.size();
    if (begin != end && *begin == '+')
    {
        ++begin;
    }
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc() && result.ptr == end;
}
}

TirReadResult ReadTir(std::istream& in)
{
    TirReadResult result;
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line))
    {
        ++lineNumber;
        const auto comment = line.find_first_of("$!");
        if (comment != std::string::npos)
        {
            line.resize(comment);
        }
        line = Trim(line);
        if (line.empty() || line.front() == '[')
        {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }
        std::string key = Trim(line.substr(0, equals));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
                       {
                           return static_cast<char>(std::toupper(c));
                       });
        const std::string text = Trim(line.substr(equals + 1));
        const auto field = Fields().find(key);
        const auto scaling = ScalingFields().find(key);
        if (field == Fields().end() && scaling == ScalingFields().end())
        {
            result.ignoredKeys.push_back(key);
            continue;
        }
        double value = 0.0;
        if (!ParseNumber(text, value))
        {
            throw std::runtime_error(".tir line " + std::to_string(lineNumber) + ": " + key + " is not a number: '" + text + "'");
        }
        if (field != Fields().end())
        {
            result.parameters.*(field->second) = value;
        }
        else
        {
            result.parameters.lambda.*(scaling->second) = value;
        }
    }

    const MagicFormulaParameters& p = result.parameters;
    if (!(p.nominalLoad > 0.0) || !(p.unloadedRadius > 0.0) || !(p.referenceVelocity > 0.0))
    {
        throw std::runtime_error(".tir needs positive FNOMIN, UNLOADED_RADIUS and LONGVL");
    }
    return result;
}

TirReadResult ReadTirFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file)
    {
        throw std::runtime_error("cannot open " + path.string());
    }
    return ReadTir(file);
}

}
