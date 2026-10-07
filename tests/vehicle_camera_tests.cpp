#include <engine/asset/ac_car_data.h>
#include <engine/editor/services/vehicle_drive_service.h>
#include <engine/renderer/camera.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

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

void RequireNear(float actual, float expected, float tolerance, const std::string& message)
{
    Require(std::abs(actual - expected) <= tolerance, message + " (" + std::to_string(actual) + ", expected " + std::to_string(expected) + ")");
}

glm::vec3 ViewSpace(const Camera& camera, const glm::vec3& point)
{
    return glm::vec3(camera.GetViewMatrix() * glm::vec4(point, 1.0f));
}

// A car heading off to the side and rolled onto its right, as in a hard corner.
PhysicsPose LeaningCar()
{
    PhysicsPose pose;
    pose.position = glm::vec3(10.0f, 2.0f, -5.0f);
    pose.rotation = glm::angleAxis(glm::radians(70.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                    glm::angleAxis(glm::radians(20.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    return pose;
}

// A point on the body (vehicle space: +Z forward, +X the car's left) in the world.
glm::vec3 OnBody(const PhysicsPose& pose, const glm::vec3& point)
{
    return glm::vec3(pose.position + glm::dvec3(pose.rotation * point));
}

// The cockpit camera sits on the body and turns with it: what is straight ahead of the eyes is in the
// middle of the view, the body's up is up on screen even with the car rolled, and its left is left.
void MountedCameraTurnsWithTheBody()
{
    const PhysicsPose pose = LeaningCar();
    VehicleCameraMount eyes;
    eyes.position = glm::vec3(-0.37f, 1.1f, -0.16f);
    Camera camera;
    VehicleDriveService::UpdateMountedCamera(camera, pose, eyes);

    Require(glm::length(camera.position - OnBody(pose, eyes.position)) < 1e-4f, "the camera is at the eyes");
    const glm::vec3 ahead = ViewSpace(camera, OnBody(pose, eyes.position + glm::vec3(0.0f, 0.0f, 10.0f)));
    Require(std::abs(ahead.x) < 1e-3f && std::abs(ahead.y) < 1e-3f && ahead.z < 0.0f, "straight ahead is in the middle of the view");
    const glm::vec3 above = ViewSpace(camera, OnBody(pose, eyes.position + glm::vec3(0.0f, 1.0f, 10.0f)));
    Require(std::abs(above.x) < 1e-3f && above.y > 0.5f, "the body's up is up on screen: the view rolls with the car");
    const glm::vec3 left = ViewSpace(camera, OnBody(pose, eyes.position + glm::vec3(1.0f, 0.0f, 10.0f)));
    Require(left.x < -0.5f && std::abs(left.y) < 1e-3f, "the car's left is on the left");
}

// The mount's pitch tips the view down; the head turns from it, a positive yaw looking left.
void MountedCameraPitchesAndTheHeadTurns()
{
    PhysicsPose pose;
    VehicleCameraMount bonnet;
    bonnet.position = glm::vec3(0.0f, 1.0f, 1.0f);
    bonnet.pitchDegrees = -10.0f;
    Camera camera;
    VehicleDriveService::UpdateMountedCamera(camera, pose, bonnet);
    const glm::vec3 level = ViewSpace(camera, OnBody(pose, bonnet.position + glm::vec3(0.0f, 0.0f, 10.0f)));
    Require(level.y > 0.5f, "looking down, the level horizon is above the middle");
    RequireNear(camera.pitchDegrees, -10.0f, 1e-3f, "the camera is pitched by the mount");

    VehicleCameraOrbit look;
    look.yawDegrees = 90.0f;
    bonnet.pitchDegrees = 0.0f;
    VehicleDriveService::UpdateMountedCamera(camera, pose, bonnet, look);
    const glm::vec3 side = ViewSpace(camera, OnBody(pose, bonnet.position + glm::vec3(10.0f, 0.0f, 0.0f)));
    Require(std::abs(side.x) < 1e-3f && std::abs(side.y) < 1e-3f && side.z < 0.0f, "turning the head 90 degrees looks out of the left side");

    look = VehicleCameraOrbit{};
    look.pitchDegrees = 30.0f;
    VehicleDriveService::UpdateMountedCamera(camera, pose, bonnet, look);
    RequireNear(camera.pitchDegrees, -30.0f, 1e-3f, "a positive look pitch looks down");
}

// The eyes come from the car's data when it has them, else from the steering wheel, else the box; the
// bonnet camera sits over the body towards the windscreen, the bumper camera low ahead of the nose.
void CameraMountsComeFromWhatIsKnown()
{
    const glm::vec3 boundsMin(-0.9f, 0.0f, -2.3f);
    const glm::vec3 boundsMax(0.9f, 1.34f, 2.3f);
    const auto hood = [](float z) -> std::optional<float>
    {
        return z > 0.6f ? std::optional<float>(0.92f) : std::optional<float>(1.3f);
    };

    VehicleCameraMount data;
    data.position = glm::vec3(-0.38f, 1.11f, -0.16f);
    data.pitchDegrees = -2.8f;
    auto mounts = VehicleDriveService::ComputeCameraMounts(boundsMin, boundsMax, data, glm::vec3(1.0f), hood);
    Require(mounts[0].position == data.position && mounts[0].pitchDegrees == data.pitchDegrees, "the car's data wins over the steering wheel");

    const glm::vec3 wheel(-0.378f, 0.816f, 0.428f);
    mounts = VehicleDriveService::ComputeCameraMounts(boundsMin, boundsMax, std::nullopt, wheel, hood);
    Require(glm::length(mounts[0].position - glm::vec3(-0.378f, 1.106f, -0.162f)) < 1e-3f, "the R34's eyes from its STEER_HR");
    Require(mounts[0].pitchDegrees < 0.0f, "looking a little down");

    const VehicleCameraMount& bonnet = mounts[1];
    Require(bonnet.position.x == 0.0f, "the bonnet camera is on the centre line");
    RequireNear(bonnet.position.z, -0.162f + (2.3f + 0.162f) * 0.45f, 1e-3f, "towards the windscreen");
    RequireNear(bonnet.position.y, 0.92f + 0.12f, 1e-3f, "a little over the bonnet");

    const VehicleCameraMount& bumper = mounts[2];
    Require(bumper.position.z > boundsMax.z && bumper.position.y < 0.6f && bumper.position.y >= 0.3f, "the bumper camera is low, ahead of the nose");

    mounts = VehicleDriveService::ComputeCameraMounts(boundsMin, boundsMax, std::nullopt, std::nullopt, nullptr);
    Require(mounts[0].position.x == 0.0f && mounts[0].position.y > 1.0f && mounts[0].position.y < 1.34f && mounts[0].position.z < 0.0f,
            "without either: on the centre line, high in the box, behind the middle");
    Require(mounts[1].position.y <= mounts[0].position.y, "the bonnet camera is never over the eyes");
}

// Gran Turismo 7's order, round and round.
void ViewsCycle()
{
    VehicleCameraView view = VehicleCameraView::Chase;
    const VehicleCameraView expected[] = {VehicleCameraView::Cockpit, VehicleCameraView::Bonnet, VehicleCameraView::Bumper, VehicleCameraView::Chase};
    for (const VehicleCameraView next : expected)
    {
        view = NextVehicleCameraView(view);
        Require(view == next, std::string("the view after goes to ") + VehicleCameraViewName(next));
    }
}

// Let go of, the head comes back to the road at its rate; at 0 it stays turned.
void TheHeadComesBack()
{
    VehicleCameraOrbit look;
    VehicleDriveService::UpdateCameraOrbit(look, true, 80.0f, 20.0f, 12.0f, 1.0f / 60.0f);
    Require(look.yawDegrees == 80.0f && look.pitchDegrees == 20.0f, "held, it turns");
    VehicleDriveService::UpdateCameraOrbit(look, false, 0.0f, 0.0f, 0.0f, 1.0f);
    Require(look.yawDegrees == 80.0f, "at 0 it stays");
    VehicleDriveService::UpdateCameraOrbit(look, false, 0.0f, 0.0f, 12.0f, 0.5f);
    Require(std::abs(look.yawDegrees) < 0.5f && std::abs(look.pitchDegrees) < 0.5f, "half a second at 12 brings it back");
}

// DRIVEREYES and ON_BOARD_PITCH_ANGLE, turned as the import turns the kn5 (half round about Y).
void DriverEyesComeFromCarIni()
{
    const VehicleCarSpec spec = AcCarData::BuildSpec(
        {{"car.ini", "[GRAPHICS]\r\nDRIVEREYES=-0.377668,1.10719,-0.162679\r\nON_BOARD_PITCH_ANGLE=-2.774010\r\nBONNET_CAMERA_POS=0,0.69,0.33\r\n"}});
    Require(spec.cockpitCamera.has_value(), "the eyes are read");
    Require(glm::length(spec.cockpitCamera->position - glm::vec3(0.377668f, 1.10719f, 0.162679f)) < 1e-5f, "in the model's frame");
    RequireNear(spec.cockpitCamera->pitchDegrees, -2.77401f, 1e-4f, "with their pitch");
    Require(!AcCarData::BuildSpec({{"car.ini", "[BASIC]\r\nTOTALMASS=900\r\n"}}).cockpitCamera.has_value(), "no DRIVEREYES, no eyes");
}
}

int main()
{
    try
    {
        MountedCameraTurnsWithTheBody();
        MountedCameraPitchesAndTheHeadTurns();
        CameraMountsComeFromWhatIsKnown();
        ViewsCycle();
        TheHeadComesBack();
        DriverEyesComeFromCarIni();
    }
    catch (const std::exception& error)
    {
        std::cerr << "vehicle camera tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "vehicle camera tests passed\n";
    return 0;
}
