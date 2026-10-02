// Runs a car's suspension through the virtual K&C rig and seven-post rig and writes what they
// measured: CSV files for plotting and summary.md (engine/suspension/suspension_rig_report.h; the
// editor's Suspension Rigs window runs the same tests).
//
//   miniengine_suspension_rig_report <Assetto Corsa car folder> <output folder>
//
// The car is read from its data.acd (or data/ folder) as the kn5 import reads it.

#include <engine/asset/ac_car_data.h>
#include <engine/physics/vehicle_suspension.h>
#include <engine/suspension/suspension_rig_report.h>

#include <filesystem>
#include <iostream>
#include <string>

using namespace me;
using namespace me::suspension;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: miniengine_suspension_rig_report <Assetto Corsa car folder> <output folder>\n";
        return 2;
    }
    try
    {
        const std::filesystem::path carFolder = argv[1];
        const std::filesystem::path out = argv[2];
        std::string problem;
        const std::optional<VehicleCarSpec> spec = AcCarData::ReadCarFolder(carFolder, &problem);
        if (!spec.has_value())
        {
            throw std::runtime_error("cannot read the car's data: " + problem);
        }
        const CarModel car = BuildCarModel(*spec, carFolder.filename().string());
        const std::optional<RigReport> report = RunRigReport(car);
        WriteRigReport(*report, out);
        std::cout << "wrote " << out.string() << " in " << report->seconds << " s\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "suspension rig report failed: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
