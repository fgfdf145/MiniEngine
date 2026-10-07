#include <engine/core/video/video_mosaic.h>
#include <engine/editor/engine_settings.h>
#include <engine/editor/services/quad_recording.h>

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

bool Near(float a, float b, float tolerance = 1e-4f)
{
    return std::abs(a - b) <= tolerance;
}

bool Near(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-4f)
{
    return glm::length(a - b) <= tolerance;
}

std::string Describe(const glm::vec3& value)
{
    return "(" + std::to_string(value.x) + ", " + std::to_string(value.y) + ", " + std::to_string(value.z) + ")";
}

// Four equal pictures make a 2 x 2 canvas twice their size, front and rear on top.
void TestGridOfEqualPictures()
{
    const std::array<VideoMosaicSize, 4> sizes = {{{960, 540}, {960, 540}, {960, 540}, {960, 540}}};
    const VideoMosaic mosaic = ComputeVideoMosaic(VideoMosaicLayout::Grid, sizes);
    Require(mosaic.width == 1920 && mosaic.height == 1080, "four 960 x 540 pictures make 1920 x 1080");
    Require(mosaic.tiles.size() == 4, "a tile per picture");
    Require(mosaic.tiles[0] == VideoMosaicTile{0, 0, 960, 540}, "front at the top left");
    Require(mosaic.tiles[1] == VideoMosaicTile{960, 0, 960, 540}, "rear at the top right");
    Require(mosaic.tiles[2] == VideoMosaicTile{0, 540, 960, 540}, "left at the bottom left");
    Require(mosaic.tiles[3] == VideoMosaicTile{960, 540, 960, 540}, "right at the bottom right");
}

// Pictures of different sizes: columns as wide as their widest, rows as tall as their tallest, each
// picture centred in its cell, and the canvas's sides even.
void TestGridOfMixedPictures()
{
    const std::array<VideoMosaicSize, 4> sizes = {{{1280, 720}, {640, 360}, {801, 451}, {640, 480}}};
    const VideoMosaic mosaic = ComputeVideoMosaic(VideoMosaicLayout::Grid, sizes);
    // Columns 1280 and 640, rows 720 and 480: 1920 x 1200.
    Require(mosaic.width == 1920 && mosaic.height == 1200, "the canvas is the columns' and rows' sum");
    Require(mosaic.tiles[0] == VideoMosaicTile{0, 0, 1280, 720}, "the largest picture fills its cell");
    Require(mosaic.tiles[1] == VideoMosaicTile{1280, 180, 640, 360}, "a short picture centred down its row");
    Require(mosaic.tiles[2] == VideoMosaicTile{239, 734, 801, 451}, "a narrow picture centred across its column");
    Require(mosaic.tiles[3] == VideoMosaicTile{1280, 720, 640, 480}, "the right picture in the last cell");

    const std::array<VideoMosaicSize, 4> odd = {{{101, 51}, {101, 51}, {101, 51}, {101, 51}}};
    const VideoMosaic even = ComputeVideoMosaic(VideoMosaicLayout::Grid, odd);
    Require(even.width == 202 && even.height == 102, "an even canvas whatever the pictures");
    const std::array<VideoMosaicSize, 4> oddSum = {{{101, 51}, {100, 50}, {100, 50}, {100, 50}}};
    const VideoMosaic rounded = ComputeVideoMosaic(VideoMosaicLayout::Grid, oddSum);
    Require(rounded.width == 202 && rounded.height == 102, "an odd sum rounded up to even");
}

// The cross: front on top, left and right either side of the middle, rear below, the centre empty.
void TestCross()
{
    const std::array<VideoMosaicSize, 4> sizes = {{{640, 360}, {640, 360}, {480, 360}, {480, 360}}};
    const VideoMosaic mosaic = ComputeVideoMosaic(VideoMosaicLayout::Cross, sizes);
    Require(mosaic.width == 480 + 640 + 480 && mosaic.height == 3 * 360, "three columns and three rows");
    Require(mosaic.tiles[0] == VideoMosaicTile{480, 0, 640, 360}, "front at the top middle");
    Require(mosaic.tiles[1] == VideoMosaicTile{480, 720, 640, 360}, "rear at the bottom middle");
    Require(mosaic.tiles[2] == VideoMosaicTile{0, 360, 480, 360}, "left on the left of the middle row");
    Require(mosaic.tiles[3] == VideoMosaicTile{1120, 360, 480, 360}, "right on the right of the middle row");
}

