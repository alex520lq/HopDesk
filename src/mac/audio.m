#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#include "../platform/audio_link.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RING_FRAMES 8192u

/* Each ring has one producer and one consumer. The realtime callbacks never
 * wait on the network, allocate, or take a lock. */
typedef struct {
    _Atomic unsigned read, write;
    int16_t pcm[RING_FRAMES][2];
} audio_ring;

struct hop_native_audio {
    AudioObjectID tap, device;
    AudioDeviceIOProcID io;
    uint32_t capture_rate;
    audio_ring capture;
    AudioQueueRef queue;
    uint32_t output_rate;
    unsigned output_frames;
    audio_ring playback;
};

static unsigned ring_read(audio_ring *r, int16_t *dst, unsigned count, unsigned backlog) {
    unsigned read = atomic_load_explicit(&r->read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(&r->write, memory_order_acquire);
    unsigned available = write - read;
    if (available > backlog) {
        read = write - backlog;
        available = backlog;
    }
    if (count > available) count = available;
    for (unsigned i = 0; i < count; ++i) {
        unsigned slot = (read + i) & (RING_FRAMES - 1);
        dst[2 * i] = r->pcm[slot][0];
        dst[2 * i + 1] = r->pcm[slot][1];
    }
    atomic_store_explicit(&r->read, read + count, memory_order_release);
    return count;
}

static unsigned ring_space(audio_ring *r, unsigned write) {
    return RING_FRAMES - (write - atomic_load_explicit(&r->read, memory_order_acquire));
}

static int16_t to_pcm(float x) {
    if (x != x) return 0;
    if (x <= -1.0f) return -32768;
    if (x >= 1.0f) return 32767;
    return (int16_t)(x * 32767.0f);
}

static OSStatus capture_input(AudioObjectID device, const AudioTimeStamp *now,
                              const AudioBufferList *input, const AudioTimeStamp *input_time,
                              AudioBufferList *output, const AudioTimeStamp *output_time, void *arg) {
    (void)device; (void)now; (void)input_time; (void)output; (void)output_time;
    hop_native_audio *audio = arg;
    if (!input || !input->mNumberBuffers) return noErr;
    const AudioBuffer *left = &input->mBuffers[0];
    if (!left->mData || !left->mNumberChannels) return noErr;
    const AudioBuffer *right = left;
    unsigned right_channel = left->mNumberChannels > 1 ? 1 : 0;
    if (left->mNumberChannels == 1 && input->mNumberBuffers > 1) right = &input->mBuffers[1];
    if (!right->mData || !right->mNumberChannels) return noErr;
    const float *l = left->mData, *r = right->mData;
    unsigned count = left->mDataByteSize / sizeof(float) / left->mNumberChannels;
    unsigned right_count = right->mDataByteSize / sizeof(float) / right->mNumberChannels;
    if (count > right_count) count = right_count;
    audio_ring *ring = &audio->capture;
    unsigned write = atomic_load_explicit(&ring->write, memory_order_relaxed);
    unsigned space = ring_space(ring, write);
    unsigned skip = count > space ? count - space : 0;
    for (unsigned i = skip; i < count; ++i) {
        unsigned slot = write++ & (RING_FRAMES - 1);
        ring->pcm[slot][0] = to_pcm(l[i * left->mNumberChannels]);
        ring->pcm[slot][1] = to_pcm(r[i * right->mNumberChannels + right_channel]);
    }
    atomic_store_explicit(&ring->write, write, memory_order_release);
    return noErr;
}

static void stop_capture(hop_native_audio *a) {
    if (@available(macOS 14.2, *)) {
        if (a->io) {
            AudioDeviceStop(a->device, a->io);
            AudioDeviceDestroyIOProcID(a->device, a->io);
            a->io = NULL;
        }
        if (a->device) {
            AudioHardwareDestroyAggregateDevice(a->device);
            a->device = 0;
        }
        if (a->tap) {
            AudioHardwareDestroyProcessTap(a->tap);
            a->tap = 0;
        }
    }
}

API_AVAILABLE(macos(14.2))
static OSStatus start_capture(hop_native_audio *a) {
    pid_t pid = getpid();
    AudioObjectID own_process = kAudioObjectUnknown;
    UInt32 size = sizeof(own_process);
    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyTranslatePIDToProcessObject,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, sizeof(pid),
                                                &pid, &size, &own_process);
    if (status != noErr || own_process == kAudioObjectUnknown) return status ? status : kAudioHardwareBadObjectError;

    CATapDescription *description = [[CATapDescription alloc]
        initStereoGlobalTapButExcludeProcesses:@[@(own_process)]];
    description.name = @"HopDesk System Audio";
    description.privateTap = YES;
    status = AudioHardwareCreateProcessTap(description, &a->tap);
    if (status != noErr) return status;

    AudioStreamBasicDescription format = {0};
    addr.mSelector = kAudioTapPropertyFormat;
    size = sizeof(format);
    status = AudioObjectGetPropertyData(a->tap, &addr, 0, NULL, &size, &format);
    if (status != noErr) return status;
    if (format.mFormatID != kAudioFormatLinearPCM ||
        !(format.mFormatFlags & kAudioFormatFlagIsFloat) || format.mBitsPerChannel != 32 ||
        !(format.mSampleRate >= 8000 && format.mSampleRate <= 192000))
        return kAudioHardwareUnsupportedOperationError;
    a->capture_rate = (uint32_t)format.mSampleRate;

    NSDictionary *tap = @{
        @kAudioSubTapUIDKey: description.UUID.UUIDString,
        @kAudioSubTapDriftCompensationKey: @YES
    };
    NSDictionary *aggregate = @{
        @kAudioAggregateDeviceNameKey: @"HopDesk Audio Capture",
        @kAudioAggregateDeviceUIDKey: NSUUID.UUID.UUIDString,
        @kAudioAggregateDeviceIsPrivateKey: @YES,
        @kAudioAggregateDeviceTapListKey: @[tap]
    };
    status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate, &a->device);
    if (status != noErr) return status;
    status = AudioDeviceCreateIOProcID(a->device, capture_input, a, &a->io);
    if (status != noErr) return status;
    return AudioDeviceStart(a->device, a->io);
}

