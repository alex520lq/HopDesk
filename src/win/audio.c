#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#endif
#include <mmdeviceapi.h>
#include <stdlib.h>
#include <string.h>
#include "../platform/audio_link.h"

#define CAPTURE_RATE 44100u
#define CONVERT_PCM (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY)

/* The Microsoft C headers declare these IDs without defining them. */
#if defined(_MSC_VER)
DEFINE_GUID(IID_IAudioClient, 0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);
DEFINE_GUID(IID_IAudioCaptureClient, 0xc8adbd64, 0xe71e, 0x48a0, 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17);
DEFINE_GUID(IID_IAudioRenderClient, 0xf294acfc, 0x3146, 0x4483, 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2);
DEFINE_GUID(IID_IActivateAudioInterfaceCompletionHandler, 0x41d949ab, 0x9862, 0x444a, 0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb);
DEFINE_GUID(IID_IMMDeviceEnumerator, 0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);
DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
#endif

/* MinGW headers omit the documented process-loopback activation blob. */
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#define AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK 1
#define PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE 1
typedef struct {
    DWORD ActivationType;
    struct {
        DWORD TargetProcessId;
        DWORD ProcessLoopbackMode;
    } ProcessLoopbackParams;
} AUDIOCLIENT_ACTIVATION_PARAMS;
#endif

struct hop_native_audio {
    IAudioClient *input, *output;
    IAudioCaptureClient *capture;
    IAudioRenderClient *render;
    HANDLE capture_event;
    const BYTE *pending;
    UINT32 pending_frames, pending_read;
    uint32_t output_rate;
    UINT32 output_capacity;
};

typedef struct {
    IActivateAudioInterfaceCompletionHandler iface;
    LONG refs;
    HANDLE done;
    AUDIOCLIENT_ACTIVATION_PARAMS params;
    PROPVARIANT activation;
    HRESULT result;
    IAudioClient *client;
} capture_activation;

static ULONG STDMETHODCALLTYPE activation_addref(IActivateAudioInterfaceCompletionHandler *iface) {
    capture_activation *a = (capture_activation *)iface;
    return (ULONG)InterlockedIncrement(&a->refs);
}

static ULONG STDMETHODCALLTYPE activation_release(IActivateAudioInterfaceCompletionHandler *iface) {
    capture_activation *a = (capture_activation *)iface;
    ULONG refs = (ULONG)InterlockedDecrement(&a->refs);
    if (!refs) {
        if (a->client) IAudioClient_Release(a->client);
        CloseHandle(a->done);
        free(a);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE activation_query(IActivateAudioInterfaceCompletionHandler *iface,
                                                   REFIID iid, void **out) {
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IAgileObject) &&
        !IsEqualIID(iid, &IID_IActivateAudioInterfaceCompletionHandler)) return E_NOINTERFACE;
    *out = iface;
    activation_addref(iface);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE activation_complete(IActivateAudioInterfaceCompletionHandler *iface,
                                                      IActivateAudioInterfaceAsyncOperation *op) {
    capture_activation *a = (capture_activation *)iface;
    IUnknown *client = NULL;
    HRESULT result = E_FAIL;
    HRESULT hr = IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &result, &client);
    if (SUCCEEDED(hr)) hr = result;
    if (SUCCEEDED(hr) && client)
        hr = IUnknown_QueryInterface(client, &IID_IAudioClient, (void **)&a->client);
    else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
    if (client) IUnknown_Release(client);
    a->result = hr;
    SetEvent(a->done);
    return S_OK;
}

static IActivateAudioInterfaceCompletionHandlerVtbl activation_vtable = {
    activation_query, activation_addref, activation_release, activation_complete
};

