// NVN counter timestamps: raw reports carry 614.4 MHz ticks (384 ticks per 625 ns), not the CPU's 19.2 MHz system counter.
// The game converts report differences with that rate itself (its GPU frame-time statistics), so a report written in system
// ticks reads 32 times too short. Conversions keep the guest clock's epoch both ways; 128-bit products avoid overflow on long uptimes.
// Adapted from nxnative (github.com/seedds/nxnative, MIT, (c) 2026 seedds).
#pragma once
#include <stdint.h>

static inline uint64_t nvn_timestamp_from_ns(uint64_t ns) { return (uint64_t)((__uint128_t)ns * 384 / 625); }
static inline uint64_t nvn_timestamp_to_ns(uint64_t ticks) { return (uint64_t)((__uint128_t)ticks * 625 / 384); }
