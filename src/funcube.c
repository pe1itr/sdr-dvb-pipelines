#define _XOPEN_SOURCE 700
#include "funcube.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* FUNcube Pro+ HID protocol: report ID 0, command, little-endian payload;
 * replies contain command, status (1 = success), payload.
 * https://github.com/pothosware/SoapyFCDPP/blob/master/SoapyFCDPP/fcdcmd.h
 * No firmware, bias-tee or USB-audio mixer settings are changed here. */
struct Funcube { int hid; snd_pcm_t *pcm; unsigned long overruns; };

static int command(Funcube *f, unsigned char cmd, uint32_t value, unsigned char *reply) {
    unsigned char out[65] = {0}, in[64];
    out[1] = cmd;
    for (int i = 0; i < 4; ++i) out[i+2] = (unsigned char)(value >> (8*i));
    if (write(f->hid, out, sizeof(out)) != sizeof(out)) {
        fprintf(stderr, "FUNcube HID write %u: %s\n", cmd, strerror(errno)); return -1;
    }
    struct pollfd fd = {.fd=f->hid, .events=POLLIN};
    int ready = poll(&fd, 1, 1000);
    if (ready <= 0 || !(fd.revents & POLLIN)) {
        fprintf(stderr, "FUNcube HID command %u: %s\n", cmd, ready == 0 ? "reply timeout" : "device interrupted/disconnected"); return -1;
    }
    ssize_t n = read(f->hid, in, sizeof(in));
    if (n < 2 || in[0] != cmd || in[1] != 1) {
        fprintf(stderr, "FUNcube HID command %u failed (reply length %zd)\n", cmd, n); return -1;
    }
    if (reply) { memset(reply, 0, 64); memcpy(reply, in, (size_t)n); }
    return 0;
}

static int read_hex(const char *directory, const char *name) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path)) return -1;
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    unsigned value; int ok = fscanf(file, "%x", &value); fclose(file);
    return ok == 1 ? (int)value : -1;
}

/* Match by USB parent, never by card ordering or the default microphone. */
static int usb_parent(const char *path, char parent[PATH_MAX]) {
    if (!realpath(path, parent)) return -1;
    for (;;) {
        if (read_hex(parent, "idVendor") == 0x04d8 && read_hex(parent, "idProduct") == 0xfb31) return 0;
        char *slash = strrchr(parent, '/');
        if (!slash || slash == parent) return -1;
        *slash = 0;
    }
}

static int find_device(const char *requested, char hid[PATH_MAX], char pcm[64]) {
    glob_t devices = {0};
    glob("/sys/class/hidraw/hidraw*", 0, NULL, &devices);
    int matches = 0;
    for (size_t i = 0; i < devices.gl_pathc; ++i) {
        char parent[PATH_MAX], candidate[PATH_MAX];
        const char *name = strrchr(devices.gl_pathv[i], '/') + 1;
        snprintf(candidate, sizeof(candidate), "/dev/%s", name);
        if (*requested && strcmp(requested, candidate)) continue;
        if (usb_parent(devices.gl_pathv[i], parent)) continue;
        strcpy(hid, candidate); ++matches;
    }
    globfree(&devices);
    if (matches != 1) {
        fprintf(stderr, "FUNcube Pro+: %s; connect a Pro+ (04d8:fb31), or select --args hid=/dev/hidrawN\n",
                matches ? "multiple devices found" : "device not found"); return -1;
    }
    char sys[PATH_MAX], parent[PATH_MAX];
    snprintf(sys, sizeof(sys), "/sys/class/hidraw/%s", strrchr(hid, '/')+1);
    if (usb_parent(sys, parent)) return -1;
    glob_t cards = {0}; glob("/sys/class/sound/card[0-9]*", 0, NULL, &cards);
    int found = 0;
    for (size_t i = 0; i < cards.gl_pathc; ++i) {
        char card_parent[PATH_MAX];
        if (!usb_parent(cards.gl_pathv[i], card_parent) && !strcmp(parent, card_parent)) {
            snprintf(pcm, 64, "hw:%s,0", strrchr(cards.gl_pathv[i], '/')+5); found = 1; break;
        }
    }
    globfree(&cards);
    if (!found) fprintf(stderr, "FUNcube Pro+: matching ALSA capture card not found\n");
    return found ? 0 : -1;
}

int funcube_frequency(Funcube *f, double frequency) {
    if (!funcube_frequency_valid(frequency)) {
        fprintf(stderr, "FUNcube Pro+: frequency must be 150000..240000000 or 420000000..1900000000 Hz\n"); return -1;
    }
    return command(f, 101, (uint32_t)llround(frequency), NULL);
}
int funcube_gain(Funcube *f, double gain) {
    if (!funcube_gain_valid(gain)) return -1;
    return command(f, 117, (uint32_t)gain, NULL);
}
void funcube_close(Funcube *f) {
    if (!f) return;
    if (f->pcm) snd_pcm_close(f->pcm);
    if (f->hid >= 0) close(f->hid);
    free(f);
}

