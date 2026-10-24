// A local achievement set: the port's own, evaluated against the game's memory.
//
// Why local, and why rcheevos: docs/vcs-handoff.md, open work item 16. The short
// version is that RetroAchievements excludes recompilations from standalone sets,
// so this is our own set in their condition format - which keeps it portable if
// that rule ever changes.
//
// The set file is read from, in order:
//
//   $PS2X_ACHIEVEMENTS            (so a set can be tried without installing it)
//   <the folder the port runs from>/achievements.toml
//   <the config folder>/achievements.toml
//
// and looks like this:
//
//   [set]
//   name = "Grand Theft Auto: Vice City Stories"
//   version = 1
//
//   [achievement]
//   id = 1
//   title = Soldier
//   description = Complete "Soldier".
//   category = Story
//   points = 10
//   trigger = 0xH...          # a RetroAchievements condition string
//
// `trigger` is parsed by rcheevos and evaluated against the guest's memory, so
// the condition language is theirs, not ours: 0xH1234 is a 16-bit read at that
// address, `=` and `>=` compare, `0xH1234=100.100.` needs a hundred hits, and so
// on. A block with no trigger is listed but never unlocks - which is how the set
// keeps its shape while an address is still being hunted.

#include "runtime/ps2_achievements.h"

#include "runtime/ps2_platform.h"
#include "runtime/ps2_memory.h" // PS2_RAM_MASK: the guest's own wrap
#include "runtime/ps2_host_settings.h" // the .pnach switch, for the cheat rule
#include "runtime/ps2_mods.h"          // and whether any enabled mod patches memory

#include <raylib.h>

