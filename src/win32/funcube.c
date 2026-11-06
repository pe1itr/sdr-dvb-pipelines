/* SPDX-License-Identifier: GPL-3.0-only
 * Do not open the default Windows microphone as raw IQ. The Linux backend
 * matches HID and audio interfaces by USB parent; that matching still needs a
 * Windows implementation before direct FUNcube capture can be enabled safely.
 */
#include "funcube.h"
#include <stdio.h>
Funcube *funcube_open(const char *args,double frequency,double gain) {
    (void)args; (void)frequency; (void)gain;
    fputs("Direct FUNcube Pro+ capture is not available in this Windows build. Use Linrad UDP input.\n",stderr);
    return NULL;
}
void funcube_close(Funcube *receiver) { (void)receiver; }
int funcube_frequency(Funcube *receiver,double value) { (void)receiver; (void)value; return -1; }
int funcube_gain(Funcube *receiver,double value) { (void)receiver; (void)value; return -1; }
int funcube_read(Funcube *receiver,int16_t *iq,size_t count) { (void)receiver; (void)iq; (void)count; return -1; }
