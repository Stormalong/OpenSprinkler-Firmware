#include <vector>

#if defined(ESP8266)
#include <Arduino.h>
#endif

#include "defines.h"

using namespace std;

// This makes inverting the signal easy
#define HUNTER_ONE 1
#define HUNTER_ZERO  0

#define START_INTERVAL  900
#define SHORT_INTERVAL  208
#define LONG_INTERVAL 1875

#if defined(ESP8266)
#define HUNTER_PIN  16 // XXX D0
#else
#define HUNTER_PIN 512+24 // GPIO 24 on Raspberry Pi, base is 512
#endif

void HunterStop(byte zone);
void HunterStart(byte zone, byte time);
void HunterProgram(byte num);
void HunterBitfield(vector <byte>& bits, byte pos, byte val, byte len);
void HunterWrite(vector<byte> buffer, bool extrabit);
void HunterLow(void);
void HunterHigh(void);
void HunterInit(void);
