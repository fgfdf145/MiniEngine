// Command line front end of the flexible ring tyre (engine/tyre/flex_ring): pre-processing, the virtual
// test rigs and the benchmark, writing CSV files an external program can read.
//
//   miniengine_flex_ring data <out.tir>                  write the default (R34) data file
//   miniengine_flex_ring preprocess [--data f.tir]       fit the model, print targets and parameters
//   miniengine_flex_ring modal [--data f.tir]            unloaded modes
//   miniengine_flex_ring static [--data f.tir] [--out d] Fz-deflection, footprint
//   miniengine_flex_ring sweep [--data f.tir] [--out d]  Fy/Mz(alpha), Fx(kappa) at several loads
//   miniengine_flex_ring cleat [--data f.tir] [--out d]  cleat run-over at 40/80/120 km/h
//   miniengine_flex_ring bench [--data f.tir] [--out d]  performance table
//   miniengine_flex_ring report [--data f.tir] --out d   all of the above
//   miniengine_flex_ring trace [--data f.tir]            roll up to 60 km/h at 3 kN, print every 10 ms
//                                                        (MINIENGINE_TRACE_SLIP=<ratio> or
//                                                        MINIENGINE_TRACE_ALPHA=<deg> from 0.6 s on)
//
// --set name=value overrides a data item (repeatable); --nofit skips the fit; --quick shortens sweeps.
// MINIENGINE_FLEX_RING_SWEEPS=<text> / MINIENGINE_FLEX_RING_BENCH=<text> run only the matching sweeps or
// benchmark cases.

#include <engine/tyre/flex_ring/flex_ring_report.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <glm/geometric.hpp>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace me::tyre::flexring;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: miniengine_flex_ring <data|preprocess|modal|static|sweep|cleat|bench|report|trace> [--data file.tir] [--out dir] [--set name=value] [--nofit] [--quick]\n";
        return 2;
    }
    const std::string command = argv[1];
    ReportOptions options;
    FlexRingData data = MakeDefaultData();
    std::string outPath;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--data" && i + 1 < argc)
        {
            const DataReadResult read = ReadDataFile(argv[++i]);
            data = read.data;
            for (const std::string& k : read.unknownKeys)
            {
                std::cerr << "unknown key: " << k << "\n";
            }
        }
        else if (a == "--out" && i + 1 < argc)
        {
            outPath = argv[++i];
        }
        else if (a == "--set" && i + 1 < argc)
        {
            const std::string kv = argv[++i];
            const size_t eq = kv.find('=');
            if (eq == std::string::npos || !SetField(data, kv.substr(0, eq), std::stod(kv.substr(eq + 1))))
            {
                std::cerr << "bad --set " << kv << "\n";
                return 2;
            }
        }
        else if (a == "--nofit")
        {
            options.fit = false;
        }
        else if (a == "--quick")
        {
            options.quick = true;
        }
        else if (command == "data" && outPath.empty())
        {
            outPath = a;
        }
        else
        {
            std::cerr << "unknown argument " << a << "\n";
            return 2;
        }
    }
    try
    {
        if (command == "data")
        {
            if (outPath.empty())
            {
                WriteData(data, std::cout);
            }
            else
            {
                WriteDataFile(data, outPath);
            }
            return 0;
        }
        if (command == "trace")
        {
            // Rolls the fitted tyre up to 60 km/h from a static load and prints every millisecond.
            const PreprocessResult pre = Preprocess(data);
            FlexRingTyre tyre(pre.parameters);
            FlatRoad road(0.0);
            RigPose pose;
            const double touch = RestTouchHeight(tyre, pose);
            const double deflection = DeflectionForLoad(tyre, road, pose, 3000.0, 2.5e5);
            std::cout << "static: deflection " << deflection * 1000.0 << " mm, load " << tyre.Contact().normalForce << " N" << std::endl;
            const double speed = 60.0 / 3.6;
            const double re = pre.parameters.beltRadius;
            double x = 0.0, angle = 0.0;
            const double dt = 0.001;
            double spin = 0.0;
            for (int i = 0; i < 1000; ++i)
            {
                const double t = (i + 1) * dt;
                const double b = std::min(t / 0.2, 1.0);
                const double v = speed * b * b * (3.0 - 2.0 * b);
                static const char* alphaText = std::getenv("MINIENGINE_TRACE_ALPHA");
                const double alpha = alphaText != nullptr && t > 0.6 ? std::atof(alphaText) * 3.14159265358979 / 180.0 : 0.0;
                const double vx = v * std::cos(alpha);
                const double vy = -v * std::sin(alpha);
                x += vx * dt;
                pose.y += vy * dt;
                static const char* slipText = std::getenv("MINIENGINE_TRACE_SLIP");
                static double freeSpin = 0.0;
                if (t <= 0.2)
                {
                    spin = v / re;
                }
                else if (slipText != nullptr && t > 0.6)
                {
                    // Prescribed slip ratio from 0.6 s on.
                    spin = (1.0 + std::atof(slipText)) * freeSpin;
                }
                else
                {
                    // A free wheel from here on: it finds the free-rolling speed.
                    spin += dt * glm::dot(tyre.LastWrench().moment, tyre.Rim().rotation[1]) / 1.6;
                    freeSpin = spin;
                }
                if (i % 100 == 99)
                {
                    std::printf("   free rolling: omega %.4f rad/s, Re %.5f m (belt radius %.5f)\n", spin, v / spin, re);
                }
                angle += spin * dt;
                pose.x = x;
                const Wrench w = tyre.Advance(MakeRim(pose, touch - deflection, angle, Vec3(vx, vy, 0.0), spin), dt);
                double fastest = 0.0;
                for (int k = 0; k < tyre.Segments(); ++k)
                {
                    const Vec3 rel = tyre.NodeVelocities()[k] - (tyre.Rim().velocity + glm::cross(tyre.Rim().angularVelocity, tyre.NodeRestPosition(k) - tyre.Rim().position));
                    fastest = std::max(fastest, glm::length(rel));
                }
                if (i % 10 == 0 || !std::isfinite(w.force.z) || (alphaText != nullptr && t > 0.6 && t < 0.62))
                {
                    std::printf("t %.3f v %.2f spin %.2f Fy %.1f Fx %.1f Fz %.1f load %.1f blocks %d sliding %d substeps %d failed %d relspeed %.3f cpu %.2f ms\n", t, v, spin, w.force.y, w.force.x, w.force.z, tyre.Contact().normalForce,
                                tyre.Contact().blocks, tyre.Contact().sliding, tyre.LastStep().substeps, tyre.LastStep().failedFactorizations, fastest, tyre.LastStep().seconds * 1000.0);
                }
                if (!std::isfinite(w.force.z))
                {
                    break;
                }
            }
            return 0;
        }
        options.outputDirectory = outPath;
        options.log = [](const std::string& line) {
            std::cout << line << std::endl;
        };
        if (command == "preprocess")
        {
            options.preprocessOnly = true;
        }
        else if (command == "modal")
        {
            options.modal = true;
        }
        else if (command == "static")
        {
            options.statics = true;
        }
        else if (command == "sweep")
        {
            options.sweeps = true;
        }
        else if (command == "cleat")
        {
            options.cleat = true;
        }
        else if (command == "bench")
        {
            options.benchmark = true;
        }
        else if (command == "report")
        {
            options.modal = options.statics = options.sweeps = options.cleat = options.benchmark = true;
        }
        else
        {
            std::cerr << "unknown command " << command << "\n";
            return 2;
        }
        RunReport(data, options);
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
