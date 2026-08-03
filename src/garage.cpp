// garage.cpp — "Cancello" opener. Reproduces the Holtek HT12E fixed code
// captured off the user's real gate remote (gate.sub / GARAGE.sig: Holtek_HT12X,
// 12-bit, OOK 433.92 MHz, Key 0x410, TE 321us) — the same signal that opens the
// gate from the moto dashboard's FS1000A module.
//
// ── Why this file was rewritten (2026-08-02) ────────────────────────────────
// The SX1262 has NO true OOK modulator (LoRa / FSK / GFSK / BPSK only). The old
// version keyed the carrier by hand — transmitDirect() for a mark, standby() for
// a space, scanned by delayMicroseconds(). That gave SOFTWARE timing (jitter +
// the SX1262's own PLL/PA transition latency), which smears the 321us marks that
// the gate's rigid Holtek envelope-detector needs. It never opened the gate.
//
// The Tesla-charge-port tile (tesla_cp.cpp) fires a working 433.92 command off
// the SAME chip — because it does NOT hand-key. It builds the waveform into a
// PAYLOAD and lets the FSK PACKET ENGINE clock it out in hardware (radio.transmit),
// with a small deviation that a wideband RKE receiver reads as OOK. Timing is
// exact because the modem generates it, not delay loops.
//
// So we now do the same here: encode the HT12E frame as an NRZ chip-stream (1
// chip = 1 TE = 321us; mark = '1', space = '0') and transmit it as a raw FSK
// packet at bitrate = 1/TE. The long inter-frame guard (~32*TE) is done with the
// carrier actually OFF (standby) — there the timing isn't critical and a real
// gap helps the receiver re-arm.
//
// Emulating OOK with FSK only works if the "space" tone falls outside the gate
// receiver's passband (so it reads as "no carrier"). We centre the MARK on
// 433.92 (fc = 433.92 - dev, assuming 1 -> +dev) and push the space a good
// distance below. GATE_DEV is THE knob to tune if it still won't trigger: wider
// pushes the space further out (good for a narrow superhet RX), but a cheap
// wideband superregen RX may hear the carrier regardless — that would be the
// hard physical limit, and the gate stays on the FS1000A dashboard / CC1101
// rf-remote (GARAGE.sig), which have real OOK modulators.
//
// If 1->+dev is inverted on this part, the mark lands on the wrong tone: flip by
// setting GATE_FC to (GATE_FREQ + GATE_DEV_MHZ) instead. Left as a one-line tweak.

#include "garage.h"
#include "lora_screen.h"
#include "aprs.h"
#include "pager.h"
#include <Arduino.h>
#include <LilyGoLib.h>

// ---- Signal, from gate.sub / GARAGE.sig (identical to the working dashboard) -
#define GATE_KEY        0x410UL   // 12-bit fixed code
#define GATE_BITS       12
#define GATE_TE_US      321       // timing element, microseconds
#define GATE_REPEAT     24        // real remotes spam many copies; give plenty
#define GATE_POWER      22        // dBm — SX1262 max, for the tiny watch antenna

// FSK-as-OOK parameters.
#define GATE_DEV_KHZ    60.0f     // deviation — how far the "space" tone sits
#define GATE_DEV_MHZ    0.060f
#define GATE_FREQ       433.92f
#define GATE_FC         (GATE_FREQ - GATE_DEV_MHZ)   // centre so MARK == 433.92
#define GATE_BITRATE    (1000.0f / GATE_TE_US)       // kbps → 321us per chip (~3.115)
#define GATE_RXBW       156.2f    // TX-irrelevant, but beginFSK wants a valid BW
#define GATE_GUARD_US   (GATE_TE_US * 32)            // inter-frame gap, carrier OFF

// One HT12E frame as an NRZ chip-stream: each data bit is 3 chips, then one
// guard mark. HT12E: '1' = MARK 1TE + SPACE 2TE (chips 1,0,0);
//                     '0' = MARK 2TE + SPACE 1TE (chips 1,1,0).
// 12 bits * 3 + 1 guard mark = 37 chips → 5 bytes (trailing bits stay 0 = space,
// which just extends into the OFF guard gap).
#define GATE_CHIPS      (GATE_BITS * 3 + 1)
#define GATE_PL_BYTES   ((GATE_CHIPS + 7) / 8)       // = 5

static int16_t s_last_error = 0;
int16_t garage_last_error() { return s_last_error; }

static void put_chip(uint8_t *pl, int idx, int on)
{
    if (on) pl[idx >> 3] |= (uint8_t)(0x80u >> (idx & 7));
}

static void build_frame(uint8_t *pl)
{
    for (int i = 0; i < GATE_PL_BYTES; i++) pl[i] = 0;
    int chip = 0;
    for (int i = GATE_BITS - 1; i >= 0; i--) {          // MSB first
        bool bit = (GATE_KEY >> i) & 1;
        put_chip(pl, chip++, 1);                        // both start with a mark
        put_chip(pl, chip++, bit ? 0 : 1);              // '0' has a 2nd mark chip
        put_chip(pl, chip++, 0);                        // both end on a space chip
    }
    put_chip(pl, chip++, 1);                            // guard mark (1 TE)
    // remaining chips in the last byte stay 0 (space) → merge into the OFF gap
}

bool garage_transmit()
{
    // Shared SX1262 — refuse if LoRa / APRS / pager hold it (same as tesla_cp).
    if (lora_screen_is_powered() || aprs_is_running() || pager_is_running())
        return false;

    uint8_t frame[GATE_PL_BYTES];
    build_frame(frame);

    // Bring the modem up in FSK, carrier centred so the MARK tone == 433.92 MHz.
    int16_t rc = radio.beginFSK(GATE_FC, GATE_BITRATE, GATE_DEV_KHZ,
                                GATE_RXBW, GATE_POWER, 0 /*preamble*/, 1.6f);
    s_last_error = rc;
    if (rc != RADIOLIB_ERR_NONE) return false;

    // Raw packet: no preamble, no sync word, no CRC, no whitening — the payload
    // IS the waveform. NRZ so a '1' bit is one high tone (no Manchester).
    radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    radio.setPreambleLength(0);
    uint8_t nosync[1] = { 0 };
    radio.setSyncWord(nosync, 0);
    radio.setCRC(0);
    radio.setWhitening(false);
    radio.fixedPacketLengthMode(GATE_PL_BYTES);

    // Fire the frame GATE_REPEAT times; between copies drop the carrier entirely
    // for the inter-frame guard, which the Holtek receiver uses to frame-sync.
    for (int rep = 0; rep < GATE_REPEAT; rep++) {
        rc = radio.transmit(frame, GATE_PL_BYTES);      // hardware-clocked chips
        if (rc != RADIOLIB_ERR_NONE) { s_last_error = rc; break; }
        if (rep + 1 < GATE_REPEAT) {
            radio.standby();                            // carrier truly OFF
            delayMicroseconds(GATE_GUARD_US);
        }
    }

    // Hand the radio back to low-power standby for the next user.
    radio.standby();
    return s_last_error == RADIOLIB_ERR_NONE;
}
