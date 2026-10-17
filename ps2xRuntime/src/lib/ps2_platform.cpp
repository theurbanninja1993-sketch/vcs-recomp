#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#endif

#include "runtime/ps2_platform.h"

#include <thread>
#if !defined(_WIN32)
#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstring>
#include "ThreadNaming.h"

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <system_error>

std::filesystem::path ps2ExecutableDirectory()
{
    std::error_code ec;
#if defined(_WIN32)
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length != 0 && length < std::size(path))
        return std::filesystem::path(path).parent_path();
    return std::filesystem::current_path(ec);
#elif defined(__linux__)
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path(ec) : exe.parent_path();
#else
    return std::filesystem::current_path(ec);
#endif
}

namespace
{
    std::thread::id g_hostThread;
}

void ps2SetHostThread()
{
    g_hostThread = std::this_thread::get_id();
    // Name it too: the per-thread CPU report otherwise shows the frame loop as
    // an anonymous thread<tid>, which is indistinguishable from a driver thread.
    ThreadNaming::SetCurrentThreadName("MainThread");
}

bool ps2IsHostThread()
{
    return g_hostThread == std::this_thread::get_id();
}

std::vector<Ps2ThreadCpu> ps2ThreadCpuTicks()
{
    std::vector<Ps2ThreadCpu> out;
#if defined(_WIN32)
    const DWORD pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return out;
    // GetThreadDescription is Windows 10 1607+; resolve it at run time so older
    // systems simply report ids.
    using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static const auto s_getDescription = []() -> GetThreadDescriptionFn
    {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        return kernel32 != nullptr
                   ? reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(kernel32, "GetThreadDescription"))
                   : nullptr;
    }();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != pid)
                continue;
            // THREAD_QUERY_INFORMATION for the start address (fall back if denied).
            HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == nullptr)
                thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == nullptr)
                continue;
            FILETIME creation{}, exitTime{}, kernel{}, user{};
            if (GetThreadTimes(thread, &creation, &exitTime, &kernel, &user))
            {
                const auto ticksOf = [](const FILETIME &t)
                { return ((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 100000ull; };
                Ps2ThreadCpu cpu;
                cpu.id = entry.th32ThreadID;
                cpu.ticks = ticksOf(kernel) + ticksOf(user);
                // Which module owns the thread's entry point, whatever else is known
                // about it. A thread that named itself but belongs to neither us nor
                // a library we know is the case worth naming: on the owner's Windows
                // laptop one such thread held 58% of a core, and with no module there
                // was nothing to go on. It cost a release cycle to find that out.
                using NtQueryInformationThreadFn = LONG(WINAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
                static const auto s_query = []() -> NtQueryInformationThreadFn
                {
                    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
                    return ntdll != nullptr
                               ? reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread"))
                               : nullptr;
                }();
                void *start = nullptr;
                if (s_query != nullptr)
                    s_query(thread, 9u /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr);
                std::string module;
                if (start != nullptr)
                {
                    HMODULE owner = nullptr;
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCWSTR>(start), &owner) &&
                        owner != nullptr)
                    {
                        wchar_t name[MAX_PATH] = {};
                        if (GetModuleBaseNameW(GetCurrentProcess(), owner, name, MAX_PATH) > 0)
                        {
                            std::wstring wide(name);
                            module.assign(wide.begin(), wide.end());
                        }
                    }
                }
                if (s_getDescription != nullptr)
                {
                    PWSTR description = nullptr;
                    if (SUCCEEDED(s_getDescription(thread, &description)) && description != nullptr)
                    {
                        std::wstring wide(description);
                        LocalFree(description);
                        cpu.label.assign(wide.begin(), wide.end());
                    }
                }
                // "module:what it calls itself", or "module:id" when it does not name
                // itself. We name every thread we create, so a module we do not know
                // is the OS, an injected hook, the audio device or the GPU driver.
                if (cpu.label.empty())
                    cpu.label = module.empty() ? ("thread" + std::to_string(cpu.id)) : (module + ":" + std::to_string(cpu.id));
                else if (!module.empty())
                    cpu.label = module + ":" + cpu.label;
                out.push_back(std::move(cpu));
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
#else
    DIR *dir = opendir("/proc/self/task");
    if (dir == nullptr)
        return out;
    while (dirent *entry = readdir(dir))
    {
        if (entry->d_name[0] == '.')
            continue;
        const std::string base = std::string("/proc/self/task/") + entry->d_name;
        Ps2ThreadCpu cpu;
        cpu.id = std::strtoull(entry->d_name, nullptr, 10);
        char name[64] = {};
        if (FILE *f = std::fopen((base + "/comm").c_str(), "r"))
        {
            if (std::fgets(name, sizeof(name), f))
                name[std::strcspn(name, "\n")] = 0;
            std::fclose(f);
        }
        cpu.label = name;
        if (FILE *f = std::fopen((base + "/stat").c_str(), "r"))
        {
            char buffer[1024] = {};
            const size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
            std::fclose(f);
            buffer[n] = 0;
            const char *p = std::strrchr(buffer, ')');
            unsigned long long utime = 0, stime = 0;
            if (p != nullptr &&
                std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) == 2)
                cpu.ticks = utime + stime;
        }
        out.push_back(std::move(cpu));
    }
    closedir(dir);
#endif
    return out;
}

