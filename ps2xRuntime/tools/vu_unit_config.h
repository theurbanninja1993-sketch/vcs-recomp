#pragma once

// The replay and generator share one implementation, compiled separately for
// each unit. Keep the registries, identifiers and capture formats distinct.
#ifndef PS2X_VU_UNIT
#define PS2X_VU_UNIT 1
#endif
static_assert(PS2X_VU_UNIT == 0 || PS2X_VU_UNIT == 1);
constexpr bool kVu0 = PS2X_VU_UNIT == 0;
constexpr uint32_t kVuCodeSize = kVu0 ? PS2_VU0_CODE_SIZE : PS2_VU1_CODE_SIZE;
constexpr uint32_t kVuDataSize = kVu0 ? PS2_VU0_DATA_SIZE : PS2_VU1_DATA_SIZE;
constexpr uint32_t kVuCaptureMagic = kVu0 ? 0x30555643u : 0x31555643u;
constexpr uint32_t kVuBudget = kVu0 ? 4096u : 65536u;
constexpr auto kVuUnit = kVu0 ? VU1Interpreter::Unit::VU0 : VU1Interpreter::Unit::VU1;
inline uint8_t *vuCode(PS2Memory &memory) { return kVu0 ? memory.getVU0Code() : memory.getVU1Code(); }
inline uint8_t *vuData(PS2Memory &memory) { return kVu0 ? memory.getVU0Data() : memory.getVU1Data(); }
inline void markVuCodeModified(PS2Memory &memory)
{
    if constexpr (kVu0) memory.markVU0CodeModified();
    else memory.markVU1CodeModified();
}
