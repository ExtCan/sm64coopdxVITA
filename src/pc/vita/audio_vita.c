#ifdef TARGET_VITA

// Native PS Vita audio backend.
//
// The game produces 32 kHz stereo audio. SDL2 would play that through the Vita's "BGM" port
// (the only one that accepts sample rates other than 48 kHz), which the system mutes in favour
// of its own background music and which turned out to stay silent on real hardware.
// This backend uses the MAIN port instead, like commercial games do: a small thread pulls the
// game's audio out of a ring buffer, resamples it from 32 kHz to 48 kHz (exactly 2:3, linear
// interpolation) and hands it to sceAudioOutOutput(), which blocks until the block was played.

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include "pc/audio/audio_api.h"
#include "vita_platform.h"

#define VITA_AUDIO_OUT_RATE    48000
#define VITA_AUDIO_OUT_GRAIN   1024                 // frames per output block (21 ms)
#define VITA_AUDIO_RING_FRAMES 16384                // 32 kHz frames, power of two
#define VITA_AUDIO_RING_MASK   (VITA_AUDIO_RING_FRAMES - 1)
#define VITA_AUDIO_MAX_QUEUED  6000                 // same limit as the SDL backend

static int sPort = -1;
static pthread_t sThread;
static pthread_mutex_t sMutex;
static bool sMutexReady = false;
static volatile bool sRunning = false;

// ring buffer of interleaved stereo frames at 32 kHz
static int16_t sRing[VITA_AUDIO_RING_FRAMES * 2];
static unsigned int sRingRead = 0;
static unsigned int sRingCount = 0;

// resampler state: the two input frames being interpolated and the position between them in thirds
static int16_t sCur[2] = { 0, 0 };
static int16_t sNext[2] = { 0, 0 };
static int sFrac = 0;

// sceAudioOutOutput() does not copy: it returns once the previous block is done and then keeps
// playing straight out of the buffer it was given. The next block therefore has to be prepared
// in a different buffer, or the one being played gets overwritten (which sounds like heavy crackling).
#define VITA_AUDIO_NUM_BUFFERS 2
static int16_t sOutBuffers[VITA_AUDIO_NUM_BUFFERS][VITA_AUDIO_OUT_GRAIN * 2] __attribute__((aligned(64)));

// Produces one block of 48 kHz audio. Called with the mutex held.
static void audio_vita_fill_block(int16_t *out) {
    bool underrun = false;

    for (int i = 0; i < VITA_AUDIO_OUT_GRAIN; i++) {
        out[i * 2 + 0] = (int16_t)(sCur[0] + ((sNext[0] - sCur[0]) * sFrac) / 3);
        out[i * 2 + 1] = (int16_t)(sCur[1] + ((sNext[1] - sCur[1]) * sFrac) / 3);

        // advance by 2/3 of an input frame
        sFrac += 2;
        if (sFrac >= 3) {
            sFrac -= 3;
            sCur[0] = sNext[0];
            sCur[1] = sNext[1];
            if (sRingCount > 0) {
                sNext[0] = sRing[sRingRead * 2 + 0];
                sNext[1] = sRing[sRingRead * 2 + 1];
                sRingRead = (sRingRead + 1) & VITA_AUDIO_RING_MASK;
                sRingCount--;
            } else {
                // the game did not deliver enough audio in time: glide to silence
                sNext[0] = sNext[1] = 0;
                underrun = true;
            }
        }
    }

    if (underrun) { gVitaAudioStats.underruns++; }
}

