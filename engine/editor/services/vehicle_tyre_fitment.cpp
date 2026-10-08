#include "vehicle_tyre_fitment.h"

#include <engine/asset/model_cache.h>
#include <engine/asset/tyre_library.h>

#include <algorithm>

namespace me::VehicleTyreFitment
{

namespace
{
VehicleCarSpec WriteAndCache(const std::filesystem::path& gltfPath, VehicleCarSpec car)
{
    TyreLibrary::WriteCarTyres(gltfPath, car);
    TyreLibrary::ResolveWheelTyres(car);
    ModelCache::UpdateCarSpec(gltfPath.string(), car);
    return car;
}
}

void Fit(Fitment& fitment, const VehicleCarSpec& car, size_t wheel, const VehicleTyreRef& tyre, bool pairAxles)
{
    if (wheel >= kVehicleWheelCount)
    {
        return;
    }
    for (const size_t target : {wheel, pairAxles ? (wheel ^ 1u) : wheel})
    {
        fitment[target] = tyre == car.wheelTyreRefs[target] ? VehicleTyreRef{} : tyre;
    }
}

bool Any(const Fitment& fitment)
{
    return std::any_of(fitment.begin(), fitment.end(), [](const VehicleTyreRef& ref) { return !ref.Empty(); });
}

VehicleCarSpec Save(const std::filesystem::path& gltfPath, const VehicleCarSpec& car, const Fitment& fitment)
{
    VehicleCarSpec saved = car;
    for (size_t wheel = 0; wheel < kVehicleWheelCount; ++wheel)
    {
        if (!fitment[wheel].Empty())
        {
            saved.wheelTyreRefs[wheel] = fitment[wheel];
        }
    }
    return WriteAndCache(gltfPath, std::move(saved));
}

VehicleCarSpec Adopt(const std::filesystem::path& gltfPath, const VehicleCarSpec& car, const std::string& carFolder)
{
    VehicleCarSpec adopted = car;
    TyreLibrary::AdoptCarTyres(adopted, carFolder);
    return WriteAndCache(gltfPath, std::move(adopted));
}
}
