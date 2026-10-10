#include <engine/application/control_server.h>
#include <engine/application/control_session.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/logic/editor_world.h>
#include <engine/renderer/rhi/backend.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSocket = SOCKET;
#define CLOSE_TEST_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
#define CLOSE_TEST_SOCKET close
#endif

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;
using nlohmann::json;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// The interface's defaults: no GPU, no window.
class FakeBackend final : public IRenderBackend
{
  public:
    RenderBackendType GetBackendType() const override
    {
        return RenderBackendType::Vulkan;
    }
    void HandleEvent(const SDL_Event&) override
    {
    }
    void DrawFrame() override
    {
    }
};

// A client of the control channel, as a test drives it: one line out, one line back.
class TestClient
{
  public:
    explicit TestClient(uint16_t port)
    {
        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        Require(connect(m_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "cannot connect to the control channel");
    }
    ~TestClient()
    {
        CLOSE_TEST_SOCKET(m_socket);
    }
    void SendLine(const std::string& line)
    {
        const std::string text = line + "\n";
        Require(send(m_socket, text.data(), static_cast<int>(text.size()), 0) == static_cast<int>(text.size()), "send failed");
    }
    json ReadLine()
    {
        for (;;)
        {
            if (const size_t newline = m_pending.find('\n'); newline != std::string::npos)
            {
                const std::string line = m_pending.substr(0, newline);
                m_pending.erase(0, newline + 1);
                return json::parse(line);
            }
            char buffer[4096];
            const int received = recv(m_socket, buffer, static_cast<int>(sizeof(buffer)), 0);
            Require(received > 0, "the control channel closed the connection");
            m_pending.append(buffer, static_cast<size_t>(received));
        }
    }

  private:
    TestSocket m_socket;
    std::string m_pending;
};

struct Fixture
{
    ControlServer server{0};
    RendererSharedState state;
    FakeBackend backend;
    std::unique_ptr<ControlSession> session;

    Fixture()
    {
        state.editorWorld = CreateEditorWorld();
        state.editorWorld->CreateTwoCubeTestScene();
        session = std::make_unique<ControlSession>(server, state, backend);
    }

