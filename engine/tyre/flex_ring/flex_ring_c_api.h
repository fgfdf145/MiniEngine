/* The flexible ring tyre for external programs (Python ctypes, MATLAB loadlibrary, C, C++): a plain C
 * interface to engine/tyre/flex_ring, in the shared library miniengine_flex_ring_c.
 *
 * Units are SI (m, s, N, Pa, rad) except the basic data items, which keep FTire's data file units (see
 * mefr_data_unit). Axes: global, z up. A rim rotation is a 3x3 row-major matrix taking rim-fixed axes to
 * global ones; its second column is the wheel's spin axis. Forces and moments are the tyre's on the rim,
 * global, about the rim centre.
 *
 * Every function returning int returns 0 on success, nonzero on failure (mefr_last_error says why). A
 * handle is not thread-safe; different handles may run on different threads at once.
 *
 *   mefr_tyre* t = mefr_create(NULL);              // the default data (the R34's Semislicks)
 *   mefr_set_data(t, "inflation_pressure", 2.2);   // bar, as in FTire's data files
 *   mefr_preprocess(t, 1);                         // fit the model
 *   mefr_road_flat(t, 0.0, 1.0);
 *   mefr_reset(t, pos, rot, vel, omega);
 *   for (...) mefr_advance(t, pos, rot, vel, omega, 0.001, force, moment);
 *   mefr_destroy(t);
 */
#ifndef MINIENGINE_FLEX_RING_C_API_H
#define MINIENGINE_FLEX_RING_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(MINIENGINE_FLEX_RING_C_EXPORTS)
#define MEFR_API __declspec(dllexport)
#elif defined(MINIENGINE_FLEX_RING_C_STATIC)
#define MEFR_API
#else
#define MEFR_API __declspec(dllimport)
#endif
#else
#define MEFR_API __attribute__((visibility("default")))
#endif

#define MEFR_VERSION 1

typedef struct mefr_tyre mefr_tyre;

/* The road's height (m) at x, y; `user` is passed through. */
typedef double (*mefr_height_fn)(void* user, double x, double y);

typedef struct mefr_contact
{
    int blocks;          /* tread blocks touching the road */
    int sliding;         /* of them sliding */
    double normal_force; /* N, sum */
    double area;         /* m^2 of rubber in contact */
    double length;       /* m, along the wheel's heading */
    double width;        /* m, across */
    double max_pressure; /* Pa */
    double mean_pressure;
    double centre[3];       /* force-weighted centre of the blocks, global */
    double friction_power;  /* W */
    int substeps;           /* internal steps of the last mefr_advance */
    double cpu_seconds;     /* wall time of the last mefr_advance */
} mefr_contact;

MEFR_API int mefr_version(void);
MEFR_API const char* mefr_last_error(void);

/* A tyre from an FTire-style data file (.tir), or the default data for a null path. */
MEFR_API mefr_tyre* mefr_create(const char* tir_path);
MEFR_API void mefr_destroy(mefr_tyre* tyre);

/* Basic data by item name (FTire's, case-insensitive) in the data file's units. Changes take effect at
 * the next mefr_preprocess. */
MEFR_API int mefr_data_count(void);
MEFR_API const char* mefr_data_name(int index);
MEFR_API const char* mefr_data_unit(int index);
MEFR_API const char* mefr_data_group(int index);
MEFR_API int mefr_set_data(mefr_tyre* tyre, const char* name, double value);
MEFR_API int mefr_get_data(const mefr_tyre* tyre, const char* name, double* value);
MEFR_API int mefr_save_data(const mefr_tyre* tyre, const char* path);

/* Pre-processing: fit (1) or only build (0) the model from the data. Resets the tyre. The fit's targets
 * can be read back: name, target and what the model shows. */
MEFR_API int mefr_preprocess(mefr_tyre* tyre, int fit);
MEFR_API int mefr_target_count(const mefr_tyre* tyre);
MEFR_API int mefr_target(const mefr_tyre* tyre, int index, const char** name, double* target, double* achieved);
/* A pre-processed parameter by its name in the reports ("radial stiffness per node", ...). */
MEFR_API int mefr_parameter_count(const mefr_tyre* tyre);
MEFR_API int mefr_parameter(const mefr_tyre* tyre, int index, const char** name, const char** unit, double* value);

/* Roads. */
MEFR_API void mefr_road_flat(mefr_tyre* tyre, double height, double friction);
MEFR_API void mefr_road_cleat(mefr_tyre* tyre, double x, double y, double across_x, double across_y, double width, double height, double bevel, double top_width,
                              int semicircular, double friction);
MEFR_API int mefr_road_grid(mefr_tyre* tyre, double x0, double y0, double dx, double dy, int nx, int ny, const double* heights /* nx * ny, x fastest */);
MEFR_API void mefr_road_callback(mefr_tyre* tyre, mefr_height_fn height, void* user);

/* Operating conditions. */
MEFR_API void mefr_set_pressure(mefr_tyre* tyre, double pascal);
MEFR_API void mefr_set_tread_depth(mefr_tyre* tyre, double metres);
MEFR_API void mefr_set_friction_scale(mefr_tyre* tyre, double scale);

/* Simulation: put the belt at rest on the rim; integrate dt seconds to the given rim state. */
MEFR_API int mefr_reset(mefr_tyre* tyre, const double position[3], const double rotation[9], const double velocity[3], const double angular_velocity[3]);
MEFR_API int mefr_advance(mefr_tyre* tyre, const double position[3], const double rotation[9], const double velocity[3], const double angular_velocity[3], double dt,
                          double force[3], double moment[3]);
/* Quasi-static settling at a fixed rim (FTire's statics). */
MEFR_API int mefr_settle(mefr_tyre* tyre, const double position[3], const double rotation[9]);

/* Outputs. */
MEFR_API int mefr_node_count(const mefr_tyre* tyre);
MEFR_API void mefr_nodes(const mefr_tyre* tyre, double* positions /* 3 per node */, double* torsion /* per node, may be null */);
MEFR_API int mefr_block_count(const mefr_tyre* tyre);
/* Per block 12 values: base xyz, tip xyz, force xyz, ground pressure, sliding speed, flags (1 contact,
 * 2 sliding). Returns the blocks written. */
MEFR_API int mefr_blocks(const mefr_tyre* tyre, double* out, int max_blocks);
MEFR_API void mefr_contact_stats(const mefr_tyre* tyre, mefr_contact* out);

/* Rigs. A steady state on a flat road at constant load: forces in road axes (Fx, Fy, Fz, Mx, My, Mz). */
MEFR_API int mefr_steady(mefr_tyre* tyre, double speed, double load, double slip_angle, double slip_ratio, double camber, int free_rolling, double forces[6],
                         double* effective_radius);
/* The test programs (comma-separated: "modal,static,sweep,cleat,bench" or "all") written as CSV, JSON
 * and a log into a directory. */
MEFR_API int mefr_run_report(mefr_tyre* tyre, const char* what, const char* out_dir, int quick);

#ifdef __cplusplus
}
#endif

#endif
