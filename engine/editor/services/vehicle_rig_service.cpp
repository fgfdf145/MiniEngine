#include "vehicle_rig_service.h"

#include <engine/asset/model_cache.h>
#include <engine/core/log/log.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/physics/vehicle_suspension.h>
#include <engine/renderer/renderer_world.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace me
{

namespace
{
constexpr double kStep = 1e-3;          // the rig's own step, s
constexpr double kSampleStep = 0.005;   // the plotted history's, s
constexpr double kHistorySeconds = 10.0;
constexpr double kMaxPadVelocity = 0.5; // m/s: the sine and the sweep slow their amplitude above it
constexpr double kFadeSeconds = 1.0;    // a new input fades in over this
constexpr double kMaxFrameSeconds = 0.1;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr const char* kCornerNames[4] = {"FL", "FR", "RL", "RR"};

// A fresh rig, settled under the car's weight before the pads move.
std::unique_ptr<suspension::SevenPostRig> MakeRig(const suspension::CarModel& car, bool friction)
{
    auto rig = std::make_unique<suspension::SevenPostRig>(car, friction);
    const std::array<double, 4> zero{};
    for (int i = 0; i < 1000; ++i)
    {
        rig->Step(zero, zero, 0.0, 0.0, 0.0, kStep);
    }
    return rig;
}

// The body's displacement (m, up) where a point of the rig frame (x forward, y left) rides on it.
double BodyAt(const suspension::SevenPostRig& rig, double x, double y)
{
    return rig.Heave() + x * rig.Pitch() + y * rig.Roll();
}

// The pads' heights at the session's time; also the frequency they move at.
std::array<double, 4> PadHeights(VehicleRigSession& session, double& frequency)
{
    const VehicleRigExcitation& e = session.excitation;
    const double since = session.time - session.inputStart;
    const double fade = std::clamp(since / kFadeSeconds, 0.0, 1.0);
    const std::array<double, 4> pattern = suspension::RigModePattern(e.mode);
    std::array<double, 4> pads{};
    frequency = 0.0;
    double x = 0.0;
    switch (e.waveform)
    {
    case VehicleRigWaveform::Sine:
    {
        frequency = std::max(e.frequency, 0.01f);
        session.phase = std::fmod(session.phase + kTwoPi * frequency * kStep, kTwoPi);
        const double amplitude = std::min(static_cast<double>(e.amplitude), kMaxPadVelocity / (kTwoPi * frequency));
        x = fade * amplitude * std::sin(session.phase);
        break;
    }
    case VehicleRigWaveform::Sweep:
    {
        const suspension::SineSweep sweep(0.5, 20.0, 60);
        const double t = std::fmod(since, sweep.Duration());
        frequency = sweep.Frequency(t);
        const double amplitude = std::min(static_cast<double>(e.amplitude), kMaxPadVelocity / (kTwoPi * frequency));
        x = fade * amplitude * std::sin(sweep.Phase(t));
        break;
    }
    case VehicleRigWaveform::Step:
    {
        const double cycle = std::fmod(since * std::max(e.frequency, 0.01f), 1.0);
        x = since < 0.5 ? 0.0 : (cycle < 0.5 ? e.amplitude : 0.0);
        break;
    }
    case VehicleRigWaveform::BodyLoads:
        frequency = std::max(e.frequency, 0.01f);
        return pads; // level: the loaders do the work
    case VehicleRigWaveform::Road:
    {
        if (!session.road)
        {
            session.road = std::make_unique<suspension::RandomRoad>(e.roadRoughness, 2.0, 7);
        }
        const suspension::CarModel& car = session.car;
        const double wheelbase = 0.5 * (car.corners[0].position.x + car.corners[1].position.x) - 0.5 * (car.corners[2].position.x + car.corners[3].position.x);
        const double s = e.roadSpeed * since;
        const suspension::RandomRoad& road = *session.road;
        pads[0] = fade * (road.Height(0, s) - road.Height(0, 0.0));
        pads[1] = fade * (road.Height(1, s) - road.Height(1, 0.0));
        pads[2] = fade * (road.Height(0, s - wheelbase) - road.Height(0, -wheelbase));
        pads[3] = fade * (road.Height(1, s - wheelbase) - road.Height(1, -wheelbase));
        return pads;
    }
    }
    for (int i = 0; i < 4; ++i)
    {
        pads[i] = pattern[i] * x;
    }
    return pads;
}

void Sample(VehicleRigSession& session, double frequency)
{
    VehicleRigStatus& h = session.history;
    const suspension::SevenPostRig& rig = *session.rig;
    h.time = session.time;
    h.inputFrequency = frequency;
    h.sampleTime.push_back(session.time);
    for (int i = 0; i < 4; ++i)
    {
        h.pad[i].push_back(session.pads[i]);
        h.tyreLoad[i].push_back(rig.TyreLoad(i));
        h.travel[i].push_back(rig.Travel(i));
    }
    h.heave.push_back(rig.Heave());
    h.pitch.push_back(rig.Pitch() * 180.0 / std::numbers::pi);
    h.roll.push_back(rig.Roll() * 180.0 / std::numbers::pi);
    const size_t keep = static_cast<size_t>(kHistorySeconds / kSampleStep);
    if (h.sampleTime.size() > keep + keep / 5)
    {
        const auto trim = [&](std::vector<double>& v) {
            v.erase(v.begin(), v.end() - static_cast<std::ptrdiff_t>(keep));
        };
        trim(h.sampleTime);
        trim(h.heave);
        trim(h.pitch);
        trim(h.roll);
        for (int i = 0; i < 4; ++i)
        {
            trim(h.pad[i]);
            trim(h.tyreLoad[i]);
            trim(h.travel[i]);
        }
    }
}

void PutBack(IEditorWorld& world, entt::entity entity, const TransformComponent& transform)
{
    if (world.IsValidEntity(entity) && world.Registry().all_of<TransformComponent>(entity))
    {
        world.EditTransform(entity) = transform;
        world.MarkTransformDirty(entity);
    }
}

// The car, its wheels and the props where the simulation has them, the motion drawn `exaggeration`
// times larger.
void Pose(RendererSharedState& state, VehicleRigSession& session)
{
    IEditorWorld& world = state.GetEditorWorld();
    const suspension::SevenPostRig& rig = *session.rig;
    const float gain = session.excitation.exaggeration;
    const glm::vec3 o = session.origin;
    glm::mat4 body = glm::translate(glm::mat4(1.0f), o + session.up * static_cast<float>(rig.Heave() * gain));
    body = glm::rotate(body, static_cast<float>(rig.Roll() * gain), session.forward);
    body = glm::rotate(body, static_cast<float>(-rig.Pitch() * gain), session.left);
    body = glm::translate(body, -o);
    world.ApplyTransformMatrix(session.entity, session.startMatrix * body);

    // Each wheel's parts ride up and down against the body.
    std::array<glm::mat4, 4> corners{};
    for (int i = 0; i < 4; ++i)
    {
        const suspension::Vec3& p = session.car.corners[i].position;
        const double relative = rig.WheelHeight(i) - BodyAt(rig, p.x, p.y);
        corners[i] = glm::translate(glm::mat4(1.0f), session.up * static_cast<float>(relative * gain));
    }
    const LoadedModelData& model = *session.model;
    std::vector<glm::mat4> transforms(model.submeshes.size(), glm::mat4(1.0f));
    for (size_t index = 0; index < model.submeshes.size(); ++index)
    {
        const ModelSubmeshData& submesh = model.submeshes[index];
        if (submesh.wheelPart != ModelWheelPart::None && submesh.wheelCorner < 4)
        {
            transforms[index] = corners[submesh.wheelCorner];
        }
    }
    state.rendererWorld.SetSubmeshLocalTransforms(session.entity, std::move(transforms));

    for (const VehicleRigSession::Prop& prop : session.props)
    {
        if (!world.IsValidEntity(prop.entity))
        {
            continue;
        }
        TransformComponent transform = prop.start;
        if (prop.corner >= 0)
        {
            transform.translation.y += static_cast<float>(session.pads[prop.corner] * gain);
        }
        else
        {
            // An aero loader: its foot stays on the base, its top follows the body.
            const float lift = static_cast<float>(BodyAt(rig, prop.rigPoint.x, prop.rigPoint.y) * gain);
            transform.scale.y = std::max(prop.start.scale.y + lift, 0.01f);
            transform.translation.y += 0.5f * (transform.scale.y - prop.start.scale.y);
        }
        world.EditTransform(prop.entity) = transform;
        world.MarkTransformDirty(prop.entity);
    }
}
}

namespace VehicleRigService
{

void Start(RendererSharedState& state, entt::entity entity, const VehicleRigExcitation& excitation)
{
    IEditorWorld& world = state.GetEditorWorld();
    if (entity == entt::null || !world.HasModelComponent(entity))
    {
        throw std::runtime_error("select the car's model in the scene to put it on the rig");
    }
    Stop(state);

    auto session = std::make_unique<VehicleRigSession>();
    session->entity = entity;
    session->name = world.GetTag(entity).name;
    session->model = ModelCache::Get(world.GetModel(entity).sourcePath);
    if (!session->model || !session->model->carSpec.has_value() || !session->model->carSpec->frontSuspension.has_value() ||
        !session->model->carSpec->rearSuspension.has_value())
    {
        throw std::runtime_error("this model carries no suspension linkage");
    }
    if (!session->model->wheelRig.has_value())
    {
        throw std::runtime_error("this model names no wheels (WHEEL_LF and the others)");
    }
    session->car = BuildCarModel(*session->model->carSpec, session->name);
    session->excitation = excitation;
    session->friction = excitation.friction;
    session->rig = MakeRig(session->car, session->friction);

    // The rig frame in the model: forward from the rear wheels to the front, left from the right
    // wheels to the left, its origin the sprung mass's centre (the rig's own) at the wheels' height.
    const std::array<ModelWheelRig::Corner, kModelWheelCornerCount>& wheels = session->model->wheelRig->corners;
    const glm::vec3 front = 0.5f * (wheels[0].center + wheels[1].center);
    const glm::vec3 rear = 0.5f * (wheels[2].center + wheels[3].center);
    session->forward = glm::normalize(front - rear);
    const glm::vec3 across = wheels[0].center - wheels[1].center;
    session->left = glm::normalize(across - session->forward * glm::dot(across, session->forward));
    session->up = glm::cross(session->forward, session->left);
    const double frontX = 0.5 * (session->car.corners[0].position.x + session->car.corners[1].position.x);
    session->origin = front - session->forward * static_cast<float>(frontX);

    session->startTransform = world.GetTransform(entity);
    session->startMatrix = world.GetModelMatrix(entity);
    const glm::mat4 toModel = glm::inverse(session->startMatrix);
    for (const entt::entity other : world.GetSceneOrder())
    {
        if (other == entity || !world.Registry().all_of<TagComponent, TransformComponent>(other))
        {
            continue;
        }
        const std::string& tag = world.GetTag(other).name;
        VehicleRigSession::Prop prop;
        prop.entity = other;
        prop.start = world.GetTransform(other);
        if (tag.rfind("Wheel pad ", 0) == 0)
        {
            const std::string corner = tag.substr(10);
            for (int i = 0; i < 4; ++i)
            {
                prop.corner = corner == kCornerNames[i] ? i : prop.corner;
            }
            if (prop.corner >= 0)
            {
                session->props.push_back(prop);
            }
        }
        else if (tag.rfind("Aero loader", 0) == 0)
        {
            const glm::vec3 top = prop.start.translation + glm::vec3(0.0f, 0.5f * prop.start.scale.y, 0.0f);
            const glm::vec3 d = glm::vec3(toModel * glm::vec4(top, 1.0f)) - session->origin;
            prop.rigPoint = glm::vec3(glm::dot(d, session->forward), glm::dot(d, session->left), glm::dot(d, session->up));
            session->props.push_back(prop);
        }
    }
    for (int i = 0; i < 4; ++i)
    {
        session->history.staticTyreLoad[i] = session->rig->StaticTyreLoad(i);
    }
    session->history.vehicleName = session->name;
    LOG_INFO("Seven-post rig running '{}' with {} rig props", session->name, session->props.size());
    state.vehicleRig.session = std::move(session);
    state.vehicleRig.lastError.clear();
}

void Stop(RendererSharedState& state)
{
    std::unique_ptr<VehicleRigSession> session = std::move(state.vehicleRig.session);
    if (!session)
    {
        return;
    }
    IEditorWorld& world = state.GetEditorWorld();
    state.rendererWorld.ClearSubmeshLocalTransforms(session->entity);
    PutBack(world, session->entity, session->startTransform);
    for (const VehicleRigSession::Prop& prop : session->props)
    {
        PutBack(world, prop.entity, prop.start);
    }
    LOG_INFO("Stopped the seven-post rig on '{}'", session->name);
}

void SetExcitation(RendererSharedState& state, const VehicleRigExcitation& excitation)
{
    VehicleRigSession* session = state.vehicleRig.session.get();
    if (session == nullptr)
    {
        return;
    }
    const VehicleRigExcitation& old = session->excitation;
    if (excitation.friction != session->friction)
    {
        session->friction = excitation.friction;
        session->rig = MakeRig(session->car, session->friction);
        session->inputStart = session->time;
    }
    if (excitation.mode != old.mode || excitation.waveform != old.waveform)
    {
        session->inputStart = session->time;
        session->phase = 0.0;
    }
    if (excitation.roadRoughness != old.roadRoughness)
    {
        session->road.reset();
    }
    session->excitation = excitation;
}

bool Tick(RendererSharedState& state, float deltaSeconds)
{
    VehicleRigSession* session = state.vehicleRig.session.get();
    if (session == nullptr)
    {
        return false;
    }
    if (!state.GetEditorWorld().HasModelComponent(session->entity))
    {
        Stop(state);
        return false;
    }
    const double rate = std::clamp(static_cast<double>(session->excitation.playbackRate), 0.0, 1.0);
    session->pendingSeconds = std::min(session->pendingSeconds + std::min(static_cast<double>(deltaSeconds), kMaxFrameSeconds) * rate, kMaxFrameSeconds);
    double frequency = session->history.inputFrequency;
    while (session->pendingSeconds >= kStep)
    {
        session->pendingSeconds -= kStep;
        session->time += kStep;
        const std::array<double, 4> previous = session->pads;
        session->pads = PadHeights(*session, frequency);
        std::array<double, 4> rates{};
        for (int i = 0; i < 4; ++i)
        {
            rates[i] = (session->pads[i] - previous[i]) / kStep;
        }
        // The body loaders: a sine at the input's frequency, faded in like the pads.
        double heaveForce = 0.0;
        double pitchMoment = 0.0;
        double rollMoment = 0.0;
        const VehicleRigExcitation& e = session->excitation;
        if (e.waveform == VehicleRigWaveform::BodyLoads)
        {
            const double since = session->time - session->inputStart;
            session->phase = std::fmod(session->phase + kTwoPi * std::max(e.frequency, 0.01f) * kStep, kTwoPi);
            const double load = std::clamp(since / kFadeSeconds, 0.0, 1.0) * std::sin(session->phase) * e.bodyLoad * session->car.sprungMass * 9.81;
            switch (e.mode)
            {
            case suspension::RigMode::Heave:
                heaveForce = load;
                break;
            case suspension::RigMode::Pitch:
                pitchMoment = load * session->car.cgHeight;
                break;
            case suspension::RigMode::Roll:
            case suspension::RigMode::Warp:
                rollMoment = load * session->car.cgHeight;
                break;
            }
        }
        session->rig->Step(session->pads, rates, heaveForce, pitchMoment, rollMoment, kStep);
        if (session->time >= session->nextSample)
        {
            session->nextSample = session->time + kSampleStep;
            Sample(*session, frequency);
        }
    }
    Pose(state, *session);
    return true;
}

VehicleRigStatus GetStatus(const RendererSharedState& state)
{
    VehicleRigStatus status;
    if (const VehicleRigSession* session = state.vehicleRig.session.get())
    {
        status = session->history;
        status.active = true;
    }
    status.lastError = state.vehicleRig.lastError;
    return status;
}

void RunWithRigAtStart(RendererSharedState& state, const std::function<void()>& action)
{
    const VehicleRigSession* session = state.vehicleRig.session.get();
    if (session != nullptr)
    {
        // The next frame's Tick poses everything again.
        IEditorWorld& world = state.GetEditorWorld();
        PutBack(world, session->entity, session->startTransform);
        for (const VehicleRigSession::Prop& prop : session->props)
        {
            PutBack(world, prop.entity, prop.start);
        }
    }
    action();
}
}
}
