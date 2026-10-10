#pragma once

#include <filesystem>

namespace me::platform::crash
{

// Catches a crash in any thread (an access violation, an exception nobody caught, std::terminate,
// abort) and writes, before the process ends, a report next to the others in CrashFolder():
//   miniengine_<time>_<pid>.txt  what happened, the crashing thread's stack with function names, files
//                                and lines (DbgHelp, from the program's PDB), and the log's last lines
//   miniengine_<time>_<pid>.dmp  a minidump for a debugger
// The report's path also goes to the log. Windows only; elsewhere it does nothing.
// Call once, early in main.
void Install();
// Puts the handler back in front: RenderDoc (--renderdoc) and some drivers install their own unhandled
// exception filter when they load. Call after the graphics device is made.
void Reassert();

// %LOCALAPPDATA%/MiniEngine/crashes (shared by every checkout, like the texture cache).
std::filesystem::path CrashFolder();

// For tests: writes a report for the calling thread's stack as it is now, without crashing, and
// returns the text file's path.
std::filesystem::path WriteReportForCurrentThread(const char* reason);
}
