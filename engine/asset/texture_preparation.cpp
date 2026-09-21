#include "texture_preparation.h"

#include "compressed_texture_cache.h"

#include <engine/core/log/log.h>

#include <algorithm>
#include <chrono>
#include <exception>

namespace me
{

PreparedTexture PrepareTexture(
    const std::string& path,
    TextureUsage usage,
    bool compress,
    const std::filesystem::path& cacheDirectory)
{
    PreparedTexture prepared{};
    if (compress)
    {
        try
        {
            const auto start = std::chrono::steady_clock::now();
            CompressedTextureLoad load = LoadOrCompressTexture(path, usage, cacheDirectory);
            prepared.fromCache = load.cacheHit;
            prepared.compressSeconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            prepared.compressed = std::move(load.texture);
            return prepared;
        }
        catch (const std::exception& error)
        {
            LOG_ERROR("Could not compress texture '{}', uploading it uncompressed: {}", path, error.what());
        }
    }
    prepared.rgba = TextureLoader::LoadRGBA8(path);
    return prepared;
}

TexturePreparationQueue::TexturePreparationQueue(PrepareFunction prepare, uint32_t workerCount)
    : m_prepare(std::move(prepare))
{
    const uint32_t count = std::max(workerCount, 1u);
    m_workers.reserve(count);
    for (uint32_t worker = 0; worker < count; ++worker)
    {
        m_workers.emplace_back([this]()
                               {
                                   WorkerLoop();
                               });
    }
}

TexturePreparationQueue::~TexturePreparationQueue()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        for (const TexturePreparationRequest& request : m_queued)
        {
            m_pendingKeys.erase(request.key);
        }
        m_queued.clear();
    }
    m_wake.notify_all();
    for (std::thread& worker : m_workers)
    {
        worker.join();
    }
}

bool TexturePreparationQueue::Enqueue(TexturePreparationRequest request)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping || !m_pendingKeys.insert(request.key).second)
        {
            return false;
        }
        m_queued.push_back(std::move(request));
    }
    m_wake.notify_one();
    return true;
}

std::vector<TexturePreparationResult> TexturePreparationQueue::TakeCompleted(size_t maxResults)
{
    std::vector<TexturePreparationResult> results;
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_completed.empty() && results.size() < maxResults)
    {
        m_pendingKeys.erase(m_completed.front().key);
        results.push_back(std::move(m_completed.front()));
        m_completed.pop_front();
    }
    return results;
}

bool TexturePreparationQueue::Contains(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pendingKeys.count(key) != 0;
}

bool TexturePreparationQueue::IsIdle() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queued.empty() && m_running == 0 && m_completed.empty();
}

size_t TexturePreparationQueue::PendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pendingKeys.size();
}

void TexturePreparationQueue::WorkerLoop()
{
    for (;;)
    {
        TexturePreparationRequest request;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this]()
                        {
                            return m_stopping || !m_queued.empty();
                        });
            if (m_stopping)
            {
                return;
            }
            request = std::move(m_queued.front());
            m_queued.pop_front();
            ++m_running;
        }

        // The prepare function runs without the lock: it is the slow part, and the whole point is
        // that the frame loop can keep enqueuing and taking while it does.
        TexturePreparationResult result{};
        result.key = request.key;
        result.usage = request.usage;
        try
        {
            result.texture = m_prepare(request.path, request.usage);
        }
        catch (const std::exception& error)
        {
            result.error = error.what();
        }
        catch (...)
        {
            result.error = "unknown error";
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        --m_running;
        m_completed.push_back(std::move(result));
    }
}
}
