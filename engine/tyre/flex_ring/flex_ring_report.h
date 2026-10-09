#pragma once

#include "flex_ring_data.h"
#include "flex_ring_preprocess.h"
#include "flex_ring_rig.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace me::tyre::flexring
{

// The test programs of the FTire publications run on one tyre (docs/design/2026-10-09-flex-ring-tyre-design.md):
// pre-processing targets, unloaded modes, static deflection and stiffness, footprint, steady-state slip
// characteristics at Fz = 2, 4, 6, 8 kN (Gipser 1999, figs. 7-12), cleat run-over at 40, 80, 120 km/h
// (figs. 5, 6), and the computing time table (Gipser 1999, table 1; Gipser 2004, fig. 14).
struct ReportOptions
{
    bool fit = true;
    bool quick = false;
    bool preprocessOnly = false;
    bool modal = false;
    bool statics = false;
    bool sweeps = false;
    bool cleat = false;
    bool benchmark = false;
    std::string outputDirectory; // CSV, JSON and Markdown files; empty: none
    std::function<void(const std::string&)> log;
    const std::atomic<bool>* cancel = nullptr;
};

struct SweepSeries
{
    std::string name;  // e.g. "Fz=4000N"
    SweepKind kind = SweepKind::SlipAngle;
    double load = 0.0;
    double fixedSlipAngle = 0.0;
    double fixedCamber = 0.0;
    std::vector<SteadyPoint> points;
};

struct CleatSeries
{
    std::string name;
    double speed = 0.0;
    CleatGeometry cleat;
    std::vector<CleatSample> samples;
};

struct BenchmarkCase
{
    std::string name;
    BenchmarkResult result;
};

struct ReportResult
{
    PreprocessResult preprocess;
    ModalResult modal;
    std::vector<StaticPoint> statics;
    StaticStiffness stiffness;
    std::vector<BlockView> footprint; // blocks in contact at the first static load
    std::vector<SweepSeries> sweeps;
    std::vector<CleatSeries> cleats;
    std::vector<BenchmarkCase> benchmarks;
    SteadyPoint freeRolling; // 60 km/h, first static load
};

// Runs what the options ask for; writes the files when a directory is given.
ReportResult RunReport(const FlexRingData& data, const ReportOptions& options);

// The internal (pre-processed) parameters as name, value, unit rows, for reports and the editor.
struct ParameterRow
{
    std::string name;
    double value = 0.0;
    std::string unit;
};
std::vector<ParameterRow> DescribeParameters(const FlexRingParameters& p);

}
