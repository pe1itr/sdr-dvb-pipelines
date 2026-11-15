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
