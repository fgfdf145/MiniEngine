// The atmosphere's ground as a surface (AtmosphereSettings::groundPlane): an endless plane at world
// y = 0 facing +Y, with the ground albedo, drawn into the G-buffer by ground.frag and met by the
// DDGI rays. Include after scene_common.glsl.
#ifndef GROUND_PLANE_GLSL
#define GROUND_PLANE_GLSL

// A rough dielectric, the default F0: the ground has no material of its own beyond its albedo.
const float GROUND_PLANE_ROUGHNESS = 0.9;

bool GroundPlaneEnabled()
{
    return ubo.groundAlbedo.w > 0.5;
}

// Where a ray meets the plane within (tMin, tMax), when it is drawn. frontFace is false for a ray
// from below it.
bool IntersectGroundPlane(vec3 origin, vec3 direction, float tMin, float tMax, out float t, out bool frontFace)
{
    frontFace = origin.y >= 0.0;
    t = direction.y != 0.0 ? -origin.y / direction.y : -1.0;
    return GroundPlaneEnabled() && t > tMin && t < tMax;
}

#endif