// A label is white glyphs on a black box at its place, and touches nothing else.
void TestLabel()
{
    constexpr uint32_t width = 64;
    constexpr uint32_t height = 32;
    std::vector<uint8_t> pixels(width * height * 4, 77);
    const VideoMosaicSize box = MeasureVideoLabel("FR", 2);
    Require(box.width == (2 * 6 + 1) * 2 && box.height == 9 * 2, "a 5 x 7 glyph and a gap per character, a pixel of box round");
    DrawVideoLabel(pixels, VideoPixelFormat::Rgba8, width, height, 4, 3, "FR", 2);
    const auto at = [&](uint32_t x, uint32_t y)
    {
        return pixels.data() + (y * width + x) * 4;
    };
    Require(at(3, 3)[0] == 77 && at(4, 2)[0] == 77, "the pixels left and above the box are untouched");
    Require(at(4 + box.width, 5)[0] == 77 && at(5, 3 + box.height)[0] == 77, "the pixels right and below the box are untouched");
    Require(at(4, 3)[0] == 0 && at(4, 3)[3] == 255, "the box is opaque black");
    // F's top row is full: its first font pixel is at one font pixel of box in from the corner.
    Require(at(4 + 2, 3 + 2)[0] == 255 && at(4 + 3, 3 + 3)[1] == 255, "the F's top-left font pixel is white");
    Require(at(4 + 2, 3 + 2 + 2 * 1)[0] == 255, "the F's stem");
    Require(at(4 + 2 + 2 * 2, 3 + 2 + 2 * 1)[0] == 0, "inside the F is the box's black");

    // Half floats: white is 1.0.
    std::vector<uint8_t> half(8 * 8 * 8, 0);
    DrawVideoLabel(half, VideoPixelFormat::RgbaHalf, 8, 8, 0, 0, "I", 1);
    uint16_t value = 0;
    std::memcpy(&value, half.data() + (1 * 8 + 2) * 8, sizeof(value));
    Require(value == 0x3C00u, "a glyph pixel is half-float 1.0");

    // Clipped to the picture.
    std::vector<uint8_t> small(4 * 4 * 4, 77);
    DrawVideoLabel(small, VideoPixelFormat::Rgba8, 4, 4, 2, 2, "ABC", 3);
    Require(small[(1 * 4 + 1) * 4] == 77 && small[(3 * 4 + 3) * 4] == 0, "a label past the edge is cut there");
}

// A car at the origin facing world +Z with no roll: the default cameras sit six metres out, level.
void TestDefaultCamerasAroundACar()
{
    const std::array<QuadCameraSettings, kQuadCameraCount> cameras = DefaultQuadCameras();
    PhysicsPose car;
    car.position = glm::vec3(10.0f, 1.0f, -5.0f);
    Camera lens;
    lens.nearPlane = 0.1f;
    lens.farPlane = 5000.0f;
    lens.exposureEv100 = 13.0f;

    const Camera front = PlaceQuadCamera(lens, car, cameras[static_cast<size_t>(QuadCameraSlot::Front)]);
    Require(Near(front.position, glm::vec3(car.position) + glm::vec3(0.0f, 0.8f, 6.0f)), "the front camera is ahead: " + Describe(front.position));
    Require(front.GetForward().z < -0.99f, "the front camera looks back at the car");
    Require(Near(front.worldUp, glm::vec3(0.0f, 1.0f, 0.0f)), "level");
    Require(front.farPlane == 5000.0f && front.exposureEv100 == 13.0f, "the lens's far plane and exposure");
    Require(front.nearPlane <= 0.05f, "close enough for the bodywork");
    Require(Near(front.fovDegrees, 35.0f), "its own field of view");

    const Camera rear = PlaceQuadCamera(lens, car, cameras[static_cast<size_t>(QuadCameraSlot::Rear)]);
    Require(Near(rear.position, glm::vec3(car.position) + glm::vec3(0.0f, 0.8f, -6.0f)) && rear.GetForward().z > 0.99f, "the rear camera behind, looking forward");

    // Vehicle space's +X is the car's left: facing world +Z, that is world +X.
    const Camera left = PlaceQuadCamera(lens, car, cameras[static_cast<size_t>(QuadCameraSlot::Left)]);
    Require(Near(left.position, glm::vec3(car.position) + glm::vec3(6.0f, 0.8f, 0.0f)), "the left camera on the car's left: " + Describe(left.position));
    Require(left.GetForward().x < -0.99f, "looking at the car's left side");
    const Camera right = PlaceQuadCamera(lens, car, cameras[static_cast<size_t>(QuadCameraSlot::Right)]);
    Require(Near(right.position, glm::vec3(car.position) + glm::vec3(-6.0f, 0.8f, 0.0f)), "the right camera on the car's right");
    // The camera's right is forward x up; seen from the car's right, its nose is on the right of
    // the picture.
    Require(glm::dot(right.GetRight(), glm::vec3(0.0f, 0.0f, 1.0f)) > 0.99f, "from the right, the car's front is to the right");
    Require(glm::dot(left.GetRight(), glm::vec3(0.0f, 0.0f, 1.0f)) < -0.99f, "from the left, the car's front is to the left");
}