#include "rc_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct Achievement
    {
        uint32_t id = 0;
        std::string title, description, category, trigger;
        int points = 0;
    };

    // One unlock banner, with the time it appeared so it can fade.
    struct Toast
    {
        std::string title;
        int points = 0;
        double at = 0.0;
    };

    std::vector<Achievement> s_achievements;
    std::vector<uint32_t> s_unlocked; // in the order they were earned
    std::vector<Toast> s_toasts;
    std::string s_setPath, s_setName;
    bool s_loaded = false;
    bool s_ready = false;
    uint8_t *s_rdram = nullptr;
    rc_runtime_t s_runtime{};
    double s_savedAt = 0.0;
    uint32_t s_cheatFlag = 0; // a guest address the set names, or 0 for none
    long s_peeks = 0;   // how many times the evaluator has read the guest's memory
    long s_events = 0;  // how many events the evaluator has raised
    bool s_cheatBlocked = false; // an unlock was refused because of a cheat

    // Where the two little files live: beside the settings, so a portable folder
    // keeps its achievements with everything else it keeps.
    std::filesystem::path unlockedPath() { return ps2ConfigDirectory() / "achievements.unlocked"; }
    std::filesystem::path progressPath() { return ps2ConfigDirectory() / "achievements.state"; }

    std::string trim(const std::string &text)
    {
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    // `key = value`, with the quotes a TOML-ish file may carry stripped, and
    // anything from a `#` on ignored - the same shape the port's other files use.
    bool splitKeyValue(const std::string &line, std::string &key, std::string &value)
    {
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            return false;
        key = trim(line.substr(0, equals));
        value = trim(line.substr(equals + 1));
        const size_t comment = value.find(" #");
        if (comment != std::string::npos)
            value = trim(value.substr(0, comment));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        return !key.empty();
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    void writeFile(const std::filesystem::path &path, const std::string &text)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out)
            out << text;
    }

    bool isUnlocked(uint32_t id)
    {
        return std::find(s_unlocked.begin(), s_unlocked.end(), id) != s_unlocked.end();
    }

    // The blob is stamped, so one written by a different build or a different set
    // is ignored rather than restored into triggers that then never evaluate.
    constexpr char kProgressMagic[8] = {'P', 'S', '2', 'X', 'A', 'C', 'H', '1'};

    // The two files: which achievements are unlocked (ours, so the list is right
    // even if the evaluator's own state is lost), and rcheevos' progress blob
    // (theirs, which is where hit counts live so a "100 of them" condition can
    // survive a session).
    void saveState()
    {
        std::string text;
        for (const uint32_t id : s_unlocked)
            text += std::to_string(id) + "\n";
        writeFile(unlockedPath(), text);

        if (s_ready)
        {
            const uint32_t size = rc_runtime_progress_size(&s_runtime, nullptr);
            if (size > 0)
            {
                std::vector<uint8_t> buffer(size);
                if (rc_runtime_serialize_progress_sized(buffer.data(), size, &s_runtime, nullptr) == 0)
                {
                    std::string stamped(kProgressMagic, sizeof kProgressMagic);
                    stamped.append(reinterpret_cast<const char *>(buffer.data()), size);
                    writeFile(progressPath(), stamped);
                }
            }
        }
        s_savedAt = GetTime();
    }

    void loadState()
    {
        std::istringstream in(readFile(unlockedPath()));
        std::string line;
        while (std::getline(in, line))
        {
            const std::string text = trim(line);
            if (!text.empty())
                s_unlocked.push_back(static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10)));
        }
        if (!s_ready)
            return;
        const std::string blob = readFile(progressPath());
        if (blob.size() <= sizeof kProgressMagic || std::memcmp(blob.data(), kProgressMagic, sizeof kProgressMagic) != 0)
            return; // none, or one this build does not understand
        rc_runtime_deserialize_progress_sized(&s_runtime,
                                              reinterpret_cast<const uint8_t *>(blob.data()) + sizeof kProgressMagic,
                                              static_cast<uint32_t>(blob.size() - sizeof kProgressMagic), nullptr);
    }

    void parseSet(const std::string &text)
    {
        std::istringstream in(text);
        std::string line;
        Achievement current;
        bool inAchievement = false;

        const auto flush = [&]
        {
            if (inAchievement && current.id != 0 && !current.title.empty())
                s_achievements.push_back(current);
            current = Achievement{};
        };

        while (std::getline(in, line))
        {
            const std::string body = trim(line);
            if (body.empty() || body[0] == '#')
                continue;
            if (body.front() == '[' && body.back() == ']')
            {
                const std::string section = trim(body.substr(1, body.size() - 2));
                if (section == "achievement")
                {
                    flush();
                    inAchievement = true;
                }
                else
                {
                    flush();
                    inAchievement = false;
                }
                continue;
            }
            std::string key, value;
            if (!splitKeyValue(body, key, value))
                continue;
            if (!inAchievement)
            {
                if (key == "name")
                    s_setName = value;
                else if (key == "cheat_flag")
                    s_cheatFlag = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
                continue;
            }
            if (key == "id")
                current.id = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
            else if (key == "title")
                current.title = value;
            else if (key == "description")
                current.description = value;
            else if (key == "category")
                current.category = value;
            else if (key == "points")
                current.points = std::atoi(value.c_str());
            else if (key == "trigger")
                current.trigger = value;
        }
        flush();
    }

    // rcheevos reads the guest's memory through this, a byte at a time, wrapped
    // the way the hardware wraps it. It may ask for an address it dereferenced
    // itself (a pointer chain), which is why the mask is applied here and not to
    // the condition.
    uint32_t RC_CCONV readGuestMemory(uint32_t address, uint8_t *buffer, uint32_t numBytes, void *ud)
    {
        const auto *rdram = static_cast<const uint8_t *>(ud);
        if (rdram == nullptr || buffer == nullptr || numBytes == 0)
            return 0;
        ++s_peeks;
        for (uint32_t i = 0; i < numBytes; ++i)
            buffer[i] = rdram[(address + i) & PS2_RAM_MASK];
        return numBytes;
    }

    bool cheating();       // defined below, and used by the unlock handler
    void grantCapstones(); // likewise

    void RC_CCONV onRuntimeEvent(const rc_runtime_event_t *event)
    {
        if (event == nullptr)
            return;
        ++s_events;
        // PS2X_ACHIEVEMENTS_DEBUG=1 prints every state change, which is how a
        // condition that is not behaving is told apart from one whose address is
        // wrong: an ACTIVATED event means the condition parsed and is being read.
        if (std::getenv("PS2X_ACHIEVEMENTS_DEBUG") != nullptr)
            std::fprintf(stderr, "[achievements] event type=%d id=%u value=%d\n", event->type, event->id,
                         event->value);
        if (event->type != RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED)
            return;
        if (cheating())
        {
            // Evaluated, but not earned: the condition is still met, and the list
            // still shows it, which is how a player finds out why nothing is
            // unlocking rather than wondering whether the set is broken.
            s_cheatBlocked = true;
            return;
        }
        if (isUnlocked(event->id))
            return;
        s_unlocked.push_back(event->id);
        for (const Achievement &achievement : s_achievements)
            if (achievement.id == event->id)
            {
                s_toasts.push_back({achievement.title, achievement.points, GetTime()});
                std::fprintf(stderr, "[achievements] unlocked: %s\n", achievement.title.c_str());
                break;
            }
        // Three at once is already a lot on screen; the oldest goes.
        while (s_toasts.size() > 3)
            s_toasts.erase(s_toasts.begin());
        grantCapstones();
        saveState();
    }

    // Whether the player is cheating, in which case nothing unlocks.
    //
    // Two ways in. The port's own .pnach patches are a switch the port holds, so
    // that one is certain. The game's own cheat codes - button sequences, which
    // the port cannot see the meaning of - are a flag in the game's memory, and
    // the set names its address in `cheat_flag` once it has been found; see the
    // handoff for the recipe (a weather cheat and two dumps isolates it, because
    // weather changes almost nothing else).
    bool cheating()
    {
        // The .pnach switch is on by default and only means "apply patches from the
        // mods I enabled" - most mods have none. What matters is whether an enabled
        // mod actually writes to memory.
        if (ps2HostSettings().pnachCheats)
            for (const PS2ModInfo &mod : ps2Mods())
                if (mod.enabled && mod.patches > 0)
                    return true;
        if (s_cheatFlag != 0 && s_rdram != nullptr)
            return s_rdram[s_cheatFlag & PS2_RAM_MASK] != 0;
        return false;
    }

    const Achievement *findAchievement(uint32_t id)
    {
        for (const Achievement &achievement : s_achievements)
            if (achievement.id == id)
                return &achievement;
        return nullptr;
    }

    // rcheevos will not evaluate a trigger whose addresses the client has not
    // said are readable - it is how an emulator tells it that a memory region is
    // not loaded yet. Here the whole 32 MB is mapped, so every address is fine.
    int RC_CCONV everyAddressIsReadable(uint32_t /*address*/) { return 1; }

    // A capstone over the other achievements cannot be a condition on memory, so
    // the port grants it: a trigger-less achievement in the Capstone category
    // unlocks once every achievement that does have a condition is unlocked.
    void grantCapstones()
    {
        // Every achievement, not just the ones with a condition: while the set is
        // still being authored, most of them have none, and a capstone that fired
        // as soon as the first one was earned would be a lie about the set.
        bool anyToEarn = false;
        bool allUnlocked = true;
        for (const Achievement &achievement : s_achievements)
        {
            if (achievement.category == "Capstone")
                continue;
            anyToEarn = true;
            if (!isUnlocked(achievement.id))
                allUnlocked = false;
        }
        if (!anyToEarn || !allUnlocked)
            return;
        for (const Achievement &achievement : s_achievements)
            if (achievement.trigger.empty() && achievement.category == "Capstone" && !isUnlocked(achievement.id))
            {
                s_unlocked.push_back(achievement.id);
                s_toasts.push_back({achievement.title, achievement.points, GetTime()});
                std::fprintf(stderr, "[achievements] unlocked: %s\n", achievement.title.c_str());
            }
    }
}

