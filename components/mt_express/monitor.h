// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "control.h"
#include <stdatomic.h>
typedef struct {
    libusb_device_handle *device;
    int socket;
    atomic_bool finished;
    int error,live_tuning;
    MtTune tune;
    unsigned input_b, dx_relock, master_clock_hz;
    uint16_t last_offset;
    enum { DX_IDLE, DX_TRACKED, DX_TRYING } dx_state;
    int64_t loss_since_ms;
} MtMonitor;
void *mt_monitor_run(void *argument);
