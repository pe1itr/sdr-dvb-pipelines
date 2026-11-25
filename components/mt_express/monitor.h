// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "control.h"
#include <stdatomic.h>
typedef struct {
    libusb_device_handle *device;
    int socket;
    atomic_bool finished;
    int error;
} MtMonitor;
void *mt_monitor_run(void *argument);