hop_native_audio *hop_native_audio_open(unsigned *directions) {
    hop_native_audio *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    atomic_init(&a->capture.read, 0);
    atomic_init(&a->capture.write, 0);
    atomic_init(&a->playback.read, 0);
    atomic_init(&a->playback.write, 0);
    if (*directions & HOP_AUDIO_SEND) {
        OSStatus status = kAudioHardwareUnsupportedOperationError;
        if (@available(macOS 14.2, *)) {
            @autoreleasepool { status = start_capture(a); }
        }
        if (status != noErr) {
            hop_log(HOP_LOG_WARN, "audio: macOS system capture failed (%d); check system audio recording permission",
                    (int)status);
            stop_capture(a);
            *directions &= ~HOP_AUDIO_SEND;
        }
    }
    return a;
}

int hop_native_audio_capture(hop_native_audio *a, int16_t *pcm, uint32_t *rate) {
    *rate = a->capture_rate;
    return (int)ring_read(&a->capture, pcm, HOP_AUDIO_FRAMES, a->capture_rate * 3 / 100);
}

static OSStatus enqueue_output(hop_native_audio *a, AudioQueueRef queue, AudioQueueBufferRef buffer,
                               int silence) {
    int16_t *pcm = buffer->mAudioData;
    unsigned frames = silence ? 0 :
        ring_read(&a->playback, pcm, a->output_frames, a->output_rate * 3 / 100);
    memset(pcm + frames * 2, 0, (a->output_frames - frames) * HOP_AUDIO_FRAME_BYTES);
    buffer->mAudioDataByteSize = a->output_frames * HOP_AUDIO_FRAME_BYTES;
    return AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

static void playback_output(void *arg, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    (void)enqueue_output(arg, queue, buffer, 0);
}

static void stop_playback(hop_native_audio *a) {
    if (!a->queue) return;
    AudioQueueStop(a->queue, true);
    AudioQueueDispose(a->queue, true);
    a->queue = NULL;
    atomic_store_explicit(&a->playback.read, 0, memory_order_relaxed);
    atomic_store_explicit(&a->playback.write, 0, memory_order_relaxed);
}

static OSStatus open_playback(hop_native_audio *a, uint32_t rate) {
    AudioStreamBasicDescription format = {
        .mSampleRate = rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = HOP_AUDIO_FRAME_BYTES,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = HOP_AUDIO_FRAME_BYTES,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 16
    };
    a->output_rate = rate;
    a->output_frames = rate / 100;
    return AudioQueueNewOutput(&format, playback_output, a, NULL, NULL, 0, &a->queue);
}

static OSStatus start_playback(hop_native_audio *a) {
    for (int i = 0; i < 3; ++i) {
        AudioQueueBufferRef buffer;
        OSStatus status = AudioQueueAllocateBuffer(a->queue, a->output_frames * HOP_AUDIO_FRAME_BYTES, &buffer);
        if (status != noErr) return status;
        status = enqueue_output(a, a->queue, buffer, i < 2);
        if (status != noErr) return status;
    }
    return AudioQueueStart(a->queue, NULL);
}

int hop_native_audio_play(hop_native_audio *a, const int16_t *pcm, unsigned count, uint32_t rate) {
    int starting = !a->queue || a->output_rate != rate;
    OSStatus status = noErr;
    if (starting) {
        stop_playback(a);
        status = open_playback(a, rate);
        if (status != noErr) goto fail;
    }
    audio_ring *ring = &a->playback;
    unsigned write = atomic_load_explicit(&ring->write, memory_order_relaxed);
    unsigned space = ring_space(ring, write);
    if (count > space) count = space;
    for (unsigned i = 0; i < count; ++i) {
        unsigned slot = (write + i) & (RING_FRAMES - 1);
        ring->pcm[slot][0] = pcm[2 * i];
        ring->pcm[slot][1] = pcm[2 * i + 1];
    }
    atomic_store_explicit(&ring->write, write + count, memory_order_release);
    if (starting) status = start_playback(a);
    if (status == noErr) return 1;
fail:
    hop_log(HOP_LOG_WARN, "audio: macOS playback failed (%d)", (int)status);
    stop_playback(a);
    return 0;
}

void hop_native_audio_close(hop_native_audio *a) {
    stop_capture(a);
    stop_playback(a);
    free(a);
}