Funcube *funcube_open(const char *args, double frequency, double gain) {
    char requested[PATH_MAX] = "", hid[PATH_MAX], pcm[64];
    int lna = 1, mixer = 1;
    char *copy = strdup(args), *save = NULL;
    if (!copy) return NULL;
    for (char *item = strtok_r(copy, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        if (!strncmp(item, "hid=/dev/hidraw", 15) && strlen(item+4) < sizeof(requested)) strcpy(requested, item+4);
        else if (!strcmp(item, "lna=0") || !strcmp(item, "lna=1")) lna = item[4]-'0';
        else if (!strcmp(item, "mixer=0") || !strcmp(item, "mixer=1")) mixer = item[6]-'0';
        else {
            fprintf(stderr, "FUNcube Pro+: unknown option '%s'; use hid=/dev/hidrawN,lna=0|1,mixer=0|1\n", item);
            free(copy); return NULL;
        }
    }
    free(copy);
    if (find_device(requested, hid, pcm)) return NULL;
    Funcube *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->hid = open(hid, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (f->hid < 0) { fprintf(stderr, "FUNcube %s: %s (requires HID access)\n", hid, strerror(errno)); goto fail; }
    struct hidraw_devinfo info = {0};
    if (ioctl(f->hid, HIDIOCGRAWINFO, &info) < 0 || info.vendor != 0x04d8 || info.product != (short)0xfb31) {
        fprintf(stderr, "FUNcube: selected HID is not a Pro+\n"); goto fail;
    }
    if (flock(f->hid, LOCK_EX | LOCK_NB)) { fprintf(stderr, "FUNcube: receiver already in use\n"); goto fail; }
    unsigned char reply[64];
    /* Drain replies left by a previous controller before querying firmware. */
    while (read(f->hid, reply, sizeof(reply)) > 0) {}
    if (command(f, 1, 0, reply)) goto fail;
    if (memcmp(reply+2, "FCDAPP", 6)) { fprintf(stderr, "FUNcube: device is not in application mode\n"); goto fail; }
    int err = snd_pcm_open(&f->pcm, pcm, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) { fprintf(stderr, "FUNcube ALSA %s: %s\n", pcm, snd_strerror(err)); goto fail; }
    /* 'hw:' plus soft_resample=0 guarantees untouched stereo I/Q at 192 kS/s. */
    err = snd_pcm_set_params(f->pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                             2, FUNCUBE_RATE, 0, 100000);
    if (err < 0) { fprintf(stderr, "FUNcube ALSA 192000 Hz stereo s16: %s\n", snd_strerror(err)); goto fail; }
    if (funcube_frequency(f, frequency) || command(f, 110, lna, NULL) ||
        command(f, 114, mixer, NULL) || funcube_gain(f, gain) || command(f, 122, 0, NULL)) goto fail;
    fprintf(stderr, "[SDR] FUNcube Pro+: %s, %s, fixed 192000 samples/s; IF gain %.0f dB, LNA %s, mixer %s\n",
            hid, pcm, gain, lna ? "on" : "off", mixer ? "high" : "low");
    return f;
fail:
    funcube_close(f); return NULL;
}

int funcube_read(Funcube *f, int16_t *iq, size_t samples) {
    if (snd_pcm_state(f->pcm) == SND_PCM_STATE_PREPARED) {
        int err = snd_pcm_start(f->pcm);
        if (err < 0) { fprintf(stderr, "FUNcube capture start: %s\n", snd_strerror(err)); return -1; }
    }
    snd_pcm_sframes_t n = snd_pcm_readi(f->pcm, iq, samples);
    if (n == -EAGAIN || n == -EINTR) {
        int ready = snd_pcm_wait(f->pcm, 100);
        if (ready >= 0 || ready == -EINTR) return 0;
        n = ready;
    }
    if (n == -EPIPE || n == -ESTRPIPE) {
        ++f->overruns;
        fprintf(stderr, "[SDR] FUNcube audio overrun/suspend #%lu; samples lost\n", f->overruns);
        /* Nonblocking recovery: no indefinite snd_pcm_resume loop on unplug. */
        int err = snd_pcm_prepare(f->pcm);
        if (err >= 0) return 0;
        n = err;
    }
    if (n < 0) { fprintf(stderr, "FUNcube capture: %s\n", snd_strerror((int)n)); return -1; }
    return (int)n;
}
