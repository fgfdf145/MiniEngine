// The physics overlay's brush contact patch, drawn without a GPU: the GT-R corners on the brush tyre,
// the overlay draws its outer front tyre's patch from above through ImGui, and the triangles are
// rasterised in software. With MINIENGINE_UI_SNAPSHOT_DIR set the picture is written there as a PNG.

#include "gtr_car_spec.h"

#include <engine/editor/ui/editor_vehicle_overlay.h>
#include <engine/physics/physics_world.h>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace me;

namespace
{
constexpr int kWidth = 900;
constexpr int kHeight = 700;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

void ServeTextures(ImDrawData& drawData)
{
    if (drawData.Textures == nullptr)
    {
        return;
    }
    for (ImTextureData* texture : *drawData.Textures)
    {
        if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
        {
            texture->SetTexID(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)));
            texture->SetStatus(ImTextureStatus_OK);
        }
        else if (texture->Status == ImTextureStatus_WantDestroy)
        {
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

// ImGui's triangles into an RGB image (as the suspension rig window test does): edge functions, colours
// interpolated, the font texture sampled at the nearest texel, blended over a dark ground.
std::vector<float> Rasterise(const ImDrawData& drawData)
{
    std::vector<float> image(static_cast<size_t>(kWidth) * kHeight * 3, 0.08f);
    const auto colour = [](ImU32 c, int shift)
    {
        return static_cast<float>((c >> shift) & 0xFF) / 255.0f;
    };
    for (const ImDrawList* list : drawData.CmdLists)
    {
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            if (cmd.UserCallback != nullptr || cmd.ElemCount == 0)
            {
                continue;
            }
            const ImTextureData* texture = reinterpret_cast<const ImTextureData*>(static_cast<std::uintptr_t>(cmd.GetTexID()));
            const int clipX0 = std::max(0, static_cast<int>(cmd.ClipRect.x));
            const int clipY0 = std::max(0, static_cast<int>(cmd.ClipRect.y));
            const int clipX1 = std::min(kWidth, static_cast<int>(std::ceil(cmd.ClipRect.z)));
            const int clipY1 = std::min(kHeight, static_cast<int>(std::ceil(cmd.ClipRect.w)));
            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3)
            {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; ++k)
                {
                    v[k] = &list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i + k]];
                }
                const float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) - (v[1]->pos.y - v[0]->pos.y) * (v[2]->pos.x - v[0]->pos.x);
                if (std::abs(area) < 1e-8f)
                {
                    continue;
                }
                const int x0 = std::max(clipX0, static_cast<int>(std::floor(std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}))));
                const int y0 = std::max(clipY0, static_cast<int>(std::floor(std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}))));
                const int x1 = std::min(clipX1, static_cast<int>(std::ceil(std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}))));
                const int y1 = std::min(clipY1, static_cast<int>(std::ceil(std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}))));
                for (int y = y0; y < y1; ++y)
                {
                    for (int x = x0; x < x1; ++x)
                    {
                        const float px = x + 0.5f;
                        const float py = y + 0.5f;
                        float w[3];
                        for (int k = 0; k < 3; ++k)
                        {
                            const ImDrawVert* a = v[(k + 1) % 3];
                            const ImDrawVert* b = v[(k + 2) % 3];
                            w[k] = ((b->pos.x - a->pos.x) * (py - a->pos.y) - (b->pos.y - a->pos.y) * (px - a->pos.x)) / area;
                        }
                        if (w[0] < 0.0f || w[1] < 0.0f || w[2] < 0.0f)
                        {
                            continue;
                        }
                        float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        float u = 0.0f;
                        float t = 0.0f;
                        for (int k = 0; k < 3; ++k)
                        {
                            rgba[0] += w[k] * colour(v[k]->col, IM_COL32_R_SHIFT);
                            rgba[1] += w[k] * colour(v[k]->col, IM_COL32_G_SHIFT);
                            rgba[2] += w[k] * colour(v[k]->col, IM_COL32_B_SHIFT);
                            rgba[3] += w[k] * colour(v[k]->col, IM_COL32_A_SHIFT);
                            u += w[k] * v[k]->uv.x;
                            t += w[k] * v[k]->uv.y;
                        }
                        if (texture != nullptr && texture->Pixels != nullptr)
                        {
                            const int tx = std::clamp(static_cast<int>(u * texture->Width), 0, texture->Width - 1);
                            const int ty = std::clamp(static_cast<int>(t * texture->Height), 0, texture->Height - 1);
                            const unsigned char* texel = texture->Pixels + (static_cast<size_t>(ty) * texture->Width + tx) * texture->BytesPerPixel;
                            if (texture->Format == ImTextureFormat_RGBA32)
                            {
                                for (int c = 0; c < 4; ++c)
                                {
                                    rgba[c] *= texel[c] / 255.0f;
                                }
                            }
                            else
                            {
                                rgba[3] *= texel[0] / 255.0f;
                            }
                        }
                        float* out = &image[(static_cast<size_t>(y) * kWidth + x) * 3];
                        for (int c = 0; c < 3; ++c)
                        {
                            out[c] = out[c] * (1.0f - rgba[3]) + rgba[c] * rgba[3];
                        }
                    }
                }
            }
        }
    }
    return image;
}

