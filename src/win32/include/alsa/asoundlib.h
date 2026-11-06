/* Windows speaker adapter for usb_receiver.c. Capture uses a separate backend;
 * Linux builds continue to include the system ALSA header. */
#ifndef LINRAD_PORTAUDIO_ADAPTER_H
#define LINRAD_PORTAUDIO_ADAPTER_H
#include <portaudio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
typedef struct { PaStream *stream; } snd_pcm_t;
typedef long snd_pcm_sframes_t;
#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_NONBLOCK 0
#define SND_PCM_FORMAT_FLOAT 0
#define SND_PCM_ACCESS_RW_INTERLEAVED 0
static inline int snd_pcm_open(snd_pcm_t **pcm,const char *device,int stream,int flags) {
    (void)stream; (void)flags;
    if (device && *device && strcmp(device,"default")) return -EINVAL;
    PaError error=Pa_Initialize();
    if (error) return -EIO;
    *pcm=calloc(1,sizeof(**pcm));
    if (!*pcm) { Pa_Terminate(); return -ENOMEM; }
    return 0;
}
static inline int snd_pcm_set_params(snd_pcm_t *pcm,int format,int access,unsigned channels,unsigned rate,int resample,unsigned latency) {
    (void)format; (void)access; (void)resample; (void)latency;
    PaError error=Pa_OpenDefaultStream(&pcm->stream,0,channels,paFloat32,rate,480,NULL,NULL);
    if (!error) error=Pa_StartStream(pcm->stream);
    return error ? -EIO : 0;
}
static inline snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm,const void *data,size_t count) {
    long available=Pa_GetStreamWriteAvailable(pcm->stream);
    if (available<0) return -EIO;
    if (!available) return -EAGAIN;
    if (count>(size_t)available) count=available;
    PaError error=Pa_WriteStream(pcm->stream,data,count);
    return !error || error==paOutputUnderflowed ? (long)count : -EIO;
}
static inline int snd_pcm_drop(snd_pcm_t *pcm) { return Pa_AbortStream(pcm->stream); }
static inline int snd_pcm_prepare(snd_pcm_t *pcm) { return Pa_IsStreamActive(pcm->stream)==1 ? 0 : Pa_StartStream(pcm->stream); }
static inline int snd_pcm_close(snd_pcm_t *pcm) {
    if (pcm->stream) Pa_CloseStream(pcm->stream);
    free(pcm); Pa_Terminate(); return 0;
}
static inline const char *snd_strerror(int error) {
    return error==-EINVAL ? "Windows audio device must be 'default'" : "Windows audio output unavailable (PortAudio)";
}
#endif
