#include "crash_handler.h"

#include <engine/core/log/log.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h.
#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <exception>
#include <thread>
#include <typeinfo>
#endif

namespace me::platform::crash
{

#ifdef _WIN32
namespace
{
// What a crashing thread hands the reporter thread. The report is written there, not on the crashing
// thread, whose stack may be exhausted (a stack overflow) or corrupt.
struct CrashRequest
{
    EXCEPTION_POINTERS* pointers = nullptr;
    DWORD threadId = 0;
    std::string reason;
    std::filesystem::path report;
};

HANDLE g_requestEvent = nullptr;
HANDLE g_doneEvent = nullptr;
CrashRequest g_request;
// The first crash reports; any other thread that crashes meanwhile waits for it and the process ends.
std::atomic<bool> g_reporting{false};
// A crash while the log or DbgHelp's own lock is held by the crashed thread could stall the reporter;
// the crashed thread ends the process after this long whatever happened.
constexpr DWORD kReportTimeoutMs = 30000;

std::string Narrow(const wchar_t* text)
{
    if (text == nullptr)
    {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(std::max(length - 1, 0)), '\0');
    if (length > 1)
    {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    }
    return result;
}

std::string Hex(uint64_t value)
{
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

std::string DescribeException(const EXCEPTION_RECORD& record)
{
    const DWORD code = record.ExceptionCode;
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    {
        const char* what = "touched";
        if (record.NumberParameters >= 2)
        {
            what = record.ExceptionInformation[0] == 0 ? "read" : record.ExceptionInformation[0] == 1 ? "wrote" : "executed";
        }
        const uint64_t address = record.NumberParameters >= 2 ? record.ExceptionInformation[1] : 0;
        return std::string(code == EXCEPTION_ACCESS_VIOLATION ? "Access violation: " : "In-page error: ") + what + " " + Hex(address);
    }
    case EXCEPTION_STACK_OVERFLOW:
        return "Stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "Integer division by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "Illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "Privileged instruction";
    case EXCEPTION_BREAKPOINT:
        return "Breakpoint";
    case 0xE06D7363:
        return "C++ exception nobody caught";
    case 0xC0000409:
        return "Stack buffer overrun or fast fail (__fastfail)";
    default:
        return "Exception " + Hex(code);
    }
}

void InitializeSymbols(HANDLE process)
{
    static bool initialized = false;
    if (initialized)
    {
        return;
    }
    initialized = true;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    // The program's folder first: its PDB and the DLLs' sit there.
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::string folder = std::filesystem::path(exe).parent_path().string();
    SymInitialize(process, folder.c_str(), TRUE);
}

std::string WalkStack(HANDLE process, HANDLE thread, CONTEXT context)
{
    InitializeSymbols(process);
    STACKFRAME64 frame{};
    DWORD machine = 0;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    std::ostringstream text;
    alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + 1024] = {};
    for (int depth = 0; depth < 96; ++depth)
    {
        if (!StackWalk64(machine, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0)
        {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        const char* moduleName = SymGetModuleInfo64(process, address, &module) ? module.ModuleName : "?";

        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 1024;
        DWORD64 displacement = 0;
        text << "  #" << depth << " " << moduleName << "!";
        if (SymFromAddr(process, address, &displacement, symbol))
        {
            text << symbol->Name << "+" << Hex(displacement);
        }
        else
        {
            text << Hex(address - module.BaseOfImage) << " (no symbols)";
        }
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line))
        {
            text << "  " << line.FileName << ":" << line.LineNumber;
        }
        text << "\n";
    }
    return text.str();
}

std::string Timestamp()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char text[64];
    std::snprintf(
        text, sizeof(text), "%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return text;
}

// On the reporter thread: the dump, the text report, then the log line.
std::filesystem::path WriteReport(const CrashRequest& request)
{
    const HANDLE process = GetCurrentProcess();
    const DWORD processId = GetCurrentProcessId();
    std::error_code error;
    const std::filesystem::path folder = CrashFolder();
    std::filesystem::create_directories(folder, error);
    const std::string stem = "miniengine_" + Timestamp() + "_" + std::to_string(processId);
    const std::filesystem::path dumpPath = folder / (stem + ".dmp");
    const std::filesystem::path textPath = folder / (stem + ".txt");

    const HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, request.threadId);
    const HANDLE dumpFile = CreateFileW(dumpPath.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool dumped = false;
    if (dumpFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION exception{};
        exception.ThreadId = request.threadId;
        exception.ExceptionPointers = request.pointers;
        exception.ClientPointers = FALSE;
        const auto type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpScanMemory);
        dumped = MiniDumpWriteDump(process, processId, dumpFile, type, request.pointers != nullptr ? &exception : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(dumpFile);
    }

    std::ofstream text(textPath, std::ios::binary);
    text << "MiniEngine crash report\n";
    text << "Reason: " << request.reason << "\n";
    if (request.pointers != nullptr && request.pointers->ExceptionRecord != nullptr && request.pointers->ExceptionRecord->ExceptionCode != 0xE0000001)
    {
        text << "Exception: " << DescribeException(*request.pointers->ExceptionRecord) << " at "
             << Hex(reinterpret_cast<uint64_t>(request.pointers->ExceptionRecord->ExceptionAddress)) << "\n";
    }
    text << "Process " << processId << ", thread " << request.threadId << "\n";
    text << "Command line: " << Narrow(GetCommandLineW()) << "\n";
    text << "Minidump: " << (dumped ? dumpPath.string() : std::string("(could not be written)")) << "\n\n";
    text << "Stack of the crashing thread:\n";
    if (request.pointers != nullptr && request.pointers->ContextRecord != nullptr)
    {
        text << WalkStack(process, thread != nullptr ? thread : GetCurrentThread(), *request.pointers->ContextRecord);
    }
    text.flush();
    text << "\nThe log's last lines:\n";
    for (const Log::RecentLine& line : Log::RecentLines(300))
    {
        text << line.text << "\n";
    }
    text.close();
    if (thread != nullptr)
    {
        CloseHandle(thread);
    }
    LOG_ERROR("Crash: {}. Report: {}", request.reason, textPath.string());
    if (const auto logger = spdlog::default_logger())
    {
        logger->flush();
    }
    return textPath;
}

void ReporterThread()
{
    for (;;)
    {
        WaitForSingleObject(g_requestEvent, INFINITE);
        g_request.report = WriteReport(g_request);
        SetEvent(g_doneEvent);
    }
}

// On the crashing thread: hands the crash over and waits for the report.
void ReportFromCrashingThread(EXCEPTION_POINTERS* pointers, std::string reason)
{
    if (g_reporting.exchange(true))
    {
        // Another thread is reporting; this one waits for the process to end.
        WaitForSingleObject(g_doneEvent, kReportTimeoutMs);
        return;
    }
    g_request.pointers = pointers;
    g_request.threadId = GetCurrentThreadId();
    g_request.reason = std::move(reason);
    SetEvent(g_requestEvent);
    WaitForSingleObject(g_doneEvent, kReportTimeoutMs);
}

// abort, std::terminate and the CRT's handlers: no exception record of their own, so a context of
// where they were called stands in.
[[noreturn]] void ReportHereAndExit(const std::string& reason)
{
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xE0000001; // the engine's own: not a hardware exception
#if defined(_M_X64)
    record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
#endif
    EXCEPTION_POINTERS pointers{&record, &context};
    ReportFromCrashingThread(&pointers, reason);
    TerminateProcess(GetCurrentProcess(), 3);
    std::_Exit(3);
}

LONG WINAPI UnhandledExceptionFilterHandler(EXCEPTION_POINTERS* pointers)
{
    const std::string reason = pointers != nullptr && pointers->ExceptionRecord != nullptr
                                   ? DescribeException(*pointers->ExceptionRecord)
                                   : std::string("Unhandled exception");
    ReportFromCrashingThread(pointers, reason);
    return EXCEPTION_EXECUTE_HANDLER;
}

void TerminateHandler()
{
    std::string reason = "std::terminate";
    if (const std::exception_ptr current = std::current_exception())
    {
        try
        {
            std::rethrow_exception(current);
        }
        catch (const std::exception& error)
        {
            reason += std::string(": uncaught ") + typeid(error).name() + ": " + error.what();
        }
        catch (...)
        {
            reason += ": uncaught exception of an unknown type";
        }
    }
    ReportHereAndExit(reason);
}

void AbortSignalHandler(int)
{
    ReportHereAndExit("abort()");
}

void PureCallHandler()
{
    ReportHereAndExit("Pure virtual function call");
}

void InvalidParameterHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t)
{
    ReportHereAndExit(
        "Invalid parameter to a CRT function" + (function != nullptr ? " in " + Narrow(function) : std::string()) +
        (file != nullptr ? " (" + Narrow(file) + ":" + std::to_string(line) + ")" : std::string()) +
        (expression != nullptr ? ": " + Narrow(expression) : std::string()));
}
}

