#include <engine/asset/texture_preparation.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// A prepared texture whose one pixel identifies the request, so results can be checked.
PreparedTexture MakePrepared(const std::string& path)
{
    PreparedTexture prepared{};
    prepared.rgba.width = 1;
    prepared.rgba.height = 1;
    prepared.rgba.channelCount = 4;
    prepared.rgba.pixels = {static_cast<uint8_t>(path.size()), 0, 0, 255};
    return prepared;
}

TexturePreparationRequest Request(const std::string& key)
{
    return TexturePreparationRequest{key, key + ".png", TextureUsage::Color};
}

// Takes results until `count` have arrived or five seconds pass.
std::vector<TexturePreparationResult> TakeAll(TexturePreparationQueue& queue, size_t count)
{
    std::vector<TexturePreparationResult> results;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (results.size() < count && std::chrono::steady_clock::now() < deadline)
    {
        for (TexturePreparationResult& result : queue.TakeCompleted(16))
        {
            results.push_back(std::move(result));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return results;
}

void CompletesAndIsTakenOnce()
{
    TexturePreparationQueue queue([](const std::string& path, TextureUsage)
                                  {
                                      return MakePrepared(path);
                                  },
                                  2);
    Require(queue.Enqueue(Request("a")) && queue.Enqueue(Request("bb")) && queue.Enqueue(Request("ccc")), "fresh keys must be accepted");

    const std::vector<TexturePreparationResult> results = TakeAll(queue, 3);
    Require(results.size() == 3, "every request must complete");
    for (const TexturePreparationResult& result : results)
    {
        Require(result.texture.has_value(), "a successful request carries its texture");
        Require(result.texture->rgba.pixels[0] == result.key.size() + 4, "the texture must be the one prepared for its key");
    }
    Require(queue.IsIdle(), "a queue with everything taken is idle");
    Require(queue.TakeCompleted(16).empty(), "a result is taken only once");
}

void DuplicateKeyIsRefusedWhilePending()
{
    std::atomic<bool> release{false};
    TexturePreparationQueue queue(
        [&release](const std::string& path, TextureUsage)
        {
            while (!release)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return MakePrepared(path);
        },
        1);

    Require(queue.Enqueue(Request("a")), "the first request is accepted");
    Require(!queue.Enqueue(Request("a")), "the same key must not be prepared twice at once");
    Require(queue.Contains("a"), "a pending key is contained");
    release = true;
    Require(TakeAll(queue, 1).size() == 1, "the request completes once released");
    Require(!queue.Contains("a"), "a taken key is no longer contained");
    Require(queue.Enqueue(Request("a")), "a taken key may be requested again");
    Require(TakeAll(queue, 1).size() == 1, "and completes again");
}

void ThrowingPrepareBecomesAFailedResult()
{
    TexturePreparationQueue queue(
        [](const std::string&, TextureUsage) -> PreparedTexture
        {
            throw std::runtime_error("bad png");
        },
        1);
    queue.Enqueue(Request("broken"));

    const std::vector<TexturePreparationResult> results = TakeAll(queue, 1);
    Require(results.size() == 1, "a failed request still completes");
    Require(!results[0].texture.has_value(), "a failed request carries no texture");
    Require(results[0].error == "bad png", "a failed request carries the error, got '" + results[0].error + "'");
}

void NotIdleUntilTaken()
{
    TexturePreparationQueue queue([](const std::string& path, TextureUsage)
                                  {
                                      return MakePrepared(path);
                                  },
                                  1);
    queue.Enqueue(Request("a"));

    // The prepare function returns at once, so after this the result is finished and untaken.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Require(!queue.IsIdle(), "a result that has not been taken keeps the queue busy");
    Require(queue.PendingCount() == 1, "the untaken result is still pending");
    Require(TakeAll(queue, 1).size() == 1, "the result can be taken");
    Require(queue.IsIdle() && queue.PendingCount() == 0, "taking it leaves the queue idle");
}

void DestructionDiscardsQueuedWork()
{
    std::atomic<bool> release{false};
    std::atomic<int> calls{0};
    std::thread releaser;
    {
        TexturePreparationQueue queue(
            [&](const std::string& path, TextureUsage)
            {
                ++calls;
                while (!release)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return MakePrepared(path);
            },
            1);
        queue.Enqueue(Request("a"));
        queue.Enqueue(Request("b"));
        queue.Enqueue(Request("c"));
        while (calls == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        releaser = std::thread(
            [&release]()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                release = true;
            });
    }
    releaser.join();
    Require(calls == 1, "queued requests must be discarded on destruction, prepared " + std::to_string(calls.load()));
}
}

int main()
{
    try
    {
        CompletesAndIsTakenOnce();
        DuplicateKeyIsRefusedWhilePending();
        ThrowingPrepareBecomesAFailedResult();
        NotIdleUntilTaken();
        DestructionDiscardsQueuedWork();
    }
    catch (const std::exception& error)
    {
        std::cerr << "texture preparation tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "texture preparation tests passed\n";
    return 0;
}