void WritePng(const std::vector<float>& image, const std::filesystem::path& path)
{
    std::vector<unsigned char> bytes(image.size());
    for (size_t i = 0; i < image.size(); ++i)
    {
        bytes[i] = static_cast<unsigned char>(std::clamp(image[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    Require(stbi_write_png(path.string().c_str(), kWidth, kHeight, 3, bytes.data(), kWidth * 3) != 0, "writes " + path.string());
}

// The GT-R on the brush tyre (as in the vehicle brush tyre tests).
VehicleSettings Gtr()
{
    VehicleWheelLayout layout{};
    const float frontZ = 2.78f * (1.0f - 0.555f);
    const float rearZ = frontZ - 2.78f;
    layout[0] = {glm::vec3(0.8375f, 0.355f, frontZ), 0.355f, 0.33f};
    layout[1] = {glm::vec3(-0.8375f, 0.355f, frontZ), 0.355f, 0.33f};
    layout[2] = {glm::vec3(0.84f, 0.355f, rearZ), 0.355f, 0.33f};
    layout[3] = {glm::vec3(-0.84f, 0.355f, rearZ), 0.355f, 0.33f};
    VehicleSettings tuning = ApplyCarSpec(VehicleSettings{}, me::test::MakeGtrSpec());
    tuning.wheelRadius = 0.355f;
    VehicleSettings settings = FitVehicleSettingsToBounds(glm::vec3(-1.0f, 0.0f, -2.3f), glm::vec3(1.0f, 1.2f, 2.3f), tuning, &layout);
    settings.centerOfMassOffset = glm::vec3(0.0f, 0.38f - settings.chassisCenter.y, -settings.chassisCenter.z);
    return settings;
}

void Simulate(PhysicsWorld& world, float seconds)
{
    for (float time = 0.0f; time < seconds; time += 1.0f / 144.0f)
    {
        world.Update(1.0f / 144.0f);
    }
}

// Turning right at 20 m/s (about 1 g), the outer front tyre (front left) carries the most and slides at the
// back of its patch; its carcass is pushed the way the road pushes the tyre. Drawn from above, the
// patch shows both its sticking and its sliding bristles.
void TestOverlayDrawsTheBrushPatch()
{
    PhysicsWorld world;
    const std::vector<glm::vec3> ground = {{-300.0f, 0.0f, -300.0f}, {-300.0f, 0.0f, 300.0f}, {300.0f, 0.0f, 300.0f}, {300.0f, 0.0f, -300.0f}};
    Require(world.AddStaticMesh(ground, std::vector<uint32_t>{0, 1, 2, 0, 2, 3}), "the ground builds");
    const VehicleId car = world.AddVehicle(Gtr(), {glm::vec3(0.0f, 0.1f, -250.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    for (int i = 0; i < 2000 && world.GetVehicleTelemetry(car).forwardSpeed < 20.0f; ++i)
    {
        Simulate(world, 0.01f);
    }
    controls.throttle = 0.3f;
    controls.steering = 0.15f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 1.5f);

    const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
    const VehicleWheelState& outer = wheels[0];
    Require(outer.inContact, "the outer front tyre is on the ground");
    Require(outer.brushRibCount == 50, "the default 50 ribs, got " + std::to_string(outer.brushRibCount));
    float sliding = 0.0f;
    for (int rib = 0; rib < outer.brushRibCount; ++rib)
    {
        const VehicleWheelState::BrushRib& contact = outer.brushRibs[static_cast<size_t>(rib)];
        Require(contact.length > 0.05f && contact.length < 0.4f, "a rib's contact length is a tyre's, got " + std::to_string(contact.length));
        Require(contact.stuckLength >= 0.0f && contact.stuckLength <= contact.length + 1e-5f, "the stuck length lies within the rib");
        sliding += contact.length - contact.stuckLength;
    }
    const float fy = -outer.lateralForce; // along the car's left
    std::cout << "outer front at 20 m/s turning: Fy " << fy << " N, carcass y " << outer.carcassDeflection.y * 1000.0f << " mm, twist "
              << outer.carcassDeflection.z * 57.2958f << " deg, sliding length over the ribs " << sliding * 1000.0f << " mm\n";
    Require(sliding > 0.0f, "cornering hard, some bristles slide");
    Require(fy * outer.carcassDeflection.y > 0.0f, "the carcass is pushed the way the road pushes the tyre");

    // Recut into fewer ribs while driving (the Vehicle panel's Brush Ribs): the patch shows them at once.
    world.SetVehicleBrushTyreBristles(car, 24, 0);
    Simulate(world, 0.2f);
    const VehicleWheelState recut = world.GetVehicleWheels(car)[0];
    Require(recut.brushRibCount == 24, "recut into 24 ribs, got " + std::to_string(recut.brushRibCount));
    Require(std::abs(recut.lateralForce) > 0.5f * std::abs(outer.lateralForce), "still cornering on the recut tyres");

    // From above and a little behind the tyre, looking down at its patch.
    const glm::vec3 target(outer.contactPosition);
    const glm::vec3 eye = target + outer.contactNormal * 0.9f - outer.contactLongitudinal * 0.35f - outer.contactLateral * 0.1f;
    const glm::mat4 view = glm::lookAt(eye, target, outer.contactLongitudinal);
    glm::mat4 projection = glm::perspective(glm::radians(40.0f), static_cast<float>(kWidth) / kHeight, 0.05f, 100.0f);
    projection[1][1] *= 1.0f;
    const glm::mat4 viewProjection = projection * view;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    VehiclePhysicsOverlaySettings settings;
    settings.enabled = true;
    settings.frictionCircles = false;
    settings.suspension = false;
    settings.forces = false;
    settings.deformationScale = 10.0f;
    DrawVehiclePhysicsOverlay(*ImGui::GetBackgroundDrawList(), ImVec2(0.0f, 0.0f), io.DisplaySize, viewProjection, {outer}, settings, 1.0f);
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    ServeTextures(*drawData);
    const std::vector<float> image = Rasterise(*drawData);
    size_t green = 0;
    size_t red = 0;
    size_t yellow = 0;
    for (size_t pixel = 0; pixel + 2 < image.size(); pixel += 3)
    {
        const float r = image[pixel], g = image[pixel + 1], b = image[pixel + 2];
        green += (g > 0.45f && r < 0.4f && b < 0.5f) ? 1 : 0;
        red += (r > 0.55f && g < 0.35f && b < 0.35f) ? 1 : 0;
        yellow += (r > 0.7f && g > 0.6f && b < 0.35f) ? 1 : 0;
    }
    std::cout << "patch drawn: " << green << " sticking, " << red << " sliding, " << yellow << " carcass-line pixels\n";
    Require(green > 500 && red > 200 && yellow > 50, "the patch shows sticking and sliding bristles and the carcass line");
    if (const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR"))
    {
        std::filesystem::create_directories(folder);
        WritePng(image, std::filesystem::path(folder) / "brush_contact_patch.png");
    }
}
}

int main()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->AddFontDefault();
    int result = 0;
    try
    {
        TestOverlayDrawsTheBrushPatch();
        std::cout << "vehicle overlay tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "vehicle overlay tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
