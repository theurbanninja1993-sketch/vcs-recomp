#pragma once

#include <string_view>
#include <string>

#if defined(_WIN32)
namespace ThreadNaming
{
    // ps2_mods.cpp (keeps <windows.h> out of files using raylib).
    void SetCurrentThreadNameWin32(std::string_view name);
}
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace ThreadNaming
{
    inline void SetCurrentThreadName(std::string_view name)
    {
#if defined(_WIN32)
        SetCurrentThreadNameWin32(name);

#elif defined(__APPLE__)
        pthread_setname_np(name.data());

#elif defined(__linux__)
        pthread_setname_np(pthread_self(), name.data());
#endif
    }
}