// The cameras turn with the car's heading; its roll and pitch move only the cameras that tilt with
// the body.
void TestCamerasFollowHeadingNotTilt()
{
    QuadCameraSettings front = DefaultQuadCameras()[static_cast<size_t>(QuadCameraSlot::Front)];
    // Heading turned 90 degrees about +Y: forward becomes world +X.
    const glm::quat heading = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    // Rolled 10 degrees about its forward, then pitched 5 nose down about its left: the nose stays
    // over the heading.
    const glm::quat tilt = glm::angleAxis(glm::radians(5.0f), glm::vec3(1.0f, 0.0f, 0.0f)) *
                           glm::angleAxis(glm::radians(10.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    PhysicsPose car;
    car.rotation = heading * tilt;
    Camera lens;

    const Camera level = PlaceQuadCamera(lens, car, front);
    Require(Near(level.position, glm::vec3(6.0f, 0.8f, 0.0f), 1e-3f), "ahead of a car heading +X: " + Describe(level.position));
    Require(Near(level.worldUp, glm::vec3(0.0f, 1.0f, 0.0f)), "the horizon stays level");
    Require(std::abs(level.GetForward().y) < 0.05f, "looking level at the car");

    front.followBodyTilt = true;
    const Camera tilted = PlaceQuadCamera(lens, car, front);
    const glm::vec3 bodyUp = car.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
    Require(Near(tilted.worldUp, bodyUp), "fixed to the body, its up is the body's");
    Require(Near(tilted.position, car.rotation * glm::vec3(0.0f, 0.8f, 6.0f), 1e-3f), "and so is where it sits");

    // A car on its nose keeps the frame it has: there is no heading to lay flat.
    PhysicsPose nose;
    nose.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::quat frame = QuadCameraFrame(nose, false);
    Require(std::abs(glm::dot(frame, nose.rotation)) > 0.9999f, "straight up or down, the body's own rotation");
}

void TestClamp()
{
    QuadRecordingSettings settings;
    settings.cameras[0].width = 1;
    settings.cameras[1].height = 100000;
    settings.cameras[2].fovDegrees = 400.0f;
    settings.framesPerSecond = 0;
    const QuadRecordingSettings clamped = ClampQuadRecordingSettings(settings);
    Require(clamped.cameras[0].width == kQuadCameraMinSize && clamped.cameras[1].height == kQuadCameraMaxSize, "sizes held to the limits");
    Require(clamped.cameras[2].fovDegrees == 120.0f && clamped.framesPerSecond == 1, "field of view and frame rate held too");

    const VideoMosaic mosaic = ComputeQuadMosaic(QuadRecordingSettings{});
    Require(mosaic.width == 1920 && mosaic.height == 1080, "the default cameras make a 1080p video");
}

// The cameras and the video come back from the settings file; an older file keeps the defaults.
void TestSettingsSurviveTheSettingsFile()
{
    EngineSettings saved;
    saved.quadRecording.layout = VideoMosaicLayout::Cross;
    saved.quadRecording.framesPerSecond = 60;
    saved.quadRecording.labels = false;
    saved.quadRecording.cameras[1].width = 1280;
    saved.quadRecording.cameras[1].height = 720;
    saved.quadRecording.cameras[2].position = glm::vec3(-4.5f, 1.25f, 0.5f);
    saved.quadRecording.cameras[2].target = glm::vec3(0.0f, 0.4f, -0.25f);
    saved.quadRecording.cameras[3].fovDegrees = 50.0f;
    saved.quadRecording.cameras[3].followBodyTilt = true;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_quad_recording_settings_test.json";
    std::string error;
    Require(SaveEngineSettings(path, saved, error), "the settings save: " + error);
    EngineSettings loaded;
    Require(LoadEngineSettings(path, loaded, error), "the settings load: " + error);
    Require(loaded.quadRecording == saved.quadRecording, "the quad recording settings come back");

    std::ofstream(path) << "{ \"version\": 1 }";
    Require(LoadEngineSettings(path, loaded, error), "the old settings load: " + error);
    std::filesystem::remove(path);
    Require(loaded.quadRecording == QuadRecordingSettings{}, "without quad recording settings the defaults apply");
}
}

int main()
{
    try
    {
        TestGridOfEqualPictures();
        TestGridOfMixedPictures();
        TestCross();
        TestLabel();
        TestDefaultCamerasAroundACar();
        TestCamerasFollowHeadingNotTilt();
        TestClamp();
        TestSettingsSurviveTheSettingsFile();
    }
    catch (const std::exception& error)
    {
        std::cerr << "quad_recording_tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "quad_recording_tests passed\n";
    return 0;
}
