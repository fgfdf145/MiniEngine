#version 450

// HDR output: clears the viewport image's rectangle of the SDR UI layer to transparent black, in
// place of ImGui drawing the image there (see VulkanHdrComposite). Drawn without blending under a
// scissor, so whatever ImGui drew beneath (the panel's background) goes too; the overlays drawn after
// blend over the hole, leaving their coverage in alpha.
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(0.0);
}
