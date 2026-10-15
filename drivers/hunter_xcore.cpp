#include "hunter_xcore.h"

#include "../defines.h"

#if defined(HUNTER_PIN)

#include "../platform/clock.h"
#include "../platform/gpio.h"

#include <cstdio>
#include <vector>

namespace {

// Bus levels; swap to invert the signal
constexpr uint8_t HUNTER_ONE = 1;
constexpr uint8_t HUNTER_ZERO = 0;

// Pulse widths in microseconds
constexpr uint32_t START_INTERVAL = 900;
constexpr uint32_t SHORT_INTERVAL = 208;
constexpr uint32_t LONG_INTERVAL = 1875;

constexpr uint8_t HUNTER_MAX_ZONE = 48;
constexpr uint8_t HUNTER_MAX_MINUTES = 240;

uint8_t previous_bits[MAX_NUM_BOARDS] = {0};

// Writes len bits of val, least significant first, starting at bit position pos
void set_bitfield(std::vector<uint8_t>& bits, uint8_t pos, uint8_t val, uint8_t len) {
	while (len > 0) {
		if (val & 0x1) {
			bits[pos / 8] |= 0x80 >> (pos % 8);
		} else {
			bits[pos / 8] &= ~(0x80 >> (pos % 8));
		}
		len--;
		val >>= 1;
		pos++;
	}
}

void write_low() {
	digitalWrite(HUNTER_PIN, HUNTER_ONE);
	delayMicroseconds(SHORT_INTERVAL);
	digitalWrite(HUNTER_PIN, HUNTER_ZERO);
	delayMicroseconds(LONG_INTERVAL);
}

void write_high() {
	digitalWrite(HUNTER_PIN, HUNTER_ONE);
	delayMicroseconds(LONG_INTERVAL);
	digitalWrite(HUNTER_PIN, HUNTER_ZERO);
	delayMicroseconds(SHORT_INTERVAL);
}

// Sends a frame: reset pulse, start pulse, the bits (high order first), an optional extra 1 bit and a stop bit
void write_frame(const std::vector<uint8_t>& buffer, bool extra_bit) {
	digitalWrite(HUNTER_PIN, HUNTER_ONE);
	delay(325);
	digitalWrite(HUNTER_PIN, HUNTER_ZERO);
	delay(65);

	digitalWrite(HUNTER_PIN, HUNTER_ONE);
	delayMicroseconds(START_INTERVAL);
	digitalWrite(HUNTER_PIN, HUNTER_ZERO);
	delayMicroseconds(SHORT_INTERVAL);

	for (uint8_t value : buffer) {
		for (uint8_t bit = 0; bit < 8; bit++) {
			if (value & 0x80) write_high();
			else write_low();
			value <<= 1;
		}
	}

	if (extra_bit) write_high();
	write_low();
}

// Runs zone (1-48) for minutes (1-240); 0 minutes stops it
void start_zone(uint8_t zone, uint8_t minutes) {
	if (zone < 1 || zone > HUNTER_MAX_ZONE || minutes > HUNTER_MAX_MINUTES) return;
	if (minutes) printf("X-Core zone %d start for %d min\n", zone, minutes);
	else printf("X-Core zone %d stop\n", zone);

	std::vector<uint8_t> buffer = {0xff, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x00, 0x01, 0xb8, 0x3f};

	// Bits 9:10 are 0x1 for zones above 12 and 0x2 otherwise
	set_bitfield(buffer, 9, zone > 12 ? 0x1 : 0x2, 2);

	// The zone is encoded six times with three different offsets
	set_bitfield(buffer, 23, zone + 0x17, 7);
	set_bitfield(buffer, 36, zone + 0x17, 7);
	set_bitfield(buffer, 49, zone + 0x23, 7);
	set_bitfield(buffer, 62, zone + 0x23, 7);
	set_bitfield(buffer, 75, zone + 0x2f, 7);
	set_bitfield(buffer, 88, zone + 0x2f, 7);

	// The time is encoded three times, split by nibble
	set_bitfield(buffer, 31, minutes, 4);
	set_bitfield(buffer, 44, minutes >> 4, 4);
	set_bitfield(buffer, 57, minutes, 4);
	set_bitfield(buffer, 70, minutes >> 4, 4);
	set_bitfield(buffer, 83, minutes, 4);
	set_bitfield(buffer, 96, minutes >> 4, 4);

	set_bitfield(buffer, 109, zone - 1, 4);

	write_frame(buffer, true);
}

} // namespace

void hunter_init() {
	pinMode(HUNTER_PIN, OUTPUT);
	digitalWrite(HUNTER_PIN, HUNTER_ZERO);
}

void hunter_apply(const uint8_t* station_bits, uint8_t nstations, uint32_t (*remaining_seconds)(uint8_t sid)) {
	for (uint8_t sid = 0; sid < nstations && sid < HUNTER_MAX_ZONE; sid++) {
		uint8_t mask = 1 << (sid & 7);
		bool on = station_bits[sid >> 3] & mask;
		if (on == (bool)(previous_bits[sid >> 3] & mask)) continue;

		if (on) {
			// The X-Core runs the zone on its own timer; give it one minute more than needed so it never
			// stops early, and the longest time when the run length is unknown
			uint32_t seconds = remaining_seconds ? remaining_seconds(sid) : 0;
			uint32_t minutes = seconds ? seconds / 60 + 1 : HUNTER_MAX_MINUTES;
			start_zone(sid + 1, minutes > HUNTER_MAX_MINUTES ? HUNTER_MAX_MINUTES : (uint8_t)minutes);
			previous_bits[sid >> 3] |= mask;
		} else {
			// Stop twice: the X-Core occasionally misses a single stop frame
			start_zone(sid + 1, 0);
			delay(100);
			start_zone(sid + 1, 0);
			previous_bits[sid >> 3] &= ~mask;
		}
	}
}

#endif