static HRESULT activate_capture(IAudioClient **client) {
    capture_activation *a = calloc(1, sizeof(*a));
    if (!a) return E_OUTOFMEMORY;
    a->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!a->done) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        free(a);
        return hr;
    }
    a->iface.lpVtbl = &activation_vtable;
    a->refs = 1;
    a->params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    a->params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    a->params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    a->activation.vt = VT_BLOB;
    a->activation.blob.cbSize = sizeof(a->params);
    a->activation.blob.pBlobData = (BYTE *)&a->params;
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, &IID_IAudioClient,
                                              &a->activation, &a->iface, &op);
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObject(a->done, 3000) == WAIT_OBJECT_0) {
            hr = a->result;
            *client = a->client;
            a->client = NULL;
        } else hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    if (op) IActivateAudioInterfaceAsyncOperation_Release(op);
    activation_release(&a->iface);
    return hr;
}

static WAVEFORMATEX pcm_format(uint32_t rate) {
    WAVEFORMATEX format = {
        .wFormatTag = WAVE_FORMAT_PCM, .nChannels = 2, .nSamplesPerSec = rate,
        .nAvgBytesPerSec = rate * HOP_AUDIO_FRAME_BYTES, .nBlockAlign = HOP_AUDIO_FRAME_BYTES,
        .wBitsPerSample = 16
    };
    return format;
}

static void stop_capture(hop_native_audio *a) {
    if (a->pending_frames) {
        IAudioCaptureClient_ReleaseBuffer(a->capture, a->pending_frames);
        a->pending_frames = 0;
    }
    if (a->input) IAudioClient_Stop(a->input);
    if (a->capture) IAudioCaptureClient_Release(a->capture);
    if (a->input) IAudioClient_Release(a->input);
    if (a->capture_event) CloseHandle(a->capture_event);
    a->capture_event = NULL;
    a->capture = NULL;
    a->input = NULL;
}

static HRESULT start_capture(hop_native_audio *a) {
    HRESULT hr = activate_capture(&a->input);
    if (FAILED(hr)) return hr;
    WAVEFORMATEX format = pcm_format(CAPTURE_RATE);
    hr = IAudioClient_Initialize(a->input, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | CONVERT_PCM, 0, 0, &format, NULL);
    if (FAILED(hr)) return hr;
    a->capture_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!a->capture_event) return HRESULT_FROM_WIN32(GetLastError());
    hr = IAudioClient_SetEventHandle(a->input, a->capture_event);
    if (FAILED(hr)) return hr;
    hr = IAudioClient_GetService(a->input, &IID_IAudioCaptureClient, (void **)&a->capture);
    if (FAILED(hr)) return hr;
    return IAudioClient_Start(a->input);
}

hop_native_audio *hop_native_audio_open(unsigned *directions) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        hop_log(HOP_LOG_WARN, "audio: COM initialization failed (0x%08lx)", (unsigned long)hr);
        return NULL;
    }
    hop_native_audio *a = calloc(1, sizeof(*a));
    if (!a) {
        CoUninitialize();
        return NULL;
    }
    if (*directions & HOP_AUDIO_SEND) {
        hr = start_capture(a);
        if (FAILED(hr)) {
            hop_log(HOP_LOG_WARN, "audio: WASAPI process loopback failed (0x%08lx)", (unsigned long)hr);
            stop_capture(a);
            *directions &= ~HOP_AUDIO_SEND;
        }
    }
    return a;
}

