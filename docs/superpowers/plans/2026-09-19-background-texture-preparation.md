# Background Texture Preparation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prepare material textures on worker threads so a scene change never blocks the editor's frame loop for seconds.

**Architecture:** A pure CPU `TexturePreparationQueue` in `engine_asset` runs `PrepareTexture` on a fixed worker pool. The renderer enqueues a change's missing textures, uploads finished ones a few per frame into a staged set, and runs the existing transactional `UploadSceneResources` once the queue is empty, taking textures from the live list, the staged set, or (as a fallback only) a synchronous prepare.

**Tech Stack:** C++20, std::thread / std::mutex / std::condition_variable, Vulkan 1.3, Dear ImGui.

**Spec:** [docs/superpowers/specs/2026-09-19-background-texture-preparation-design.md](../specs/2026-09-19-background-texture-preparation-design.md)

## Global Constraints

- C++20, Allman braces, `namespace me`; stage new files, then `scripts/check-format.ps1` must pass before each commit.
- Build `cmake --build --preset vs2026-x64-debug --parallel`; tests `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`.
- `engine_asset` stays free of Vulkan.
- Zero validation messages in Debug. The upload stays transactional: a failure leaves the previous content drawable.
- Commits go straight to `main`, ending with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`. Never stage `miniengine.settings.json` or `docs/superpowers/plans/2026-09-13-gbuffer-phase2-deferred-shading.md`.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `engine/asset/texture_preparation.{h,cpp}` | Create | `PreparedTexture`, `PrepareTexture`, `TexturePreparationQueue` |
| `tests/texture_preparation_tests.cpp`, `tests/CMakeLists.txt` | Create/Modify | `miniengine.texture_preparation` |
| `engine/asset/CMakeLists.txt` | Modify | New sources |
| `engine/renderer/vulkan/renderer.{h,cpp}` | Modify | Queue, staged set, request/pump, fallback-only inline prepare, frame-time warning |
| `engine/editor/renderer_shared_state.h`, `engine/editor/editor_backend_base.cpp`, `engine/editor/editor_ui.{h,cpp}`, `engine/editor/ui/editor_scene_panel.cpp` | Modify | Status line |
| `README.md` | Modify | Record the behaviour |

---

### Task 1: `TexturePreparationQueue`

**Interfaces — Produces:**
`struct PreparedTexture { std::optional<CompressedTexture> compressed; TextureData rgba; bool fromCache = false; double compressSeconds = 0.0; }`;
`PreparedTexture PrepareTexture(const std::string& path, TextureUsage usage, bool compress, const std::filesystem::path& cacheDirectory)`;
`struct TexturePreparationRequest { std::string key; std::string path; TextureUsage usage; }`;
`struct TexturePreparationResult { std::string key; TextureUsage usage; std::optional<PreparedTexture> texture; std::string error; }`;
`class TexturePreparationQueue` with `TexturePreparationQueue(PrepareFunction, uint32_t workerCount)`, `bool Enqueue(TexturePreparationRequest)`, `std::vector<TexturePreparationResult> TakeCompleted(size_t maxResults)`, `bool Contains(const std::string&) const`, `bool IsIdle() const`, `size_t PendingCount() const`, where `using PrepareFunction = std::function<PreparedTexture(const std::string& path, TextureUsage usage)>`.

- [ ] **Step 1: Failing tests** (`tests/texture_preparation_tests.cpp`, linking `engine_asset`), each with an injected prepare function:
  - `CompletesAndIsTakenOnce`: three requests; poll `TakeCompleted(16)` until three results arrive (5 s timeout); keys match; `IsIdle()` then true and a further take is empty.
  - `DuplicateKeyIsRefusedWhilePending`: a prepare function blocked on a flag; `Enqueue` of the same key twice returns true then false; `Contains` true; after release and take, the key can be enqueued again.
  - `ThrowingPrepareBecomesAFailedResult`: prepare throws `std::runtime_error("bad png")`; the result has no texture and `error == "bad png"`.
  - `NotIdleUntilTaken`: after completion but before `TakeCompleted`, `IsIdle()` is false and `PendingCount()` is 1.
  - `DestructionDiscardsQueuedWork`: one worker; prepare blocks until a flag a helper thread sets after 50 ms; three requests; destroying the queue returns after the running one finishes and prepare was called exactly once.
- [ ] **Step 2:** build fails (header missing).
- [ ] **Step 3: Implement.** Move `PreparedTexture`/`PrepareTexture` from `renderer.cpp`'s anonymous namespace, adding the cache directory parameter. The queue holds `std::mutex`, `std::condition_variable`, a `std::deque` of queued requests, a `std::deque` of completed results, the set of pending keys (queued, running or completed-not-taken), a running count and a stopping flag; `workerCount` `std::thread`s loop: wait for work or stop, pop, run the prepare function outside the lock, catch `std::exception` into `error`, push the result. The destructor sets stopping, drops queued requests (and their keys), notifies all and joins.
- [ ] **Step 4:** tests pass; full ctest; format.
- [ ] **Step 5: Commit** `feat(asset): prepare textures on a worker pool`.

### Task 2: Renderer integration

- [ ] **Step 1: Members** (`renderer.h`): `std::unique_ptr<TexturePreparationQueue> m_texturePreparation`; `std::unordered_map<std::string, std::unique_ptr<VulkanTexture>> m_stagedTextures`; `std::unordered_set<std::string> m_failedTextureKeys`; `bool m_sceneUploadPending`; `size_t m_texturesRequested`; a `TextureUploadStats { size_t fromCache, compressedNow, uncompressed; double compressSeconds; }` member. Methods: `RequestSceneUpload()`, `PumpSceneUpload()`, `AbandonPendingTextures()`, `std::unique_ptr<VulkanTexture> UploadPreparedTexture(const PreparedTexture&, TextureUsage, VulkanUploadBatch&)`.
- [ ] **Step 2: Construction and teardown.** The constructor creates the queue after the device, with `max(1, hardware_concurrency / 2)` workers and a prepare function capturing `m_device->SupportsBlockCompression()` and `EnginePaths::CacheRoot() / "textures"`. The destructor resets the queue first, and clears `m_stagedTextures` after the device wait and before `DestroyDeviceResources`.
- [ ] **Step 3: Request.** `RequestSceneUpload` calls `DropSubmeshesOfRemovedEntities`, builds the set of live keys from `m_textureCacheKeys`, walks `rendererWorld.GetRenderSubmeshes()` (textured ones) over the thirteen slots with their usages, and enqueues each `BuildTextureCacheKey(path, usage)` that is not live, staged, failed or already in the queue, counting accepted requests into `m_texturesRequested`; then sets `m_sceneUploadPending`.
- [ ] **Step 4: Pump.** If no upload is pending, completed results are taken and dropped. Otherwise up to four are taken and uploaded through one batch into `m_stagedTextures` (failed ones logged, their keys recorded); an out-of-memory error abandons the change like an upload failure (`AbandonPendingTextures`, error text, `m_sceneUploadPending = false`). If still pending and the queue is idle, clear the flag and run `UploadSceneResourcesOrKeepPrevious`. Finally set `State().sceneUploadStatus` to `"Preparing textures: {done} of {total}"` while pending, else empty.
- [ ] **Step 5: Upload.** Delete the parallel prefetch block from `UploadSceneResources`. `loadTextureIndex` tries live, then staged (moving it into the new list), then failed (fallback index), then synchronous `PrepareTexture` + `UploadPreparedTexture`. `UploadSceneResourcesOrKeepPrevious` logs the stats and clears staged and failed on success; on out-of-memory it also calls `AbandonPendingTextures`.
- [ ] **Step 6: DrawFrame.** Replace both `UploadSceneResourcesOrKeepPrevious()` calls with `RequestSceneUpload(); PumpSceneUpload();`, add a `PumpSceneUpload()` every frame after `ProcessPendingOperations`, and a local RAII timer that logs `LOG_WARN("A frame took {:.1f} s", seconds)` over one second.
- [ ] **Step 7: Verify.** Debug build, full ctest, validation run with Sponza (zero messages); cold Release Sponza with the cache cleared: no frame warning longer than a second from texture work, log shows textures completing while frames continue.
- [ ] **Step 8: Commit** `feat(vulkan): prepare scene textures in the background`.

### Task 3: Status line, acceptance, record

- [ ] `RendererSharedState::sceneUploadStatus`; `EditorUiController::Draw` and `DrawScenePanel` take it after `lastSceneIoError` and show it with `ImGui::TextDisabled` at the top of the Scene panel.
- [ ] Windows Application event log shows no Application Hang after a cold tree load and a cold Sponza load.
- [ ] README section 7: texture preparation runs on worker threads; the scene updates in one step when a change's textures are ready.
- [ ] Commit `feat(editor): show texture preparation progress` and `docs(readme): record background texture preparation`.
