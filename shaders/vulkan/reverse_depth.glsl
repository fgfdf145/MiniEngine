// The scene's depth is reverse-Z (engine/renderer/vulkan/reverse_depth.h): the near plane is at
// depth 1 and the far plane at 0, which is also what the depth attachments clear to, so a pixel
// no geometry wrote reads DEPTH_FAR and a nearer surface has the greater depth. Positions still
// reconstruct through invViewProj and ViewZFromDepth unchanged; only comparisons and the constant
// planes flip.

#ifndef REVERSE_DEPTH_GLSL
#define REVERSE_DEPTH_GLSL

#define DEPTH_NEAR 1.0
#define DEPTH_FAR 0.0

// True where no geometry (and no ground) was drawn: the background.
bool IsFarDepth(float depth)
{
    return depth <= DEPTH_FAR;
}

#endif
