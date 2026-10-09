#include "flex_ring_c_api.h"

#include "flex_ring_preprocess.h"
#include "flex_ring_report.h"
#include "flex_ring_rig.h"
#include "flex_ring_tyre.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <exception>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace me::tyre::flexring;

struct mefr_tyre
{
    FlexRingData data;
    PreprocessResult pre;
    std::vector<ParameterRow> parameters;
    std::unique_ptr<FlexRingTyre> tyre;
    std::unique_ptr<Road> road;
    mefr_height_fn callback = nullptr;
    void* callbackUser = nullptr;
    double pressure = 0.0;
    double treadDepth = -1.0;
    double frictionScale = 1.0;
};

namespace
{
thread_local std::string g_error;

int Fail(const std::string& message)
{
    g_error = message;
    return 1;
}

RimState Rim(const double p[3], const double r[9], const double v[3], const double w[3])
{
    RimState rim;
    rim.position = Vec3(p[0], p[1], p[2]);
    // Row-major in, glm column-major: column j holds rotation[i * 3 + j] for i = 0..2.
    for (int j = 0; j < 3; ++j)
    {
        rim.rotation[j] = Vec3(r[0 * 3 + j], r[1 * 3 + j], r[2 * 3 + j]);
    }
    rim.velocity = v != nullptr ? Vec3(v[0], v[1], v[2]) : Vec3(0.0);
    rim.angularVelocity = w != nullptr ? Vec3(w[0], w[1], w[2]) : Vec3(0.0);
    return rim;
}

bool EnsureModel(mefr_tyre* t)
{
    if (!t->tyre)
    {
        if (mefr_preprocess(t, 1) != 0)
        {
            return false;
        }
    }
    return true;
}

void ApplyConditions(mefr_tyre* t)
{
    if (!t->tyre)
    {
        return;
    }
    t->tyre->SetRoad(t->road.get());
    if (t->pressure > 0.0)
    {
        t->tyre->SetPressure(t->pressure);
    }
    if (t->treadDepth >= 0.0)
    {
        t->tyre->SetTreadDepth(t->treadDepth);
    }
    t->tyre->SetFrictionScale(t->frictionScale);
}
}