int hop_native_audio_capture(hop_native_audio *a, int16_t *pcm, uint32_t *rate) {
    HRESULT hr;
    if (!a->pending_frames) {
        UINT32 available = 0;
        hr = IAudioCaptureClient_GetNextPacketSize(a->capture, &available);
        if (FAILED(hr)) goto fail;
        if (!available) return 0;
        BYTE *bytes = NULL;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = IAudioCaptureClient_GetBuffer(a->capture, &bytes, &frames, &flags, NULL, NULL);
        if (FAILED(hr)) goto fail;
        if (!frames) return 0;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            hr = IAudioCaptureClient_ReleaseBuffer(a->capture, frames);
            if (FAILED(hr)) goto fail;
            return 0;
        }
        a->pending = bytes;
        a->pending_frames = frames;
        a->pending_read = 0;
    }
    unsigned frames = a->pending_frames - a->pending_read;
    if (frames > HOP_AUDIO_FRAMES) frames = HOP_AUDIO_FRAMES;
    memcpy(pcm, a->pending + a->pending_read * HOP_AUDIO_FRAME_BYTES,
           frames * HOP_AUDIO_FRAME_BYTES);
    a->pending_read += frames;
    if (a->pending_read == a->pending_frames) {
        hr = IAudioCaptureClient_ReleaseBuffer(a->capture, a->pending_frames);
        a->pending_frames = 0;
        if (FAILED(hr)) goto fail;
    }
    *rate = CAPTURE_RATE;
    return (int)frames;
fail:
    hop_log(HOP_LOG_WARN, "audio: WASAPI capture interrupted (0x%08lx)", (unsigned long)hr);
    return -1;
}

static void stop_playback(hop_native_audio *a) {
    if (a->output) IAudioClient_Stop(a->output);
    if (a->render) IAudioRenderClient_Release(a->render);
    if (a->output) IAudioClient_Release(a->output);
    a->render = NULL;
    a->output = NULL;
}

static HRESULT open_playback(hop_native_audio *a, uint32_t rate) {
    IMMDeviceEnumerator *enumerator = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr)) return hr;
    IMMDevice *device = NULL;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eMultimedia, &device);
    IMMDeviceEnumerator_Release(enumerator);
    if (FAILED(hr)) return hr;
    hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&a->output);
    IMMDevice_Release(device);
    if (FAILED(hr)) return hr;
    WAVEFORMATEX format = pcm_format(rate);
    hr = IAudioClient_Initialize(a->output, AUDCLNT_SHAREMODE_SHARED, CONVERT_PCM, 400000, 0, &format, NULL);
    if (FAILED(hr)) return hr;
    hr = IAudioClient_GetBufferSize(a->output, &a->output_capacity);
    if (FAILED(hr)) return hr;
    hr = IAudioClient_GetService(a->output, &IID_IAudioRenderClient, (void **)&a->render);
    if (FAILED(hr)) return hr;
    a->output_rate = rate;
    return S_OK;
}

int hop_native_audio_play(hop_native_audio *a, const int16_t *pcm, unsigned frames, uint32_t rate) {
    HRESULT hr;
    int starting = !a->output || a->output_rate != rate;
    if (starting) {
        stop_playback(a);
        hr = open_playback(a, rate);
        if (FAILED(hr)) goto fail;
    }
    UINT32 padding = 0;
    hr = IAudioClient_GetCurrentPadding(a->output, &padding);
    if (FAILED(hr)) goto fail;
    if (padding > a->output_capacity || frames > a->output_capacity - padding || padding > rate / 25) return 1;
    unsigned silence = starting ? rate / 50 : 0;
    if (silence > a->output_capacity - padding - frames) silence = 0;
    BYTE *out = NULL;
    hr = IAudioRenderClient_GetBuffer(a->render, frames + silence, &out);
    if (FAILED(hr)) goto fail;
    memset(out, 0, silence * HOP_AUDIO_FRAME_BYTES);
    memcpy(out + silence * HOP_AUDIO_FRAME_BYTES, pcm, frames * HOP_AUDIO_FRAME_BYTES);
    hr = IAudioRenderClient_ReleaseBuffer(a->render, frames + silence, 0);
    if (FAILED(hr)) goto fail;
    if (starting) hr = IAudioClient_Start(a->output);
    if (SUCCEEDED(hr)) return 1;
fail:
    hop_log(HOP_LOG_WARN, "audio: WASAPI playback failed (0x%08lx)", (unsigned long)hr);
    stop_playback(a);
    return 0;
}

void hop_native_audio_close(hop_native_audio *a) {
    stop_capture(a);
    stop_playback(a);
    free(a);
    CoUninitialize();
}
