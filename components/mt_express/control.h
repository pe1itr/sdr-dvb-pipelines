// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <libusb.h>
#include <stddef.h>
#include <stdint.h>

int mt_register_read(libusb_device_handle *dev, uint8_t address, uint16_t reg,
                     int reg_bytes, uint8_t *result, size_t count);
int mt_register_write(libusb_device_handle *dev, uint8_t address, uint16_t reg,
                      int reg_bytes, const uint8_t *data, size_t count);

int mt_control_open(libusb_device_handle *dev);

typedef struct {
    unsigned frequency_khz, symbol_rate, standard, code, input_b, search_khz;
    int alpha, beta, lock_acquire, lock_release;
} MtTune;
int mt_tune(libusb_device_handle *dev, const MtTune *settings);