const std::string &ps2AchievementsSetPath() { return s_setPath; }
const std::string &ps2AchievementsSetName() { return s_setName; }
bool ps2AchievementsLoaded() { return s_loaded; }
int ps2AchievementsTotal() { return static_cast<int>(s_achievements.size()); }
int ps2AchievementsUnlocked() { return static_cast<int>(s_unlocked.size()); }

void ps2AchievementsInit(uint8_t *rdram)
{
    if (s_loaded)
    {
        s_rdram = rdram;
        return;
    }
    s_loaded = true;
    s_rdram = rdram;

    const char *fromEnvironment = std::getenv("PS2X_ACHIEVEMENTS");
    std::vector<std::filesystem::path> candidates;
    if (fromEnvironment != nullptr && fromEnvironment[0] != '\0')
        candidates.emplace_back(fromEnvironment);
    candidates.push_back(ps2ExecutableDirectory() / "achievements.toml");
    candidates.push_back(ps2ConfigDirectory() / "achievements.toml");

    std::string text;
    for (const std::filesystem::path &candidate : candidates)
    {
        text = readFile(candidate);
        if (!text.empty())
        {
            s_setPath = candidate.string();
            break;
        }
    }
    if (text.empty())
    {
        std::fprintf(stderr, "[achievements] no set found (looked for %s)\n", candidates.front().string().c_str());
        return;
    }

    parseSet(text);
    if (s_achievements.empty())
    {
        std::fprintf(stderr, "[achievements] %s holds no achievements\n", s_setPath.c_str());
        return;
    }

    rc_runtime_init(&s_runtime);
    size_t activated = 0;
    for (const Achievement &achievement : s_achievements)
    {
        if (achievement.trigger.empty())
            continue; // listed, but no condition yet - see the file's own comments
        if (rc_runtime_activate_achievement(&s_runtime, achievement.id, achievement.trigger.c_str(), nullptr, 0) == 0)
            ++activated;
        else
            std::fprintf(stderr, "[achievements] could not parse the trigger for %s: %s\n",
                         achievement.title.c_str(), achievement.trigger.c_str());
    }
    s_ready = true;
    loadState();
    // Validation comes last, and is what makes a trigger evaluable at all: done
    // before the restore, a blob's memref state can leave every trigger sitting
    // there being read and never tested - which looks exactly like a set whose
    // addresses are all wrong.
    rc_runtime_validate_addresses(&s_runtime, onRuntimeEvent, everyAddressIsReadable);
    grantCapstones(); // everything already earned: the capstone is owed
    std::fprintf(stderr, "[achievements] %s: %zu achievements, %zu with a condition, %zu unlocked\n",
                 s_setName.empty() ? s_setPath.c_str() : s_setName.c_str(), s_achievements.size(), activated,
                 s_unlocked.size());
}