extern "C" {

int mefr_version(void)
{
    return MEFR_VERSION;
}

const char* mefr_last_error(void)
{
    return g_error.c_str();
}

mefr_tyre* mefr_create(const char* path)
{
    try
    {
        auto t = std::make_unique<mefr_tyre>();
        t->data = path != nullptr && path[0] != '\0' ? ReadDataFile(path).data : MakeDefaultData();
        t->road = std::make_unique<FlatRoad>(0.0);
        return t.release();
    }
    catch (const std::exception& e)
    {
        Fail(e.what());
        return nullptr;
    }
}

void mefr_destroy(mefr_tyre* tyre)
{
    delete tyre;
}

int mefr_data_count(void)
{
    return static_cast<int>(Fields().size());
}

const char* mefr_data_name(int index)
{
    return index >= 0 && index < mefr_data_count() ? Fields()[static_cast<size_t>(index)].name : nullptr;
}

const char* mefr_data_unit(int index)
{
    return index >= 0 && index < mefr_data_count() ? Fields()[static_cast<size_t>(index)].unit : nullptr;
}

const char* mefr_data_group(int index)
{
    return index >= 0 && index < mefr_data_count() ? Fields()[static_cast<size_t>(index)].group : nullptr;
}

int mefr_set_data(mefr_tyre* t, const char* name, double value)
{
    if (t == nullptr || name == nullptr)
    {
        return Fail("null argument");
    }
    return SetField(t->data, name, value) ? 0 : Fail(std::string("no numeric data item ") + name);
}

int mefr_get_data(const mefr_tyre* t, const char* name, double* value)
{
    if (t == nullptr || name == nullptr || value == nullptr)
    {
        return Fail("null argument");
    }
    return GetField(t->data, name, *value) ? 0 : Fail(std::string("no numeric data item ") + name);
}

int mefr_save_data(const mefr_tyre* t, const char* path)
{
    try
    {
        WriteDataFile(t->data, path);
        return 0;
    }
    catch (const std::exception& e)
    {
        return Fail(e.what());
    }
}

int mefr_preprocess(mefr_tyre* t, int fit)
{
    if (t == nullptr)
    {
        return Fail("null tyre");
    }
    try
    {
        PreprocessOptions options;
        options.fitStatic = fit != 0;
        options.fitModal = fit != 0;
        t->pre = Preprocess(t->data, options);
        t->parameters = DescribeParameters(t->pre.parameters);
        t->tyre = std::make_unique<FlexRingTyre>(t->pre.parameters);
        ApplyConditions(t);
        return 0;
    }
    catch (const std::exception& e)
    {
        return Fail(e.what());
    }
}

int mefr_target_count(const mefr_tyre* t)
{
    return t != nullptr ? static_cast<int>(t->pre.targets.size()) : 0;
}

int mefr_target(const mefr_tyre* t, int index, const char** name, double* target, double* achieved)
{
    if (t == nullptr || index < 0 || index >= mefr_target_count(t))
    {
        return Fail("no such target");
    }
    const FitTarget& f = t->pre.targets[static_cast<size_t>(index)];
    if (name != nullptr)
    {
        *name = f.name.c_str();
    }
    if (target != nullptr)
    {
        *target = f.target;
    }
    if (achieved != nullptr)
    {
        *achieved = f.achieved;
    }
    return 0;
}

int mefr_parameter_count(const mefr_tyre* t)
{
    return t != nullptr ? static_cast<int>(t->parameters.size()) : 0;
}

int mefr_parameter(const mefr_tyre* t, int index, const char** name, const char** unit, double* value)
{
    if (t == nullptr || index < 0 || index >= mefr_parameter_count(t))
    {
        return Fail("no such parameter");
    }
    const ParameterRow& r = t->parameters[static_cast<size_t>(index)];
    if (name != nullptr)
    {
        *name = r.name.c_str();
    }
    if (unit != nullptr)
    {
        *unit = r.unit.c_str();
    }
    if (value != nullptr)
    {
        *value = r.value;
    }
    return 0;
}

void mefr_road_flat(mefr_tyre* t, double height, double friction)
{
    if (t == nullptr)
    {
        return;
    }
    t->road = std::make_unique<FlatRoad>(height, 0.0, 0.0, friction);
    ApplyConditions(t);
}

void mefr_road_cleat(mefr_tyre* t, double x, double y, double ax, double ay, double width, double height, double bevel, double topWidth, int semicircular, double friction)
{
    if (t == nullptr)
    {
        return;
    }
    CleatGeometry g;
    g.position[0] = x;
    g.position[1] = y;
    g.direction[0] = ax;
    g.direction[1] = ay;
    g.width = width;
    g.height = height;
    g.bevel = bevel;
    g.topWidth = topWidth;
    g.semicircular = semicircular != 0;
    t->road = std::make_unique<CleatRoad>(0.0, g, friction);
    ApplyConditions(t);
}

int mefr_road_grid(mefr_tyre* t, double x0, double y0, double dx, double dy, int nx, int ny, const double* heights)
{
    if (t == nullptr || heights == nullptr || nx < 2 || ny < 2)
    {
        return Fail("bad grid");
    }
    std::vector<double> h(heights, heights + static_cast<size_t>(nx) * ny);
    t->road = std::make_unique<GridRoad>(x0, y0, dx, dy, nx, ny, std::move(h));
    ApplyConditions(t);
    return 0;
}

void mefr_road_callback(mefr_tyre* t, mefr_height_fn height, void* user)
{
    if (t == nullptr)
    {
        return;
    }
    t->callback = height;
    t->callbackUser = user;
    t->road = std::make_unique<FunctionRoad>([t](double x, double y) {
        return t->callback != nullptr ? t->callback(t->callbackUser, x, y) : 0.0;
    });
    ApplyConditions(t);
}

void mefr_set_pressure(mefr_tyre* t, double pascal)
{
    if (t != nullptr)
    {
        t->pressure = pascal;
        ApplyConditions(t);
    }
}

void mefr_set_tread_depth(mefr_tyre* t, double metres)
{
    if (t != nullptr)
    {
        t->treadDepth = metres;
        ApplyConditions(t);
    }
}

void mefr_set_friction_scale(mefr_tyre* t, double scale)
{
    if (t != nullptr)
    {
        t->frictionScale = scale;
        ApplyConditions(t);
    }
}

int mefr_reset(mefr_tyre* t, const double position[3], const double rotation[9], const double velocity[3], const double angular[3])
{
    if (t == nullptr || position == nullptr || rotation == nullptr || !EnsureModel(t))
    {
        return Fail("bad tyre or rim state");
    }
    t->tyre->Reset(Rim(position, rotation, velocity, angular));
    return 0;
}

int mefr_advance(mefr_tyre* t, const double position[3], const double rotation[9], const double velocity[3], const double angular[3], double dt, double force[3],
                 double moment[3])
{
    if (t == nullptr || position == nullptr || rotation == nullptr || !EnsureModel(t))
    {
        return Fail("bad tyre or rim state");
    }
    try
    {
        const Wrench w = t->tyre->Advance(Rim(position, rotation, velocity, angular), dt);
        if (force != nullptr)
        {
            force[0] = w.force.x;
            force[1] = w.force.y;
            force[2] = w.force.z;
        }
        if (moment != nullptr)
        {
            moment[0] = w.moment.x;
            moment[1] = w.moment.y;
            moment[2] = w.moment.z;
        }
        return std::isfinite(w.force.x + w.force.y + w.force.z) ? 0 : Fail("the integration diverged");
    }
    catch (const std::exception& e)
    {
        return Fail(e.what());
    }
}

int mefr_settle(mefr_tyre* t, const double position[3], const double rotation[9])
{
    if (t == nullptr || position == nullptr || rotation == nullptr || !EnsureModel(t))
    {
        return Fail("bad tyre or rim state");
    }
    return t->tyre->SettleStatic(Rim(position, rotation, nullptr, nullptr)) ? 0 : Fail("did not settle");
}

int mefr_node_count(const mefr_tyre* t)
{
    return t != nullptr && t->tyre ? t->tyre->Segments() : 0;
}

void mefr_nodes(const mefr_tyre* t, double* positions, double* torsion)
{
    if (t == nullptr || !t->tyre)
    {
        return;
    }
    const int n = t->tyre->Segments();
    for (int k = 0; k < n; ++k)
    {
        const Vec3& x = t->tyre->NodePositions()[static_cast<size_t>(k)];
        if (positions != nullptr)
        {
            positions[3 * k + 0] = x.x;
            positions[3 * k + 1] = x.y;
            positions[3 * k + 2] = x.z;
        }
        if (torsion != nullptr)
        {
            torsion[k] = t->tyre->Torsion()[static_cast<size_t>(k)];
        }
    }
}

int mefr_block_count(const mefr_tyre* t)
{
    return t != nullptr && t->tyre ? static_cast<int>(t->tyre->Blocks().size()) : 0;
}

int mefr_blocks(const mefr_tyre* t, double* out, int maxBlocks)
{
    if (t == nullptr || !t->tyre || out == nullptr)
    {
        return 0;
    }
    const std::vector<BlockView>& blocks = t->tyre->Blocks();
    const int count = std::min(maxBlocks, static_cast<int>(blocks.size()));
    for (int i = 0; i < count; ++i)
    {
        const BlockView& b = blocks[static_cast<size_t>(i)];
        double* o = out + 12 * i;
        o[0] = b.base.x;
        o[1] = b.base.y;
        o[2] = b.base.z;
        o[3] = b.tip.x;
        o[4] = b.tip.y;
        o[5] = b.tip.z;
        o[6] = b.force.x;
        o[7] = b.force.y;
        o[8] = b.force.z;
        o[9] = b.groundPressure;
        o[10] = b.slidingSpeed;
        o[11] = (b.contact ? 1.0 : 0.0) + (b.sliding ? 2.0 : 0.0);
    }
    return count;
}

void mefr_contact_stats(const mefr_tyre* t, mefr_contact* out)
{
    if (t == nullptr || out == nullptr)
    {
        return;
    }
    std::memset(out, 0, sizeof(*out));
    if (!t->tyre)
    {
        return;
    }
    const ContactStats& c = t->tyre->Contact();
    out->blocks = c.blocks;
    out->sliding = c.sliding;
    out->normal_force = c.normalForce;
    out->area = c.area;
    out->length = c.length;
    out->width = c.width;
    out->max_pressure = c.maxPressure;
    out->mean_pressure = c.meanPressure;
    out->centre[0] = c.centre.x;
    out->centre[1] = c.centre.y;
    out->centre[2] = c.centre.z;
    out->friction_power = c.frictionPower;
    out->substeps = t->tyre->LastStep().substeps;
    out->cpu_seconds = t->tyre->LastStep().seconds;
}

int mefr_steady(mefr_tyre* t, double speed, double load, double slipAngle, double slipRatio, double camber, int freeRolling, double forces[6], double* effectiveRadius)
{
    if (t == nullptr || !EnsureModel(t))
    {
        return Fail("bad tyre");
    }
    try
    {
        SteadySettings s;
        s.speed = speed;
        s.load = load;
        s.slipAngle = slipAngle;
        s.slipRatio = slipRatio;
        s.camber = camber;
        s.freeRolling = freeRolling != 0;
        const SteadyPoint p = RunSteady(t->pre.parameters, s);
        if (forces != nullptr)
        {
            forces[0] = p.forces.fx;
            forces[1] = p.forces.fy;
            forces[2] = p.forces.fz;
            forces[3] = p.forces.mx;
            forces[4] = p.forces.my;
            forces[5] = p.forces.mz;
        }
        if (effectiveRadius != nullptr)
        {
            *effectiveRadius = p.effectiveRadius;
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        return Fail(e.what());
    }
}

int mefr_run_report(mefr_tyre* t, const char* what, const char* outDir, int quick)
{
    if (t == nullptr || outDir == nullptr)
    {
        return Fail("null argument");
    }
    try
    {
        const std::string w = what != nullptr ? what : "all";
        const auto has = [&](const char* key) {
            return w == "all" || w.find(key) != std::string::npos;
        };
        ReportOptions options;
        options.quick = quick != 0;
        options.modal = has("modal");
        options.statics = has("static");
        options.sweeps = has("sweep");
        options.cleat = has("cleat");
        options.benchmark = has("bench");
        options.outputDirectory = outDir;
        std::filesystem::create_directories(outDir);
        auto log = std::make_shared<std::ofstream>(std::filesystem::path(outDir) / "log.txt");
        options.log = [log](const std::string& line) {
            *log << line << "\n";
            log->flush();
        };
        RunReport(t->data, options);
        return 0;
    }
    catch (const std::exception& e)
    {
        return Fail(e.what());
    }
}
}
