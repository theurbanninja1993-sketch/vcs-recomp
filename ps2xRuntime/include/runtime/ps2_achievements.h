#pragma once

// A local achievement set: the port's own, evaluated against the game's memory.
//
// The evaluator is rcheevos (MIT), the same library RetroAchievements clients
// use, and the conditions are written in its format - so a set written here is
// portable if RetroAchievements ever accepts recompilations. It does not today
// (see docs/vcs-handoff.md, open work item 16), and this is the route the owner
// chose instead: our own set, our own file, no network and no account.
//
// The set is a file beside the port, unlocks live beside the settings, and
// nothing here opens a socket. The engine has no UI of its own beyond the unlock
// banner; the list is drawn by the settings panel, which reads it through
// ps2AchievementsView().

#include <cstdint>
#include <string>
#include <vector>

// One achievement, as the panel needs it.
struct Ps2AchievementView
{
    uint32_t id = 0;
    std::string title;
    std::string description;
    std::string category;
    int points = 0;
    bool unlocked = false;
    bool hasProgress = false;   // the condition reports "n of m"
    std::string progress;       // formatted by rcheevos, e.g. "12/99"
};

// Loads the set and restores what has been unlocked. `rdram` is the guest's 32 MB,
// which is where every condition reads from. Safe to call more than once.
void ps2AchievementsInit(uint8_t *rdram);

// Evaluates every achievement against the guest's memory. Called once per
// presented frame, so it must stay cheap: thirty conditions are a few hundred
// reads, which is nothing beside a frame of emulation.
void ps2AchievementsFrame(uint8_t *rdram);

// The unlock banner, drawn over the game. Does nothing when nothing is showing.
void ps2AchievementsDrawOverlay();

// The set, for the panel to list. Cheap enough to call every frame the panel is
// open, and it is the only thing the panel needs from here.
std::vector<Ps2AchievementView> ps2AchievementsView();

// Forgets every unlock, and clears the hit counts with them.
void ps2AchievementsReset();

// The file the set was read from, whether there was one, and what it calls itself.
// The panel says so rather than showing an empty list with no explanation.
const std::string &ps2AchievementsSetPath();
const std::string &ps2AchievementsSetName();
bool ps2AchievementsLoaded();

// How many the set holds, and how many are unlocked.
int ps2AchievementsTotal();
int ps2AchievementsUnlocked();
