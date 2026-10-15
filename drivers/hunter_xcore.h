#pragma once

#include <cstdint>

// Hunter X-Core support: zones on an X-Core connected to the REM port are driven over its
// single-wire bus on HUNTER_PIN, following the station outputs. Station n is X-Core zone n+1.
// Based on https://github.com/seb821/OpenSprinkler-Firmware-Hunter

void hunter_init();

// Starts or stops X-Core zones whose station output changed since the last call.
// remaining_seconds returns how long a station still has to run, or 0 if unknown.
void hunter_apply(const uint8_t* station_bits, uint8_t nstations, uint32_t (*remaining_seconds)(uint8_t sid));
