"""Shared settings for the Khronos reference comparison (see README.md)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ASSETS = REPO_ROOT / "assets"
KHRONOS = ASSETS / "khronos"
MODELS = KHRONOS / "models"
ENVIRONMENTS = KHRONOS / "environments"
CAPTURES = KHRONOS / "captures"
SCENES = ASSETS / "scenes" / "khronos"

SAMPLE_ASSETS = "KhronosGroup/glTF-Sample-Assets"
SAMPLE_ASSETS_RAW = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main"
ENVIRONMENT_URL = (
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Environments/low_resolution_hdrs/Cannon_Exterior.hdr"
)
ENVIRONMENT_FILE = "Cannon_Exterior.hdr"

# The Sample Viewer's defaults (GltfSVApp.js, 2026-09-26): IBL intensity 1, environment rotation
# 90 degrees. The engine lights an HDRI at INTENSITY cd/m^2 per texel unit and its reference view
# exposes a texel of 1 to 1. It must be 1: emissive colours are cd/m^2 in the engine and plain units in
# the viewer, so only then does an emissive of 1 show as the viewer shows it, next to the environment. HDRI_ROTATION_DEGREES is the engine rotation that matches the viewer's 90: found
# by rendering CompareMetallic at 0, 90, 180 and 270 (mean differences 51, 16, 3.0 and 33 of 255).
HDRI_INTENSITY = 1.0
HDRI_ROTATION_DEGREES = 180.0

# The models compared, grouped by what they test. Phase 4 adds the transmission set.
MODELS_BY_FEATURE = {
    "core": ["CompareBaseColor", "CompareMetallic", "CompareRoughness", "CompareNormal"],
    "clearcoat": ["CompareClearcoat", "ClearCoatTest"],
    "sheen": ["CompareSheen", "SheenTestGrid"],
    "specular": ["CompareSpecular", "SpecularTest"],
    "ior": ["CompareIor", "IORTestGrid"],
    "iridescence": ["CompareIridescence", "IridescenceMetallicSpheres", "IridescenceDielectricSpheres"],
    "anisotropy": ["CompareAnisotropy", "AnisotropyStrengthTest", "AnisotropyRotationTest"],
    "emissive_strength": ["CompareEmissiveStrength", "EmissiveStrengthTest"],
    "texture_transform": ["TextureTransformTest"],
    "unlit": ["UnlitTest"],
    "variants": ["MaterialsVariantsShoe"],
}


def all_models():
    return [model for models in MODELS_BY_FEATURE.values() for model in models]


def model_gltf(model):
    """The model's .gltf inside assets/, or None when it has not been fetched."""
    found = sorted((MODELS / model).glob("*.gltf"))
    return found[0] if found else None
