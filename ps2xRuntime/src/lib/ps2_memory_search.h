// A value search over the guest's 32 MB: the tool the achievement conditions are
// authored with.
//
// The loop is the one the F9 RAM dumps are for, run live instead of by hand. Find
// a number you can see in the game (a count, a level, a rating), then narrow it by
// what it does - it changed, it went up, it stayed the same - until one address is
// left. Live is the difference between an afternoon per achievement and a minute:
// you do the thing in the game and press a button, rather than dumping twice and
// diffing 32 MB.
//
// It also finds things that are not numbers on screen at all, which is most of
// them: dump, do the thing, dump, and diff the two.
//
// See docs/vcs-handoff.md for the conditions still to be found.

#include "runtime/ps2_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // How a candidate is compared with what it was. "Same" is the useful one for a
    // value that is already on screen; the others are for the things that are not.
    enum class Narrow
    {
        Changed,
        Unchanged,
        Increased,
        Decreased,
        Equals,
    };

    struct MemorySearch
    {
        int width = 2;                 // 0 = byte, 1 = half, 2 = word, 3 = float
        char valueText[32] = "";
        std::vector<uint32_t> candidates;
        std::vector<uint32_t> previous; // what each candidate held at the last step
        std::string note;
        bool full = false;
    };

    constexpr size_t kMaxCandidates = 4096;

    uint32_t readAs(const uint8_t *rdram, uint32_t address, int width)
    {
        const size_t at = address & PS2_RAM_MASK;
        switch (width)
        {
        case 0: return rdram[at];
        case 1:
        {
            uint16_t value = 0;
            std::memcpy(&value, rdram + at, sizeof value);
            return value;
        }
        default:
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + at, sizeof value);
            return value;
        }
        }
    }

    int widthBytes(int width) { return width == 0 ? 1 : (width == 1 ? 2 : 4); }

    // The typed value, as the bits the guest would hold: a float is compared as its
    // bits, so one path does all four widths.
    bool parseValue(const char *text, int width, uint32_t &out)
    {
        const std::string body(text);
        if (body.empty())
            return false;
        if (width == 3)
        {
            char *end = nullptr;
            const float asFloat = std::strtof(body.c_str(), &end);
            if (end == body.c_str())
                return false;
            std::memcpy(&out, &asFloat, sizeof out);
            return true;
        }
        char *end = nullptr;
        const unsigned long long value = std::strtoull(body.c_str(), &end, 0);
        if (end == body.c_str())
            return false;
        out = static_cast<uint32_t>(value);
        return true;
    }

    std::string formatValue(uint32_t raw, int width)
    {
        char buffer[64];
        if (width == 3)
        {
            float asFloat = 0.0f;
            std::memcpy(&asFloat, &raw, sizeof asFloat);
            std::snprintf(buffer, sizeof buffer, "%.4g", static_cast<double>(asFloat));
            return buffer;
        }
        std::snprintf(buffer, sizeof buffer, "%u  (0x%0*X)", raw, widthBytes(width) * 2, raw);
        return buffer;
    }

    // Every aligned position, compared against the typed value. Not aligned to the
    // width on purpose: a game is free to keep a 32-bit count on a two-byte
    // boundary, and missing it would look exactly like the value not being in
    // memory at all.
    void searchFor(const uint8_t *rdram, MemorySearch &search)
    {
        uint32_t wanted = 0;
        if (!parseValue(search.valueText, search.width, wanted))
        {
            search.note = "type a value to search for first";
            return;
        }
        const int step = widthBytes(search.width);
        search.candidates.clear();
        search.previous.clear();
        search.full = false;
        for (size_t at = 0; at + step <= PS2_RAM_SIZE; ++at)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + at, step);
            if (search.width == 0)
                value &= 0xFFu;
            else if (search.width == 1)
                value &= 0xFFFFu;
            if (value != wanted)
                continue;
            if (search.candidates.size() >= kMaxCandidates)
            {
                search.full = true;
                break;
            }
            search.candidates.push_back(static_cast<uint32_t>(at));
            search.previous.push_back(value);
        }
        search.note = search.candidates.empty()
                          ? "nothing holds that value"
                          : (std::to_string(search.candidates.size()) + " address(es)" +
                             (search.full ? " - too many to list, narrow it down" : ""));
    }

    void narrow(const uint8_t *rdram, MemorySearch &search, Narrow how)
    {
        if (search.candidates.empty())
        {
            search.note = "nothing to narrow yet";
            return;
        }
        uint32_t wanted = 0;
        if (how == Narrow::Equals && !parseValue(search.valueText, search.width, wanted))
        {
            search.note = "type a value to compare against first";
            return;
        }
        std::vector<uint32_t> keptAddresses;
        std::vector<uint32_t> keptValues;
        const size_t before = search.candidates.size();
        for (size_t i = 0; i < search.candidates.size(); ++i)
        {
            const uint32_t value = readAs(rdram, search.candidates[i], search.width);
            const uint32_t was = search.previous[i];
            bool keep = false;
            switch (how)
            {
            case Narrow::Changed: keep = value != was; break;
            case Narrow::Unchanged: keep = value == was; break;
            case Narrow::Increased: keep = value > was; break;
            case Narrow::Decreased: keep = value < was; break;
            case Narrow::Equals: keep = value == wanted; break;
            }
            if (!keep)
                continue;
            keptAddresses.push_back(search.candidates[i]);
            keptValues.push_back(value);
        }
        search.candidates = std::move(keptAddresses);
        search.previous = std::move(keptValues);
        search.note = std::to_string(search.candidates.size()) + " of " + std::to_string(before) + " left";
    }
}
