#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrTexture;

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 color = texture(hdrTexture, fragTexCoord).rgb;

    // Reinhard, moved verbatim out of triangle.frag. The expression is unchanged so that this
    // pass produces the same values the forward shader used to produce.
    color = color / (color + vec3(1.0));

    // This pass is the sole writer of the LDR target and knows coverage is total, so it writes
    // alpha explicitly rather than relying on the RGB-only color write mask the material
    // pipelines use to keep the attachment's clear alpha intact. ImGui composites the viewport
    // image over the editor, so this alpha must be 1.0.
    outColor = vec4(color, 1.0);
}