// PS2X_ACH_TRACE=0x004CD18C:f,0x0052FC74 prints each watched address whenever its
// value changes, with the game frame it changed on: a byte, or a float with the ":f"
// suffix. How a condition's address is checked against a real session, one frame at
// a time, instead of by two dumps.
static void traceWatchedAddresses(const uint8_t *rdram)
{
    struct Watch
    {
        uint32_t address;
        bool isFloat;
        uint32_t last;
    };
    static bool parsed = false;
    static std::vector<Watch> watches;
    static unsigned long long frame = 0;
    ++frame;
    if (!parsed)
    {
        parsed = true;
        const char *spec = std::getenv("PS2X_ACH_TRACE");
        std::string text = spec != nullptr ? spec : "";
        size_t start = 0;
        while (start < text.size())
        {
            size_t comma = text.find(',', start);
            if (comma == std::string::npos)
                comma = text.size();
            std::string item = text.substr(start, comma - start);
            start = comma + 1;
            Watch watch{0, false, 0xFFFFFFFFu};
            if (item.size() > 2 && item.compare(item.size() - 2, 2, ":f") == 0)
            {
                watch.isFloat = true;
                item.resize(item.size() - 2);
            }
            watch.address = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)) & PS2_RAM_MASK;
            watches.push_back(watch);
        }
    }
    for (Watch &watch : watches)
    {
        uint32_t bits = 0;
        if (watch.isFloat)
            std::memcpy(&bits, rdram + watch.address, sizeof bits);
        else
            bits = rdram[watch.address];
        if (bits == watch.last)
            continue;
        watch.last = bits;
        if (watch.isFloat)
        {
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof value);
            std::fprintf(stderr, "[ach-trace] frame %llu  0x%08X = %g\n", frame, watch.address, value);
        }
        else
        {
            std::fprintf(stderr, "[ach-trace] frame %llu  0x%08X = %u\n", frame, watch.address, bits);
        }
    }
}

