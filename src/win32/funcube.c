/* SPDX-License-Identifier: GPL-3.0-only
 * Pair HID and exclusive WASAPI IQ capture by device container, never by
 * microphone ordering or a friendly-name substring. */
#define COBJMACROS
#include "funcube.h"
#include <hidapi/hidapi.h>
#include <portaudio.h>
#include <pa_win_wasapi.h>
#include <setupapi.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>

struct Funcube {
    hid_device *hid;
    PaStream *pcm;
    int audio_initialized, com_initialized;
    HANDLE lock;
    unsigned long overruns;
};
static int command(Funcube *f,unsigned char cmd,uint32_t value,unsigned char *reply) {
    unsigned char out[65]={0}, in[64];out[1]=cmd;
    for (int i=0;i<4;++i) out[i+2]=(unsigned char)(value>>(8*i));
    if (hid_write(f->hid,out,sizeof(out))!=(int)sizeof(out)) {
        fprintf(stderr,"FUNcube HID write %u failed\n",cmd);return -1;
    }
    int n=hid_read_timeout(f->hid,in,sizeof(in),1000);
    if (n<2 || in[0]!=cmd || in[1]!=1) {
        fprintf(stderr,"FUNcube HID command %u failed (reply length %d)\n",cmd,n);return -1;
    }
    if (reply) {memset(reply,0,64);memcpy(reply,in,n);}
    return 0;
}
static int hid_container(const char *path,GUID *container) {
    wchar_t *wide=(wchar_t *)g_utf8_to_utf16(path,-1,NULL,NULL,NULL);
    if (!wide) return -1;
    HDEVINFO devices=SetupDiCreateDeviceInfoList(NULL,NULL);
    SP_DEVICE_INTERFACE_DATA iface={.cbSize=sizeof(iface)};
    SP_DEVINFO_DATA device={.cbSize=sizeof(device)};
    int found=0;
    if (devices!=INVALID_HANDLE_VALUE && SetupDiOpenDeviceInterfaceW(devices,wide,0,&iface)) {
        DWORD size=0;
        SetupDiGetDeviceInterfaceDetailW(devices,&iface,NULL,0,&size,NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail=malloc(size);
        if (detail) {
            detail->cbSize=sizeof(*detail);
            DEVPROPTYPE type;
            found=SetupDiGetDeviceInterfaceDetailW(devices,&iface,detail,size,NULL,&device) &&
                SetupDiGetDevicePropertyW(devices,&device,&DEVPKEY_Device_ContainerId,&type,
                    (BYTE *)container,sizeof(*container),NULL,0) && type==DEVPROP_TYPE_GUID;
            free(detail);
        }
    }
    if (devices!=INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(devices);
    g_free(wide);return found ? 0 : -1;
}
static PaDeviceIndex matching_audio(const GUID *container) {
    PaDeviceIndex selected=paNoDevice;
    for (PaDeviceIndex i=0;i<Pa_GetDeviceCount();++i) {
        const PaDeviceInfo *info=Pa_GetDeviceInfo(i);
        if (!info || info->maxInputChannels<2 || Pa_GetHostApiInfo(info->hostApi)->type!=paWASAPI) continue;
        IMMDevice *device=NULL;
        if (PaWasapi_GetIMMDevice(i,(void **)&device)!=paNoError || !device) continue;
        IPropertyStore *properties=NULL;
        if (FAILED(IMMDevice_OpenPropertyStore(device,STGM_READ,&properties))) continue;
        PROPVARIANT value;PropVariantInit(&value);
        HRESULT result=IPropertyStore_GetValue(properties,&PKEY_Device_ContainerId,&value);
        GUID endpoint;
        int match=0;
        if (SUCCEEDED(result)) {
            if (value.vt==VT_CLSID && value.puuid) {endpoint=*value.puuid;match=1;}
            else if (value.vt==VT_LPWSTR && value.pwszVal && SUCCEEDED(CLSIDFromString(value.pwszVal,&endpoint))) match=1;
        }
        match=match && IsEqualGUID(container,&endpoint);
        PropVariantClear(&value);IPropertyStore_Release(properties);
        /* PortAudio owns the borrowed IMMDevice pointer. */
        if (match) {
            if (selected!=paNoDevice) {fputs("FUNcube: multiple matching audio endpoints\n",stderr);return paNoDevice;}
            selected=i;
        }
    }
    return selected;
}
int funcube_frequency(Funcube *f,double value) {
    if (!funcube_frequency_valid(value)) return -1;
    return command(f,101,(uint32_t)llround(value),NULL);
}
int funcube_gain(Funcube *f,double value) {
    return funcube_gain_valid(value) ? command(f,117,(uint32_t)value,NULL) : -1;
}
void funcube_close(Funcube *f) {
    if (!f) return;
    if (f->pcm) {Pa_AbortStream(f->pcm);Pa_CloseStream(f->pcm);}
    if (f->audio_initialized) Pa_Terminate();
    if (f->hid) hid_close(f->hid);
    if (f->lock) CloseHandle(f->lock);
    if (f->com_initialized) CoUninitialize();
    free(f);
}
Funcube *funcube_open(const char *args,double frequency,double gain) {
    char *copy=g_strdup(args), *requested=NULL;
    int lna=1,mixer=1,valid=1;
    char *save=NULL;
    for (char *item=strtok_r(copy,",",&save);item;item=strtok_r(NULL,",",&save)) {
        if (!strncmp(item,"hid=",4) && item[4]) requested=item+4;
        else if (!strcmp(item,"lna=0") || !strcmp(item,"lna=1")) lna=item[4]-'0';
        else if (!strcmp(item,"mixer=0") || !strcmp(item,"mixer=1")) mixer=item[6]-'0';
        else valid=0;
    }
    if (!valid || !funcube_frequency_valid(frequency) || !funcube_gain_valid(gain)) {
        fputs("FUNcube: invalid frequency/gain or options; use hid=WINDOWS_HID_PATH,lna=0|1,mixer=0|1\n",stderr);
        g_free(copy);return NULL;
    }
    struct hid_device_info *list=hid_enumerate(0x04d8,0xfb31), *selected=NULL;
    int count=0;
    for (struct hid_device_info *item=list;item;item=item->next) {
        if (requested && strcmp(item->path,requested)) continue;
        selected=item;++count;
    }
    Funcube *f=calloc(1,sizeof(*f));
    GUID container;
    if (!f || count!=1 || hid_container(selected->path,&container)) {
        fputs("FUNcube: connect one Pro+ (04d8:fb31), or select its HID path; device container must be available\n",stderr);
        hid_free_enumeration(list);g_free(copy);free(f);return NULL;
    }
    char mutex[128];snprintf(mutex,sizeof(mutex),"Local\\Linrad-FUNcube-%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x",
        container.Data1,container.Data2,container.Data3,container.Data4[0],container.Data4[1],container.Data4[2],container.Data4[3],
        container.Data4[4],container.Data4[5],container.Data4[6],container.Data4[7]);
    f->lock=CreateMutexA(NULL,FALSE,mutex);
    DWORD lock_error=GetLastError();
    f->hid=hid_open_path(selected->path);
    hid_free_enumeration(list);g_free(copy);
    if (!f->lock || lock_error==ERROR_ALREADY_EXISTS || !f->hid) goto fail;
    unsigned char reply[64];
    for (int i=0;i<64 && hid_read_timeout(f->hid,reply,sizeof(reply),0)>0;++i) {}
    if (command(f,1,0,reply) || memcmp(reply+2,"FCDAPP",6)) goto fail;
    HRESULT com=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    if (FAILED(com) && com!=RPC_E_CHANGED_MODE) goto fail;
    f->com_initialized=SUCCEEDED(com);
    if (Pa_Initialize()!=paNoError) goto fail;
    f->audio_initialized=1;
    PaDeviceIndex audio=matching_audio(&container);
    if (audio==paNoDevice) {fputs("FUNcube: matching WASAPI capture endpoint not found\n",stderr);goto fail;}
    PaWasapiStreamInfo wasapi={0};
    wasapi.size=sizeof(wasapi);wasapi.hostApiType=paWASAPI;wasapi.version=1;
    wasapi.flags=paWinWasapiExclusive | paWinWasapiExplicitSampleFormat;
    PaStreamParameters input={audio,2,paInt16,.1,&wasapi};
    PaError error=Pa_OpenStream(&f->pcm,&input,NULL,FUNCUBE_RATE,0,paClipOff|paDitherOff,NULL,NULL);
    if (error) {fprintf(stderr,"FUNcube 192000 Hz stereo IQ: %s\n",Pa_GetErrorText(error));goto fail;}
    if (funcube_frequency(f,frequency) || command(f,110,lna,NULL) || command(f,114,mixer,NULL) ||
        funcube_gain(f,gain) || command(f,122,0,NULL) || Pa_StartStream(f->pcm)) goto fail;
    fprintf(stderr,"[SDR] FUNcube Pro+: exclusive WASAPI, 192000 samples/s, IF gain %.0f dB\n",gain);
    return f;
fail:
    fputs("FUNcube: cannot open/control receiver; check device access and exclusive audio availability\n",stderr);
    funcube_close(f);return NULL;
}
int funcube_read(Funcube *f,int16_t *iq,size_t count) {
    long available=Pa_GetStreamReadAvailable(f->pcm);
    if (available<0) return -1;
    if (!available) {Pa_Sleep(5);return 0;}
    if (count>(size_t)available) count=available;
    PaError error=Pa_ReadStream(f->pcm,iq,count);
    if (error==paInputOverflowed) {
        fprintf(stderr,"[SDR] FUNcube input overflow #%lu; samples lost\n",++f->overruns);
    } else if (error) {fprintf(stderr,"FUNcube capture: %s\n",Pa_GetErrorText(error));return -1;}
    return (int)count;
}
