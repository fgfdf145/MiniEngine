#include <SDL3/SDL_main.h>
#include <engine/application/editor_application.h>
#include <engine/core/log/log.h>

#include <exception>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

#if defined(_WIN32)
// The Direct3D 12 Agility SDK (docs/design/2026-10-09-d3d12-backend-design.md): the runtime loads its
// D3D12Core.dll from the D3D12 folder next to the program (copied there by the build) when the
// program exports which version it was built against. Only the executable's exports count.
extern "C"
{
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif

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