void ps2AchievementsFrame(uint8_t *rdram)
{
    if (!s_ready || rdram == nullptr)
        return;
    s_rdram = rdram;
    if (std::getenv("PS2X_ACH_TRACE") != nullptr)
        traceWatchedAddresses(rdram);
    rc_runtime_do_frame(&s_runtime, onRuntimeEvent, readGuestMemory, rdram, nullptr);

    // A set that never fires is either a set whose addresses are wrong or an engine
    // that is not evaluating at all, and the difference is worth being able to see:
    // no reads means nothing was even asked for.
    if (std::getenv("PS2X_ACHIEVEMENTS_DEBUG") != nullptr)
    {
        static double s_lastReport = 0.0;
        static bool s_reportedFirst = false;
        if (!s_reportedFirst)
        {
            s_reportedFirst = true;
            std::fprintf(stderr, "[achievements] debug: first frame evaluated, %ld events, %ld reads\n", s_events,
                         s_peeks);
        }
        if (GetTime() - s_lastReport >= 5.0)
        {
            s_lastReport = GetTime();
            uint32_t cutscene = 0;
            std::memcpy(&cutscene, rdram + (0x6F4D14u & PS2_RAM_MASK), sizeof cutscene);
            std::fprintf(stderr,
                         "[achievements] debug: %u triggers, %ld reads, %ld events; pause flag=%u cutscene flag=%u\n",
                         s_runtime.trigger_count, s_peeks, s_events, rdram[0x4CD2ACu & PS2_RAM_MASK], cutscene);
        }
    }

    // The blob holds hit counts, so it is worth keeping across a crash, not just
    // across a clean exit - but not worth writing every frame either.
    if (GetTime() - s_savedAt > 30.0)
        saveState();
}

bool ps2AchievementsCheating() { return cheating(); }
bool ps2AchievementsCheatBlocked() { return s_cheatBlocked; }

void ps2AchievementsReset()
{
    s_unlocked.clear();
    s_toasts.clear();
    if (s_ready)
        rc_runtime_reset(&s_runtime);
    std::error_code ec;
    std::filesystem::remove(unlockedPath(), ec);
    std::filesystem::remove(progressPath(), ec);
    std::fprintf(stderr, "[achievements] every unlock forgotten\n");
}

std::vector<Ps2AchievementView> ps2AchievementsView()
{
    std::vector<Ps2AchievementView> view;
    view.reserve(s_achievements.size());
    for (const Achievement &achievement : s_achievements)
    {
        Ps2AchievementView row;
        row.id = achievement.id;
        row.title = achievement.title;
        row.description = achievement.description;
        row.category = achievement.category;
        row.points = achievement.points;
        row.unlocked = isUnlocked(achievement.id);
        if (s_ready && !achievement.trigger.empty())
        {
            char buffer[64] = {};
            if (rc_runtime_format_achievement_measured(&s_runtime, achievement.id, buffer, sizeof buffer) != 0 &&
                buffer[0] != '\0')
            {
                row.hasProgress = true;
                row.progress = buffer;
            }
        }
        view.push_back(std::move(row));
    }
    return view;
}

void ps2AchievementsDrawOverlay()
{
    if (s_toasts.empty())
        return;

    const double now = GetTime();
    constexpr double kShown = 6.0;
    while (!s_toasts.empty() && now - s_toasts.front().at > kShown)
        s_toasts.erase(s_toasts.begin());
    if (s_toasts.empty())
        return;

    const int width = GetScreenWidth();
    int y = 64; // clear of the port's own fps overlay, which sits at 10,10
    for (const Toast &toast : s_toasts)
    {
        const double age = now - toast.at;
        if (std::getenv("PS2X_ACHIEVEMENTS_DEBUG") != nullptr && age < 0.1)
            std::fprintf(stderr, "[achievements] banner: %s (%.0f points)\n", toast.title.c_str(),
                         static_cast<double>(toast.points));
        const float alpha = static_cast<float>(std::min(1.0, std::max(0.0, (kShown - age) / 1.0)));
        const std::string heading = "ACHIEVEMENT UNLOCKED";
        const std::string body = toast.points > 0 ? toast.title + "   (" + std::to_string(toast.points) + ")"
                                                  : toast.title;
        const int headingWidth = MeasureText(heading.c_str(), 18);
        const int bodyWidth = MeasureText(body.c_str(), 24);
        const int boxWidth = std::max(headingWidth, bodyWidth) + 48;
        const int x = (width - boxWidth) / 2;

        DrawRectangleRounded(Rectangle{static_cast<float>(x), static_cast<float>(y), static_cast<float>(boxWidth), 78.0f},
                             0.18f, 6, Color{12, 14, 20, static_cast<unsigned char>(225 * alpha)});
        DrawText(heading.c_str(), x + (boxWidth - headingWidth) / 2, y + 12, 18,
                 Color{240, 200, 90, static_cast<unsigned char>(255 * alpha)});
        DrawText(body.c_str(), x + (boxWidth - bodyWidth) / 2, y + 38, 24,
                 Color{236, 240, 248, static_cast<unsigned char>(255 * alpha)});
        y += 92;
    }
}
