// garage.cpp — "Cancello" opener. Reproduces the exact Holtek HT12E fixed code
// captured off the user's real gate remote on the moto dashboard (gate.sub:
// Protocol Holtek_HT12X, 12-bit, OOK 433.92 MHz, TE 321us). The moto dash keyed
// an FS1000A OOK module on a GPIO; the T-Watch has no such module, so this does
// a BEST-EFFORT OOK by keying the SX1262 carrier on/off (transmitDirect = on,
// standby = off) with the identical HT12E timing. HT12E receivers are very
// tolerant (fixed code, long guard, many repeats), so this often works, but the
// keying isn't as clean as a real OOK module — if the gate won't trigger, a tiny
// FS1000A/CC1101 433 module is the reliable path.

#include "garage.h"
#include "lora_screen.h"
#include "aprs.h"
#include "pager.h"
#include <Arduino.h>
#include <LilyGoLib.h>

// ---- Signal, straight from gate.sub -----------------------------------------
#define GATE_FREQ    433.92f
#define GATE_KEY     0x410UL   // 12-bit fixed code
#define GATE_BITS    12
#define GATE_TE      321       // timing element, microseconds
#define GATE_REPEAT  20        // carrier keying is jittery — send extra copies

static int16_t s_last_error = 0;
int16_t garage_last_error() { return s_last_error; }

// SX1262 has no native OOK modulator. Instead of stopping/restarting the carrier
// per symbol (standby -> transmitDirect re-locks the PLL, far too slow/jittery),
// keep the carrier locked the whole burst and key it by swinging the PA between
// max and min power. The ~31 dB amplitude swing reads as OOK to an envelope-
// detector gate receiver, and staying locked keeps the pulse timing clean.
#define GATE_PWR_MARK   22    // dBm — "on"
#define GATE_PWR_SPACE  (-9)  // dBm — "off" (SX1262 minimum)
static inline void mark()  { radio.setOutputPower(GATE_PWR_MARK);  }
static inline void space() { radio.setOutputPower(GATE_PWR_SPACE); }

bool garage_transmit()
{
    // Shared SX1262 — refuse if LoRa / APRS / pager hold it (same as tesla_cp).
    if (lora_screen_is_powered() || aprs_is_running() || pager_is_running())
        return false;

    // Park the radio on 433.92 MHz at full power. Bitrate/deviation/bw are
    // irrelevant (we key the raw carrier's amplitude); reuse the safe values the
    // Tesla-CP tile uses so the SX1262 comes up in a known-good state.
    int16_t rc = radio.beginFSK(GATE_FREQ, 2.5f, 10.0f, 46.9f, GATE_PWR_MARK, 16, 1.6f);
    s_last_error = rc;
    if (rc != RADIOLIB_ERR_NONE) return false;

    // Carrier ON for the whole burst (stays PLL-locked); mark()/space() only
    // swing its power.
    radio.transmitDirect();

    const uint32_t te  = GATE_TE;
    const uint32_t te2 = te * 2;

    // HT12E encoding (MSB first): "1" = MARK 1*TE, SPACE 2*TE; "0" = MARK 2*TE,
    // SPACE 1*TE; then a guard of MARK 1*TE, SPACE 36*TE. Frame repeated
    // GATE_REPEAT times. NOTE: interrupts stay ENABLED — a ~450 ms interrupts-off
    // burst would trip the interrupt watchdog. HT12E tolerates timing jitter.
    for (int rep = 0; rep < GATE_REPEAT; rep++) {
        for (int i = GATE_BITS - 1; i >= 0; i--) {
            bool bit = (GATE_KEY >> i) & 1;
            if (bit) {
                mark();  delayMicroseconds(te);
                space(); delayMicroseconds(te2);
            } else {
                mark();  delayMicroseconds(te2);
                space(); delayMicroseconds(te);
            }
        }
        // Guard / inter-frame sync.
        mark();  delayMicroseconds(te);
        space(); delayMicroseconds(te * 36);
    }

    // Hand the radio back to standby for the next user (LoRa / pager / APRS).
    radio.standby();
    return s_last_error == RADIOLIB_ERR_NONE;
}
