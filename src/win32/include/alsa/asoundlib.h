/* Windows speaker adapter for USB monitoring and DAB+. Capture uses a separate backend;
 * Linux builds continue to include the system ALSA header. */
#ifndef LINRAD_PORTAUDIO_ADAPTER_H
#define LINRAD_PORTAUDIO_ADAPTER_H
#include <portaudio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
typedef struct { PaStream *stream; PaDeviceIndex device; } snd_pcm_t;
typedef long snd_pcm_sframes_t;
#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_NONBLOCK 0
#define SND_PCM_FORMAT_FLOAT 0
#define SND_PCM_FORMAT_S16_LE 1
#define SND_PCM_ACCESS_RW_INTERLEAVED 0
static inline int snd_pcm_open(snd_pcm_t **pcm,const char *device,int stream,int flags) {
    (void)stream; (void)flags;
    PaError error=Pa_Initialize();
    if (error) return -EIO;
    PaDeviceIndex selected=Pa_GetDefaultOutputDevice();
    if (device && *device && strcmp(device,"default")) {
        selected=paNoDevice;
        for (PaDeviceIndex i=0;i<Pa_GetDeviceCount();++i) {
            const PaDeviceInfo *info=Pa_GetDeviceInfo(i);
            if (info && info->maxOutputChannels>0 && !strcmp(device,info->name)) { selected=i; break; }
        }
    }
    if (selected==paNoDevice) { Pa_Terminate(); return -EINVAL; }
    *pcm=(snd_pcm_t *)calloc(1,sizeof(**pcm));
    if (!*pcm) { Pa_Terminate(); return -ENOMEM; }
    (*pcm)->device=selected;
    return 0;
}
static inline int snd_pcm_set_params(snd_pcm_t *pcm,int format,int access,unsigned channels,unsigned rate,int resample,unsigned latency) {
    (void)access; (void)resample;
    if (format!=SND_PCM_FORMAT_FLOAT && format!=SND_PCM_FORMAT_S16_LE) return -EINVAL;
    PaStreamParameters output={0};
    output.device=pcm->device; output.channelCount=channels;
    output.sampleFormat=format==SND_PCM_FORMAT_S16_LE ? paInt16 : paFloat32;
    output.suggestedLatency=latency/1000000.0;
    PaError error=Pa_OpenStream(&pcm->stream,NULL,&output,rate,paFramesPerBufferUnspecified,paNoFlag,NULL,NULL);
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
static inline int snd_pcm_drop(snd_pcm_t *pcm) { return pcm->stream ? Pa_AbortStream(pcm->stream) : 0; }
static inline int snd_pcm_prepare(snd_pcm_t *pcm) { return Pa_IsStreamActive(pcm->stream)==1 ? 0 : Pa_StartStream(pcm->stream); }
static inline int snd_pcm_wait(snd_pcm_t *pcm,int timeout) {
    Pa_Sleep(timeout); return Pa_GetStreamWriteAvailable(pcm->stream)>0;
}
static inline int snd_pcm_recover(snd_pcm_t *pcm,int error,int silent) {
    (void)silent;
    return error==-EPIPE ? snd_pcm_prepare(pcm) : error;
}
static inline int snd_pcm_close(snd_pcm_t *pcm) {
    if (pcm->stream) Pa_CloseStream(pcm->stream);
    free(pcm); Pa_Terminate(); return 0;
}
static inline const char *snd_strerror(int error) {
    return error==-EINVAL ? "Select 'default' or a Windows playback device name" : "Windows audio output unavailable (PortAudio)";
}
#endif
