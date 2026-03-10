#include <log/log.h>
#include <vulkan/renderer.h>
#include <window/window.h>

#include <exception>

int main()
{
    Log::Init();
    LOG_INFO("MiniEngine starting");

    try
    {
        Window window(1280, 720, "MiniEngine");
        VulkanRenderer renderer(window);

        while (!window.ShouldClose())
        {
            window.PollEvents();
            renderer.DrawFrame();
        }
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("Unhandled exception: {}", error.what());
        return 1;
    }

    LOG_INFO("MiniEngine shutting down");

    return 0;
}