static void *audio_vita_thread(void *arg) {
    (void) arg;

    // audio must not stutter when the main thread is busy: run slightly above default priority
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    SceUID thid = sceKernelGetThreadId();
    if (sceKernelGetThreadInfo(thid, &info) == 0) {
        sceKernelChangeThreadPriority(thid, info.currentPriority - 8);
    }

    int bufferIndex = 0;
    while (sRunning) {
        int16_t *buffer = sOutBuffers[bufferIndex];
        bufferIndex = (bufferIndex + 1) % VITA_AUDIO_NUM_BUFFERS;

        pthread_mutex_lock(&sMutex);
        audio_vita_fill_block(buffer);
        pthread_mutex_unlock(&sMutex);

        // blocks until the previous block has been played
        int ret = sceAudioOutOutput(sPort, buffer);
        gVitaAudioStats.blocks++;
        if (ret < 0) {
            gVitaAudioStats.outputError = ret;
            // do not spin if the port is broken
            sceKernelDelayThread(20 * 1000);
        }
    }

    return NULL;
}

static bool audio_vita_init(void) {
    sPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, VITA_AUDIO_OUT_GRAIN, VITA_AUDIO_OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (sPort < 0) {
        printf("[vita] audio: sceAudioOutOpenPort failed: 0x%08X\n", (unsigned int) sPort);
        return false;
    }

    int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    int ret = sceAudioOutSetVolume(sPort, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, volume);
    if (ret < 0) {
        printf("[vita] audio: sceAudioOutSetVolume failed: 0x%08X\n", (unsigned int) ret);
    }

    if (pthread_mutex_init(&sMutex, NULL) != 0) {
        sceAudioOutReleasePort(sPort);
        sPort = -1;
        return false;
    }
    sMutexReady = true;

    sRingRead = 0;
    sRingCount = 0;
    sRunning = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    int err = pthread_create(&sThread, &attr, audio_vita_thread, NULL);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        printf("[vita] audio: could not create the audio thread (%d)\n", err);
        sRunning = false;
        sceAudioOutReleasePort(sPort);
        sPort = -1;
        return false;
    }

    printf("[vita] audio: native output on the MAIN port (port %d, %d Hz, %d frames per block)\n",
        sPort, VITA_AUDIO_OUT_RATE, VITA_AUDIO_OUT_GRAIN);
    return true;
}

static int audio_vita_buffered(void) {
    // an unlocked read is fine here, this is only used as a pacing hint
    return (int) sRingCount;
}

static int audio_vita_get_desired_buffered(void) {
    // The game tops the queue up to roughly this many 32 kHz frames once per 30 Hz tick, and one
    // output block drains 683 of them: keep a little more headroom than the SDL backend (1100)
    return 1500;
}

static void audio_vita_play(const uint8_t *buf, size_t len) {
    if (!sRunning) { return; }

    const int16_t *frames = (const int16_t *) buf;
    unsigned int numFrames = (unsigned int)(len / 4);

    pthread_mutex_lock(&sMutex);
    // don't let the queue grow without bounds if the game runs ahead of the output
    if (sRingCount < VITA_AUDIO_MAX_QUEUED) {
        unsigned int space = VITA_AUDIO_RING_FRAMES - sRingCount;
        if (numFrames > space) { numFrames = space; }
        unsigned int write = (sRingRead + sRingCount) & VITA_AUDIO_RING_MASK;
        for (unsigned int i = 0; i < numFrames; i++) {
            sRing[write * 2 + 0] = frames[i * 2 + 0];
            sRing[write * 2 + 1] = frames[i * 2 + 1];
            write = (write + 1) & VITA_AUDIO_RING_MASK;
        }
        sRingCount += numFrames;
    }
    pthread_mutex_unlock(&sMutex);
}

static void audio_vita_shutdown(void) {
    if (sRunning) {
        sRunning = false;
        pthread_join(sThread, NULL);
    }
    if (sPort >= 0) {
        sceAudioOutReleasePort(sPort);
        sPort = -1;
    }
    if (sMutexReady) {
        pthread_mutex_destroy(&sMutex);
        sMutexReady = false;
    }
}

struct AudioAPI audio_vita = {
    audio_vita_init,
    audio_vita_buffered,
    audio_vita_get_desired_buffered,
    audio_vita_play,
    audio_vita_shutdown
};

#endif // TARGET_VITA
