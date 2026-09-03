# P0 — Device Capability Foundation Design

Part of [Ray Tracing Hybrid Pipeline — Decomposition Design](2026-09-04-raytracing-hybrid-pipeline-design.md).
That document holds the measured baseline, the locked decisions, and why this
sub-project runs first.

## Goal

Make the Vulkan logical device request the features every later ray tracing
sub-project needs, select the physical device deliberately rather than by
enumeration order, and report what was actually enabled.

Nothing in the rendered image changes. No acceleration structure, allocator,
or descriptor work happens here.

## Current Problems

- `instance.cpp` requests `VK_API_VERSION_1_3`, but `device.cpp` passes a plain
  `VkPhysicalDeviceFeatures` with an empty `pNext`. Not one Vulkan 1.2 or 1.3
  feature is enabled, so `bufferDeviceAddress` (needed by P1 and P3) and the
  descriptor indexing bits (needed by P2) are unavailable.
- `device.cpp` requires only `VK_KHR_swapchain`. None of the ray tracing
  extensions are requested.
- `VulkanDevice::IsSuitable` accepts the first device that passes. On this
  machine the discrete GPU happens to enumerate first, but the specification
  does not guarantee that, and the integrated GPU present here exposes no ray
  tracing extensions at all. Selecting it would silently disable every later
  sub-project.
- Nothing above the device knows what the device can do, so there is no basis
  for a rasterization fallback in P5.

## Components

### `engine/renderer/device_selection.h` / `.cpp` (new, `engine_render_core`)

Backend-agnostic scoring, deliberately free of Vulkan types so it lands in
`engine_render_core` and can be unit tested the way `material_alpha_tests`
already tests `material_pipeline`. `device.cpp` translates at the boundary.

```cpp
enum class GpuKind
{
    Discrete,
    Integrated,
    Other
};

// Feature bits this engine cares about. `descriptorIndexing` is an aggregate:
// true only when every sub-bit listed in the feature chain below is true,
// because a partial set is unusable for the P2 bindless design.
struct GpuFeatureBits
{
    bool bufferDeviceAddress = false;
    bool descriptorIndexing = false;
    bool accelerationStructure = false;
    bool rayQuery = false;
    bool rayTracingPipeline = false;
};

struct GpuProbe
{
    std::string name;
    GpuKind kind = GpuKind::Other;
    uint32_t apiVersion = 0;
    bool hasGraphicsQueue = false;
    bool hasPresentQueue = false;
    bool hasSwapchainSupport = false;
    std::set<std::string> extensions;
    GpuFeatureBits features;
};

struct GpuScore
{
    bool eligible = false;
    int score = 0;
    std::string rejectReason; // populated only when ineligible
};

GpuScore ScoreGpu(const GpuProbe& probe);
bool IsRayTracingReady(const GpuProbe& probe);

// Returns the index of the winning probe, or nullopt when none is eligible.
// Scoring and the tie-break both live here so the selection rule is testable
// as a whole rather than only per device.
std::optional<size_t> SelectGpu(std::span<const GpuProbe> probes);
```

**Eligibility.** A device is ineligible, with a recorded reason, when any of
these is missing: `apiVersion` at least 1.3; a graphics queue family; a present
queue family; `VK_KHR_swapchain` together with non-empty surface formats and
present modes.

The API version check is new and load-bearing. The instance declares 1.3, so
chaining `VkPhysicalDeviceVulkan12Features` onto a device reporting less than
1.3 is invalid usage.

**Scoring.** Eligible devices score `+1000` for `GpuKind::Discrete`, and `+300`
when `IsRayTracingReady` holds. Ray tracing readiness is all-or-nothing —
`VK_KHR_acceleration_structure`, `VK_KHR_ray_query` and
`VK_KHR_deferred_host_operations` all present, *and* the `accelerationStructure`
and `rayQuery` feature bits both true. Partial support scores nothing because
partial support cannot run P5.

`SelectGpu` takes the highest score. Ties resolve to the lowest enumeration
index, so selection stays deterministic across runs. Ineligible probes are
never selected, even when every candidate is ineligible — the caller gets
`nullopt` and raises the error described under Error Handling.

### `engine/renderer/vulkan/device_features.h` / `.cpp` (new, `engine_renderer`)

Holds the `VkPhysicalDeviceFeatures2` pNext chain and its translation to and
from `GpuFeatureBits`.

The chain is:

```
VkPhysicalDeviceFeatures2
  └─ VkPhysicalDeviceVulkan12Features
       └─ VkPhysicalDeviceAccelerationStructureFeaturesKHR
            └─ VkPhysicalDeviceRayQueryFeaturesKHR
```

`VkPhysicalDeviceVulkan11Features` and `VkPhysicalDeviceVulkan13Features` are
deliberately absent: no bit in either is used by P0 through P5. P4 may add
`Vulkan13Features` for `synchronization2` when it needs it.

Bits requested from `VkPhysicalDeviceVulkan12Features`:

