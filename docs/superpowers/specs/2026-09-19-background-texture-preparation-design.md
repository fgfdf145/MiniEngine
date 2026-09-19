# Background Texture Preparation Design

## Goal

Keep the editor responsive while a scene change waits on texture work. Decoding, compressing and
cache reads of material texture files move to worker threads; the editor keeps drawing the scene
as it was until every texture the change needs is ready, and only then uploads and swaps the new
content in one step, as today.

## Motivation

Dragging the NewSponza cypress tree into a Debug editor froze it until Windows closed it as hung
(Application Hang, event 1002): all texture preparation ran on the main thread inside
`UploadSceneResources`, and a window that pumps no messages for five seconds is "not responding".
Commit `3ce9f63` shortened the work, but a cold NewSponza load still blocks for about 11 s in
Release. The blocking part is CPU work that needs no GPU: PNG decode, mip generation, BC encoding
and reading compressed textures from the cache.

## Current State

- A renderables change (model load or placement, deletion, material edit) sets `renderablesDirty`;
  `DrawFrame` calls `UploadSceneResourcesOrKeepPrevious` at two points in the frame.
- `UploadSceneResources` prepares every material texture file not already live in parallel chunks
  (`PrepareTexture`), uploads it, uploads every submesh's geometry, and commits through
  `ApplyRenderContent`. It is transactional: a failure leaves the previous content intact.
- `DropSubmeshesOfRemovedEntities` removes submeshes of deleted entities from the live list, the
  one change the previous content cannot survive.

## Locked Decisions

1. **Only texture preparation moves off the main thread.** Geometry upload and the commit stay
   where they are: they touch the GPU and the render lists, and they are short (NewSponza's warm
   upload is about a second).
2. **The commit waits for all of a change's textures.** The screen shows the previous content, never
   a half-textured model: the new content appears in one step when its last texture is ready. A
   change that needs no new texture file (a deletion, a material factor edit, a model whose textures
   are already live) uploads in the same frame, exactly as today.
3. **Prepared textures are uploaded as they finish,** a few per frame, into a staged set the next
   commit takes from. This bounds host memory to the textures in flight rather than the whole
   scene's worth of decoded or compressed pixels.
4. **Later changes join the wait.** A change made while textures are still being prepared adds its
   own missing textures to the queue; the commit happens once the queue is empty, against the scene
   as it is then. Textures a later change made unnecessary are prepared anyway and discarded at the
   commit.
5. **Deletions apply immediately** through `DropSubmeshesOfRemovedEntities`, so the previous content
   never draws a destroyed entity while the change is pending.
6. **Status is visible:** while textures are pending, the Scene panel shows
   "Preparing textures: done of total" in grey above any error.

## Components

### `engine/asset/texture_preparation.{h,cpp}` (pure CPU, no Vulkan)

`PreparedTexture` and `PrepareTexture` move here from `renderer.cpp` unchanged, with the cache
directory as a parameter. New:

```cpp
struct TexturePreparationRequest { std::string key; std::string path; TextureUsage usage; };
struct TexturePreparationResult
{
    std::string key;
    TextureUsage usage;
    std::optional<PreparedTexture> texture; // empty when preparation threw
    std::string error;
};

class TexturePreparationQueue
{
  public:
    using PrepareFunction = std::function<PreparedTexture(const std::string& path, TextureUsage usage)>;
    TexturePreparationQueue(PrepareFunction prepare, uint32_t workerCount);
    ~TexturePreparationQueue(); // discards queued requests, waits for running ones, joins

    // False, and no work, when the key is already queued, running or completed but not taken.
    bool Enqueue(TexturePreparationRequest request);
    std::vector<TexturePreparationResult> TakeCompleted(size_t maxResults);
    bool Contains(const std::string& key) const;
    // Nothing queued, running or waiting to be taken.
    bool IsIdle() const;
    size_t PendingCount() const; // queued + running + completed, not yet taken
};
```

Workers are `std::thread`s created once, `max(1, hardware threads / 2)` of them, leaving room for
the main thread and for the band-parallel encoder inside each texture. Exceptions thrown by the
prepare function become failed results carrying the message.

### Renderer

- `m_texturePreparation` (the queue), `m_stagedTextures` (key to uploaded `VulkanTexture`),
  `m_failedTextureKeys`, `m_sceneUploadPending`, and the preparation counts for the log line and the
  status text.
- `RequestSceneUpload()`, called where `UploadSceneResourcesOrKeepPrevious` is called today: drops
  submeshes of removed entities, enqueues every material texture file of the current scene that is
  not live, staged, failed or already queued, and sets `m_sceneUploadPending`.
- `PumpSceneUpload()`, called every frame and right after a request: takes up to four completed
  results, uploads them into `m_stagedTextures` through one batch (a failed one is logged and its
  key recorded, so its slots fall back to the default texture), then, if an upload is pending and the
  queue is idle, runs `UploadSceneResourcesOrKeepPrevious`.
- `UploadSceneResources` loses its parallel prefetch. A texture lookup tries, in order: live, staged,
  failed (fallback), and finally synchronous `PrepareTexture` — reached only by the startup upload
  in the constructor and by any texture the queue somehow missed, so correctness never depends on
  the queue. After a successful commit the staged set and the failed keys are cleared; after a
  failed one (out of memory) the staged textures are released with the abandoned upload.
- The destructor destroys the queue first (joining its workers), then the staged textures.
- `DrawFrame` logs a warning naming the duration whenever one frame takes longer than one second,
  so a regression back to main-thread stalls shows up in the log.

### Status

`RendererSharedState` gains `std::string sceneUploadStatus`, set by the renderer while
`m_sceneUploadPending` and empty otherwise. `EditorUiController::Draw` and `DrawScenePanel` take it
and show it in grey at the top of the Scene panel.

## Failure Handling

- A texture that fails to decode: its result carries the error; logged once; its slots use the
  default texture, as today.
- Out of GPU memory while staging a texture: the pending change is abandoned exactly as an
  out-of-memory upload is today (previous content kept, error shown); staged textures are released.
- Editor shutdown during preparation: the queue destructor discards queued work and joins after the
  running textures finish.

## Verification

1. `miniengine.texture_preparation` (new, pure CPU, with an injected prepare function):
   requests complete and are taken once; a duplicate key is refused while pending; a throwing
   request becomes a failed result; `IsIdle` is false until everything is taken; destroying a queue
   with blocked work neither hangs nor crashes (the prepare function is released by a flag).
2. The existing suite, and zero validation messages in Debug.
3. Cold NewSponza load: the longest frame stays well under five seconds (measured from the frame
   loop), the log shows textures completing while frames continue, and the model appears in one
   step.
4. Windows reports no Application Hang for a cold load of the tree and of NewSponza.

## Out of Scope

- Asynchronous geometry upload or model parsing changes (model loading is already asynchronous).
- Showing partially textured content.
- Cancelling preparation that a later change made unnecessary.
