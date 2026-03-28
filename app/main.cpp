#include <SDL3/SDL_main.h>
#include <editor_application.h>
#include <log/log.h>

#include <exception>

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