void Install()
{
    static bool installed = false;
    if (installed)
    {
        return;
    }
    installed = true;
    g_requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread(ReporterThread).detach();
    SetUnhandledExceptionFilter(UnhandledExceptionFilterHandler);
    std::set_terminate(TerminateHandler);
    std::signal(SIGABRT, AbortSignalHandler);
    _set_purecall_handler(PureCallHandler);
    _set_invalid_parameter_handler(InvalidParameterHandler);
    // abort() reports through the signal; no "abort() has been called" box, no WER report of its own.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // Room for the filter to run on a thread whose stack overflowed (this thread; the others hand
    // over to the reporter at once).
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
}

void Reassert()
{
    Install();
    SetUnhandledExceptionFilter(UnhandledExceptionFilterHandler);
    std::set_terminate(TerminateHandler);
    std::signal(SIGABRT, AbortSignalHandler);
}

std::filesystem::path CrashFolder()
{
    wchar_t* localAppData = nullptr;
    size_t length = 0;
    std::filesystem::path base;
    if (_wdupenv_s(&localAppData, &length, L"LOCALAPPDATA") == 0 && localAppData != nullptr)
    {
        base = localAppData;
        free(localAppData);
    }
    else
    {
        base = std::filesystem::temp_directory_path();
    }
    return base / "MiniEngine" / "crashes";
}

std::filesystem::path WriteReportForCurrentThread(const char* reason)
{
    Install();
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xE0000001;
    EXCEPTION_POINTERS pointers{&record, &context};
    g_reporting = true;
    ResetEvent(g_doneEvent);
    g_request.pointers = &pointers;
    g_request.threadId = GetCurrentThreadId();
    g_request.reason = reason;
    SetEvent(g_requestEvent);
    WaitForSingleObject(g_doneEvent, kReportTimeoutMs);
    ResetEvent(g_doneEvent);
    g_reporting = false;
    return g_request.report;
}
#else
void Install()
{
}

void Reassert()
{
}

std::filesystem::path CrashFolder()
{
    return std::filesystem::temp_directory_path() / "miniengine_crashes";
}

std::filesystem::path WriteReportForCurrentThread(const char*)
{
    return {};
}
#endif
}