    // Set by the thread that reads an answer.
    std::atomic<bool> ready{false};
};

// Sends one request and runs frames until its answer is back.
json Request(Fixture& fixture, TestClient& client, const json& request)
{
    client.SendLine(request.dump());
    json answer;
    std::thread reader([&]()
                       {
                           answer = client.ReadLine();
                           fixture.ready = true;
                       });
    fixture.ready = false;
    for (int frame = 0; frame < 5000 && !fixture.ready; ++frame)
    {
        fixture.session->Update(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    reader.join();
    Require(answer.value("id", json()) == request.value("id", json()), "the answer carries the request's id");
    return answer;
}

// Requests go in over the socket and come back with their id; errors come back as errors.
void RoundTripsOverTheSocket()
{
    Fixture fixture;
    Require(fixture.server.Port() != 0, "port 0 takes a free port");
    TestClient client(fixture.server.Port());

    const json ping = Request(fixture, client, {{"id", 1}, {"cmd", "ping"}});
    Require(ping["ok"] == true, "ping answers");
    Require(ping["result"].contains("pid"), "ping tells the process id");

    const json unknown = Request(fixture, client, {{"id", "two"}, {"cmd", "no.such.command"}});
    Require(unknown["ok"] == false, "an unknown command is an error");
    Require(unknown["error"].get<std::string>().find("help") != std::string::npos, "the error points at help");

    client.SendLine("not json");
    json broken;
    std::thread reader([&]()
                       {
                           broken = client.ReadLine();
                           fixture.ready = true;
                       });
    fixture.ready = false;
    for (int frame = 0; frame < 5000 && !fixture.ready; ++frame)
    {
        fixture.session->Update(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    reader.join();
    Require(broken["ok"] == false && broken["id"].is_null(), "a line that is not JSON is answered with an error");
}

// frames answers only once that many frames have been drawn after it came in.
void FramesWaitForFrames()
{
    Fixture fixture;
    TestClient client(fixture.server.Port());
    client.SendLine(json{{"id", 5}, {"cmd", "frames"}, {"args", {{"count", 3}}}}.dump());
    // Ample time for the server's thread to queue it, so the first Update below takes it.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    json answer;
    std::thread reader([&]()
                       {
                           answer = client.ReadLine();
                           fixture.ready = true;
                       });
    // Frames the loop skips (minimized) do not count: every other one here.
    for (int frame = 0; frame < 5000 && !fixture.ready; ++frame)
    {
        fixture.session->Update(frame % 2 == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    reader.join();
    Require(answer["ok"] == true, "frames answers");
    // Taken on drawn frame 1, answered on drawn frame 4.
    Require(answer["result"]["frame"].get<uint64_t>() == 4, "frames waited for three drawn frames, got " + answer["result"].dump());
}

// render.set changes the editor's render settings by render.get's keys, all or none.
void RenderSettingsByKey()
{
    Fixture fixture;
    const json before = fixture.session->Execute("render.get", json::object());
    Require(before.contains("ray_tracing.reflections"), "render.get names grouped fields group.key");
    Require(before.contains("taa"), "render.get names top-level fields by key");
    Require(before.contains("camera.auto_exposure.enabled") || before.size() > 40, "render.get holds the camera's adaptation");

    const bool reflections = before["ray_tracing.reflections"].get<bool>();
    fixture.session->Execute("render.set", {{"values", {{"ray_tracing.reflections", !reflections}, {"path_tracing.max_bounces", 3}}}});
    Require(fixture.state.editorUi.EditRenderDebug().rayTracing.reflections == !reflections, "render.set sets a bool");
    Require(fixture.state.editorUi.EditRenderDebug().pathTracing.maxBounces == 3, "render.set sets a number");

    bool threw = false;
    try
    {
        fixture.session->Execute("render.set", {{"values", {{"ray_tracing.reflections", reflections}, {"no_such_setting", 1}}}});
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    Require(threw, "an unknown key is an error");
    Require(fixture.state.editorUi.EditRenderDebug().rayTracing.reflections == !reflections, "a failed render.set changes nothing");

    threw = false;
    try
    {
        fixture.session->Execute("render.set", {{"values", {{"taa", 3}}}});
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    Require(threw, "a number for a bool is an error");
}

// camera.set's look_at turns the camera to face the point.
void CameraLooksAt()
{
    Fixture fixture;
    const json camera = fixture.session->Execute("camera.set", {{"position", {1.0, 2.0, 3.0}}, {"look_at", {1.0, 2.0, -7.0}}, {"exposure_ev100", 11.5}});
    const glm::vec3 forward = fixture.state.camera.GetForward();
    Require(std::abs(forward.z + 1.0f) < 1e-4f && std::abs(forward.x) < 1e-4f && std::abs(forward.y) < 1e-4f, "the camera faces -Z");
    Require(!fixture.state.camera.autoExposure.enabled && fixture.state.camera.exposureEv100 == 11.5f, "a set exposure turns auto exposure off");
    Require(camera["position"][2].get<float>() == 3.0f, "camera.set answers with the camera");
}

// entities.list lists the scene; entity.set moves one found by name.
void EntitiesByName()
{
    Fixture fixture;
    const json entities = fixture.session->Execute("entities.list", json::object());
    Require(entities.is_array() && !entities.empty(), "the test scene has entities");
    const std::string name = entities[0]["name"].get<std::string>();
    const json moved = fixture.session->Execute("entity.set", {{"name", name}, {"position", {4.0, 5.0, 6.0}}});
    Require(moved["position"][1].get<float>() == 5.0f, "entity.set moves it");
    bool threw = false;
    try
    {
        fixture.session->Execute("entity.set", {{"name", "nobody"}, {"position", {0, 0, 0}}});
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    Require(threw, "an unknown name is an error");
}
}

// deterministic fixes the frame step and the exposure, and gives the camera its adaptation back.
void DeterministicFramesAndBack()
{
    Fixture fixture;
    fixture.state.camera.autoExposure.enabled = true;
    fixture.state.camera.autoWhiteBalance.enabled = true;
    fixture.state.camera.exposureEv100 = 9.5f;
    const uint32_t restartBefore = fixture.state.temporalRestart;
    const json on = fixture.session->Execute("deterministic", {{"frame_seconds", 0.0}});
    Require(fixture.state.fixedFrameSeconds == 0.0f, "frame_seconds 0 freezes time");
    Require(!fixture.state.camera.autoExposure.enabled && !fixture.state.camera.autoWhiteBalance.enabled, "adaptation is off");
    Require(on["exposure_ev100"].get<float>() == 9.5f, "the exposure is pinned where it was");
    Require(fixture.state.temporalRestart == restartBefore + 1, "temporal history restarts");
    fixture.session->Execute("deterministic", {{"enabled", false}});
    Require(!fixture.state.fixedFrameSeconds.has_value(), "real time again");
    Require(fixture.state.camera.autoExposure.enabled && fixture.state.camera.autoWhiteBalance.enabled, "adaptation comes back");

    const uint32_t fullBefore = fixture.state.fullRestart;
    fixture.session->Execute("restart_temporal", {{"full", true}});
    Require(fixture.state.fullRestart == fullBefore + 1, "a full restart is its own counter");
}

// scene.get and scene.set by path: the environment, a light by tag, kinds checked, all or none.
void ScenePaths()
{
    Fixture fixture;
    IEditorWorld& world = fixture.state.GetEditorWorld();
    SerializedLightData light;
    light.tagName = "Key";
    light.intensity = 500.0f;
    const entt::entity lightEntity = world.CreateLightEntity(light);

    const json environment = fixture.session->Execute("scene.get", {{"path", "environment"}});
    Require(environment.contains("exposure_compensation_ev"), "scene.get reads a section");
    Require(fixture.session->Execute("scene.get", {{"path", "lights.Key.intensity"}}).get<float>() == 500.0f, "arrays by tag");

    fixture.session->Execute("scene.set", {{"values", {{"lights.Key.intensity", 1234.0}, {"environment.exposure_compensation_ev", 1.5}}}});
    Require(world.GetLightComponent(lightEntity).intensity == 1234.0f, "scene.set changes the light");
    Require(world.GetEnvironment().exposureCompensationEv == 1.5f, "scene.set changes the environment");

    bool threw = false;
    try
    {
        fixture.session->Execute("scene.set", {{"values", {{"lights.Key.intensity", 1.0}, {"lights.Key.cast_shadows", 3}}}});
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    Require(threw, "a number for a bool is an error");
    Require(world.GetLightComponent(lightEntity).intensity == 1234.0f, "a failed scene.set changes nothing");

    threw = false;
    try
    {
        fixture.session->Execute("scene.get", {{"path", "lights.Nobody"}});
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    Require(threw, "an unknown tag is an error");
}

int main()
{
    try
    {
        DeterministicFramesAndBack();
        ScenePaths();
        RoundTripsOverTheSocket();
        FramesWaitForFrames();
        RenderSettingsByKey();
        CameraLooksAt();
        EntitiesByName();
    }
    catch (const std::exception& error)
    {
        std::cerr << "control channel tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "control channel tests passed\n";
    return 0;
}