| Bit | Needed by |
| --- | --- |
| `bufferDeviceAddress` | P1 allocator flag, P3 acceleration structure input |
| `runtimeDescriptorArray` | P2 |
| `descriptorBindingPartiallyBound` | P2 |
| `descriptorBindingVariableDescriptorCount` | P2 |
| `descriptorBindingSampledImageUpdateAfterBind` | P2 |
| `shaderSampledImageArrayNonUniformIndexing` | P2 |

Plus `accelerationStructure` and `rayQuery` from their respective structures.
The existing `samplerAnisotropy` moves into `VkPhysicalDeviceFeatures2::features`
and keeps its current conditional-on-support behavior.

**Copy and move are deleted on the chain type.** The members hold each other's
addresses through `pNext`; copying or moving the aggregate leaves dangling
pointers. This is the same hazard the 2026-09-03 record documents for
`PipelineVariantState`, and the reason is written into the header.

### `VulkanDeviceCapabilities`

```cpp
struct VulkanDeviceCapabilities
{
    GpuFeatureBits supported; // what the physical device reported
    GpuFeatureBits enabled;   // what vkCreateDevice actually turned on
};
```

The two sets stay separate rather than collapsing into one. They differ
whenever a bit is supported but deliberately not requested — most visibly
`rayTracingPipeline`, which is probed and reported so the capability is
visible, but never enabled, because P5 uses ray query and enabling an unused
feature is not free.

### `engine/renderer/vulkan/device.cpp` (modified)

- `kRequiredExtensions` keeps only `VK_KHR_swapchain`. A new
  `kOptionalRayTracingExtensions` holds `VK_KHR_deferred_host_operations`,
  `VK_KHR_acceleration_structure` and `VK_KHR_ray_query`. The 1.2-core
  dependencies of those extensions — buffer device address, descriptor
  indexing, SPIR-V 1.4 — need no extension strings on a 1.3 device.
- `IsSuitable` is replaced by a probe pass over every enumerated device that
  builds a `GpuProbe` vector and hands it to `SelectGpu`.
- `pEnabledFeatures` becomes `nullptr`; the chain hangs off
  `VkDeviceCreateInfo::pNext`.
- `VulkanDevice::GetCapabilities()` is added.

## Data Flow

```
enumerate physical devices
  → per device: extensions, vkGetPhysicalDeviceFeatures2,
                queue families, swapchain support  →  GpuProbe
  → SelectGpu (ScoreGpu per probe, highest wins, ties by lowest index)
  → m_physicalDevice, m_capabilities.supported
  → intersect requested extensions and feature bits with what is supported
  → m_capabilities.enabled
  → vkCreateDevice(pNext = &features2, pEnabledFeatures = nullptr)
  → log
```

**Only bits the physical device reported as true are enabled.** Query first,
then bitwise-and against the request set. Enabling an extension whose feature
bit is false is undefined behavior in later calls, and drivers do ship that
combination.

## Error Handling

- No eligible device: keep the existing `std::runtime_error`, but extend the
  message with the per-device rejection reasons collected during probing, so
  the log says which device failed which criterion.
- Eligible device without ray tracing: **do not throw.** Start normally, leave
  `capabilities.enabled.rayQuery` false, and log one warning. This is the
  correct behavior for an integrated-GPU-only machine and the precondition for
  the P5 rasterization fallback.
- Ray tracing extension present but its feature bit false: treat as
  unsupported. The extension is not enabled and the device does not score as
  ray tracing ready.

Capabilities are exposed only through `VulkanDevice::GetCapabilities()` and the
log. `rhi/backend.h` and `RenderBackendDescriptor` are not touched — they
describe whether a backend can be created, which is a different layer. Showing
capabilities in the editor UI is P7 work.

## Logging

One block at device creation, on `LOG_INFO`, listing: the selected device name
and score; every other candidate with its score or rejection reason; each
optional extension as enabled or missing; and each feature bit in
`capabilities.enabled` individually. A single `LOG_WARN` when the selected
device is not ray tracing ready.

This log is the acceptance evidence for the parts that cannot be automated.

## Testing

`tests/device_selection_tests.cpp`, linking `engine_render_core` only, in the
style of the existing `material_alpha_tests`:

- discrete with ray tracing beats integrated with ray tracing;
- discrete with ray tracing beats discrete without;
- missing `VK_KHR_swapchain` is ineligible, with a reason;
- `apiVersion` below 1.3 is ineligible, with a reason;
- an extension present while its feature bit is false does not count as ray
  tracing ready;
- equal scores resolve to the lower enumeration index.

The feature chain itself and `vkCreateDevice` cannot be exercised in CTest.
They are covered by a 60-frame smoke run plus manual reading of the startup
log. Per README rule 5 this is recorded as log inspection, not as verified
behavior.

## Out of Scope

Enabling `VK_KHR_ray_tracing_pipeline`; any use of the capabilities beyond
logging and the accessor; editor UI surfacing; changes to `rhi/backend.h`;
`VkPhysicalDeviceAccelerationStructurePropertiesKHR` limit queries, which P3
will need and P3 will add; and every other sub-project P1 through P5.
