#include <SDL3/SDL_main.h>
#include <engine/application/editor_application.h>
#include <engine/core/log/log.h>

#include <exception>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

int main(int argc, char** argv)
{
    Log::Init();
    LOG_INFO("MiniEngine starting");

    try
    {
        EditorApplication::PrintDependencyLinkStatus();
        EditorApplication application(EditorApplication::ParseArgs(argc, argv));
        const int exitCode = application.Run();
        LOG_INFO("MiniEngine shutting down");
        return exitCode;
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("Unhandled exception: {}", error.what());
        return 1;
    }
}
