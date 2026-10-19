#ifndef PS2_MODS_H
#define PS2_MODS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Drop-in mods: every folder in the mods folder is a mod; what it contains
// decides what it does. Supported so far: PCSX2 texture packs (replacement
// images named after the texture's hash, in any sub-folder) and PCSX2 .pnach
// memory patches.
struct PS2ModInfo
{
    std::string name; // folder name
    std::string path;
    bool enabled = true;
    size_t textures = 0;   // replacement textures
    size_t patches = 0;    // memory writes parsed from the mod's .pnach files
    size_t otherFiles = 0;
};

// One enabled write from a PCSX2 .pnach file: "patch=1,EE,ADDRESS,TYPE,VALUE".
struct PS2PnachEntry
{
    uint32_t address = 0; // physical EE RAM address (0..PS2_RAM_SIZE-1)
    uint8_t size = 1;     // 1 = byte, 2 = short, 4 = word
    uint32_t value = 0;   // written little-endian
};

// Parses the enabled writes out of PCSX2 .pnach text (extra blank lines,
// comments and "extended"/conditional codes are ignored). Exposed for tests.
std::vector<PS2PnachEntry> ps2PnachParse(std::string_view text);

// Writes parsed entries into a RAM buffer (bounds-checked, little-endian).
// Exposed for tests; the runtime passes the 32 MB EE RAM each frame.
void ps2PnachApply(const std::vector<PS2PnachEntry> &entries, uint8_t *ram, size_t ramSize);

// <mods folder>: PS2X_MODS_DIR, or "mods" next to the executable (created).
std::string ps2ModsDirectory();
// Rescans the mods folder (enabled state from the settings) and applies
// the enabled mods.
void ps2ModsRefresh();
const std::vector<PS2ModInfo> &ps2Mods();
// Enables / disables a mod (persisted in the settings) and re-applies.
void ps2ModsSetEnabled(const std::string &name, bool enabled);
// Opens the mods folder in the desktop file manager.
void ps2ModsOpenFolder();
// Opens a folder in the desktop's file manager.
void ps2OpenFolder(const std::string &folder);
// The folder holding the executable (mods/, music/ and the station logo live there).
std::string ps2ExecutableDirectoryPath();
// Applies the enabled mods' .pnach writes to EE RAM. Called once per presented
// frame (PCSX2 re-applies enabled patches continuously). No-op when cheats are
// switched off or nothing is loaded.
void ps2ModsApplyPatches(uint8_t *rdram);

#endif