void ps2RaiseCurrentThreadPriority()
{
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#else
    // Lowering the nice value needs privileges; if it fails the thread simply
    // keeps the default, which is what happens today anyway.
    setpriority(PRIO_PROCESS, 0, -5);
#endif
}

size_t ps2ProcessResidentMb()
{
#if defined(_WIN32)
    // Resolved at run time: mingw's headers alias GetProcessMemoryInfo to the
    // K32 form, which links a static stub into the executable instead of
    // importing psapi, and a stub in a shipped binary is not worth the risk.
    using GetProcessMemoryInfoFn = BOOL(WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static const GetProcessMemoryInfoFn s_get = []() -> GetProcessMemoryInfoFn
    {
        HMODULE psapi = LoadLibraryW(L"psapi.dll");
        return psapi != nullptr
                   ? reinterpret_cast<GetProcessMemoryInfoFn>(GetProcAddress(psapi, "GetProcessMemoryInfo"))
                   : nullptr;
    }();
    if (s_get != nullptr)
    {
        PROCESS_MEMORY_COUNTERS counters{};
        if (s_get(GetCurrentProcess(), &counters, sizeof(counters)))
            return counters.WorkingSetSize / (1024u * 1024u);
    }
    return 0u;
#else
    FILE *file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr)
        return 0u;
    long total = 0, resident = 0;
    const int read = std::fscanf(file, "%ld %ld", &total, &resident);
    std::fclose(file);
    if (read != 2)
        return 0u;
    const long pageKb = sysconf(_SC_PAGESIZE) / 1024;
    return static_cast<size_t>(resident) * static_cast<size_t>(pageKb > 0 ? pageKb : 4) / 1024u;
#endif
}

std::filesystem::path ps2ConfigDirectory()
{
    static const std::filesystem::path s_dir = []()
    {
        std::error_code ec;
        const bool portable = std::getenv("PS2X_PORTABLE") != nullptr ||
                              std::filesystem::exists(ps2ExecutableDirectory() / "portable.txt", ec);
        if (portable)
            return ps2ExecutableDirectory() / "config";
        if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return std::filesystem::path(xdg) / "ps2recomp";
        if (const char *appData = std::getenv("APPDATA"); appData && *appData)
            return std::filesystem::path(appData) / "ps2recomp";
        if (const char *home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".config" / "ps2recomp";
        return std::filesystem::current_path() / "ps2recomp";
    }();
    std::error_code ec;
    std::filesystem::create_directories(s_dir, ec);
    return s_dir;
}

void ps2OpenFolder(const std::string &path)
{
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#if defined(__APPLE__)
    const std::string command = "open \"" + path + "\" &";
#else
    const std::string command = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
#endif
    if (std::system(command.c_str()) != 0)
        std::cerr << "could not open " << path << std::endl;
#endif
}

void ps2ShowFatalError(const std::string &message)
{
    std::cerr << message << std::endl;
#if defined(_WIN32)
    const int length = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, wide.data(), length);
    MessageBoxW(nullptr, wide.c_str(), L"PS2Recomp", MB_OK | MB_ICONERROR);
#endif
}

#if defined(_WIN32)
namespace ThreadNaming
{
    void SetCurrentThreadNameWin32(std::string_view name)
    {
        using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
        HMODULE kernel32 = ::GetModuleHandleW(L"Kernel32.dll");
        auto setThreadDescription =
            reinterpret_cast<SetThreadDescriptionFn>(reinterpret_cast<void *>(::GetProcAddress(kernel32, "SetThreadDescription")));
        if (setThreadDescription)
        {
            std::wstring wname(name.begin(), name.end());
            setThreadDescription(::GetCurrentThread(), wname.c_str());
        }
    }
}
#endif

#if defined(_WIN32)
// Laptops with two GPUs - an Optimus one, or an AMD PowerXpress one - run an
// OpenGL process on the integrated GPU unless the executable asks otherwise, and
// the GPU renderer wants the discrete one. These two exported variables are how an
// application asks. Without them the player has to find the vendor's control panel
// and add the game to it by hand, which is not something a player should have to
// know about. (NVIDIA's Optimus documentation names the first; the second is AMD's
// equivalent and is harmless where it does not apply.)
extern "C"
{
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
