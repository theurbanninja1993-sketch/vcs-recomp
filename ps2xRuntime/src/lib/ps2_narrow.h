// A path as the narrow string the C file APIs want.
//
// std::filesystem::path::c_str() is wchar_t on Windows and char everywhere else, so
// passing a path straight to std::fopen is a type error on the Windows cross-build and
// perfectly fine on Linux, where the two are the same type. That is why this went
// unnoticed: the Windows build had never been compiled. The overloads below take either
// a path or a string, so a call site does not have to know which it has.
#pragma once

#include <filesystem>
#include <string>

namespace ps2x
{
    inline std::string narrow(const std::string &text)
    {
        return text;
    }

    inline std::string narrow(const std::filesystem::path &path)
    {
        return path.string();
    }
}
