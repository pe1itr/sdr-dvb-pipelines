/*
 * This file is part of DVB channel bonding.
 * Copyright (C) 2026 Rob Hardenberg, PE1ITR
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * DVB channel bonding is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * DVB channel bonding is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DVB channel bonding. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef BOND_NATIVE_H
#define BOND_NATIVE_H
#include "dvb_bond.h"
typedef struct bond_native bond_native;
bond_native *bond_native_create(const dvb_bond_config_v1 *c);
void bond_native_destroy(bond_native *n);
void bond_native_reset(bond_native *n);
void bond_native_deactivate(bond_native *n);
void bond_native_note_restart(bond_native *n);
int bond_native_active(const bond_native *n);
int bond_native_tx_push(bond_native *n, const uint8_t p[188]);
int bond_native_finish(bond_native *n);
int bond_native_tx_pop(bond_native *n, unsigned lane, uint64_t now, uint8_t p[188]);
int bond_native_rx_push(bond_native *n, unsigned slot, const uint8_t p[188], uint64_t now);
int bond_native_rx_pop(bond_native *n, uint64_t now, uint8_t p[188]);
uint64_t bond_native_due(const bond_native *n, unsigned lane);
void bond_native_status(const bond_native *n, dvb_bond_status_v1 *s);
int bond_native_event(bond_native *n, dvb_bond_event_v1 *e);
#endif
