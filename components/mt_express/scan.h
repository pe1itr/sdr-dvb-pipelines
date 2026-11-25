// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// Private local datagram, from the second tuner to the spectrum view.
#define MT_SCAN_MAGIC 0x4d545333u
enum { MT_SCAN_POINTS = 61, MT_SCAN_STEP_KHZ = 1000, MT_SCAN_MISSING = UINT16_MAX };
enum { MT_SCAN_OFF = 0, MT_SCAN_ON = 1 }; // Other control words are tuning frequencies in kHz.
typedef struct {
    uint32_t magic, center_khz;
    uint16_t power[MT_SCAN_POINTS]; // Sum of STV0910 P1 POWERI and POWERQ.
} MtScanRow;
