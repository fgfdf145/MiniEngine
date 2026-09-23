// Real spherical harmonics to band 2 in the Y-up basis of engine/renderer/spherical_harmonics.h:
// same order, same constants.
#ifndef SPHERICAL_HARMONICS_GLSL
#define SPHERICAL_HARMONICS_GLSL

void EvaluateShBasis(vec3 d, out float basis[9])
{
    basis[0] = 0.282095;
    basis[1] = 0.488603 * d.z;
    basis[2] = 0.488603 * d.y;
    basis[3] = 0.488603 * d.x;
    basis[4] = 1.092548 * d.x * d.z;
    basis[5] = 1.092548 * d.z * d.y;
    basis[6] = 0.315392 * (3.0 * d.y * d.y - 1.0);
    basis[7] = 1.092548 * d.x * d.y;
    basis[8] = 0.546274 * (d.x * d.x - d.z * d.z);
}

// Ramamoorthi and Hanrahan's clamped-cosine factors per coefficient.
const float SH_COSINE_LOBE[9] = float[9](
    3.14159265, 2.09439510, 2.09439510, 2.09439510,
    0.78539816, 0.78539816, 0.78539816, 0.78539816, 0.78539816);

#endif
