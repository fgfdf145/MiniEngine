#pragma once

#include "texture_compression.h"
#include "texture_loader.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace me
{

// A material texture file ready to upload: block-compressed when the device samples BC formats,
// RGBA8 otherwise.
struct PreparedTexture
{
    std::optional<CompressedTexture> compressed;
    TextureData rgba;
    bool fromCache = false;
    double compressSeconds = 0.0;
};

// The CPU half of loading one texture file; safe to run on any thread. When `compress` is set the
// compressed form comes from `cacheDirectory` or is built and cached there; a texture that cannot be
// compressed or cached is decoded to RGBA8 instead, on its own. A texture that cannot be decoded at
// all throws.
PreparedTexture PrepareTexture(
    const std::string& path,
    TextureUsage usage,
    bool compress,
    const std::filesystem::path& cacheDirectory);

struct TexturePreparationRequest
{
    // What the caller finds the result by; a key is prepared at most once at a time.
    std::string key;
    std::string path;
    TextureUsage usage = TextureUsage::Color;
};

struct TexturePreparationResult
{
    std::string key;
    TextureUsage usage = TextureUsage::Color;
    // Empty when preparation threw; `error` then holds the message.
    std::optional<PreparedTexture> texture;
    std::string error;
};

// Prepares textures on a fixed pool of worker threads so the frame loop never waits on decoding or
// compression. Requests run in the order given; results are collected with TakeCompleted.
class TexturePreparationQueue
{
  public:
    using PrepareFunction = std::function<PreparedTexture(const std::string& path, TextureUsage usage)>;

    TexturePreparationQueue(PrepareFunction prepare, uint32_t workerCount);
    // Discards requests not yet started, waits for the running ones and joins the workers.
    ~TexturePreparationQueue();

    TexturePreparationQueue(const TexturePreparationQueue&) = delete;
    TexturePreparationQueue& operator=(const TexturePreparationQueue&) = delete;

    // Returns false, and queues nothing, when the key is already queued, running, or completed but
    // not yet taken.
    bool Enqueue(TexturePreparationRequest request);
    // Up to `maxResults` finished requests, oldest first. Each result is returned exactly once.
    std::vector<TexturePreparationResult> TakeCompleted(size_t maxResults);
    bool Contains(const std::string& key) const;
    // Nothing queued, running or waiting to be taken.
    bool IsIdle() const;
    // Requests queued, running or completed but not yet taken.
    size_t PendingCount() const;

  private:
    void WorkerLoop();

    PrepareFunction m_prepare;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<TexturePreparationRequest> m_queued;
    std::deque<TexturePreparationResult> m_completed;
    std::unordered_set<std::string> m_pendingKeys;
    size_t m_running = 0;
    bool m_stopping = false;
    std::vector<std::thread> m_workers;
};
}
