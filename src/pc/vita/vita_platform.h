#ifndef VITA_PLATFORM_H
#define VITA_PLATFORM_H

#ifdef TARGET_VITA

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read-only: this is where the contents of the VPK end up (lang, mods, palettes, dynos)
#define VITA_RESOURCE_PATH "app0:"
// Read/write: config, saves, the ROM, user mods, caches
#define VITA_USER_PATH     "ux0:data/sm64coopdx"
// The ROM has to be placed in VITA_USER_PATH by the user (any *.z64 name works)
#define VITA_ROM_FILENAME  "baserom.us.z64"
// stdout/stderr end up in this file inside VITA_USER_PATH
#define VITA_LOG_FILE      "vita_log.txt"

#define VITA_SCREEN_WIDTH  960
#define VITA_SCREEN_HEIGHT 544

// Has to be the first thing main() does: clocks, system modules, network, user folder, logging
void vita_init(void);

// Called right before the GL context gets created (configures vitaGL)
void vita_gl_pre_init(void);
// Called once the GL context exists
void vita_gl_post_init(void);

// Called once per frame from the main thread.
// Shows fatal errors that were raised on other threads.
void vita_update(void);

const char *vita_user_path(void);

// Shows a message (system dialog if the screen is already up) and terminates the process
void vita_fatal(const char *msg) __attribute__((noreturn));

// Debugging aid: true when the file VITA_USER_PATH/debug_no_shaders.txt exists. Shader compile
// errors are then ignored (nothing gets drawn) instead of being fatal, which allows running the
// game logic in an emulator that lacks the shader compiler module.
bool vita_debug_no_shaders(void);

// Debugging aid: true when the file VITA_USER_PATH/debug_autohost.txt exists. The game then
// skips the main menu and hosts a game immediately (for automated emulator runs, together with
// debug_warp.txt "<seconds> <level>" and debug_exit_after.txt "<seconds>").
bool vita_debug_autohost(void);

// Prints free memory of every budget to the log, prefixed with `tag`
void vita_log_memory(const char *tag);

// Writes the console's IPv4 address into `out`, returns false when there is no connection
bool vita_get_ip_address(char *out, unsigned int outSize);

// Frame time statistics: microseconds spent per section (or plain counters for TICKS/FRAMES),
// accumulated by pc_main.c and printed as per-tick averages (then reset) with the heartbeat
enum VitaPerfSection {
    VITA_PERF_TICKS,    // counter: 30 Hz game updates
    VITA_PERF_FRAMES,   // counter: frames drawn
    VITA_PERF_NETWORK,
    VITA_PERF_GAME,     // game logic, including building the display list
    VITA_PERF_LUA,
    VITA_PERF_AUDIO,    // audio synthesis
    VITA_PERF_GFX,      // display list -> triangles -> vitaGL, per drawn frame
    VITA_PERF_SWAP,     // buffer swap, includes waiting for vsync
    VITA_PERF_DELAY,    // idle time when the frame rate is capped
    // renderer backend (gfx_vitagl.c), these are part of VITA_PERF_GFX
    VITA_PERF_DRAW,          // time inside vitaGL submitting vertex data and draw calls
    VITA_PERF_DRAW_CALLS,    // counter
    VITA_PERF_TRIANGLES,     // counter
    VITA_PERF_TEX_UPLOAD,    // time spent uploading textures
    VITA_PERF_TEX_UPLOADS,   // counter
    VITA_PERF_SHADER_BUILD,  // time spent generating/compiling shaders
    VITA_PERF_SHADER_BUILDS, // counter
    // inside VITA_PERF_GFX: where the display list processing spends its time (gfx_pc.c).
    // The *_TIME values are sampled: one call in eight is timed and counted eight times.
    VITA_PERF_EVENTS,        // window/input event pump at the start of a frame
    VITA_PERF_INTERP,        // frame interpolation patches
    VITA_PERF_VTX_TIME,      // gfx_sp_vertex: transform and light vertices
    VITA_PERF_VTX_CALLS,     // counter
    VITA_PERF_VTX_COUNT,     // counter: vertices
    VITA_PERF_TRI_TIME,      // gfx_sp_tri1: cull, pick shader/textures, emit vertex data
    VITA_PERF_TRI_CALLS,     // counter: triangles before culling
    VITA_PERF_MTX_TIME,      // gfx_sp_matrix
    VITA_PERF_MTX_CALLS,     // counter
    VITA_PERF_MAX
};
extern unsigned long long gVitaPerf[VITA_PERF_MAX];
unsigned long long vita_perf_now(void);
static inline void vita_perf_add(enum VitaPerfSection section, unsigned long long value) {
    gVitaPerf[section] += value;
}

// Times one out of eight executions of `code` and books it eight-fold, so that code that runs
// thousands of times per frame can be profiled without the timer calls distorting the result.
#define VITA_PERF_SAMPLED(timeSection, code) { \
    static unsigned int _vitaPerfCalls = 0; \
    if (((++_vitaPerfCalls) & 7) == 0) { \
        unsigned long long _vitaPerfStart = vita_perf_now(); \
        code; \
        gVitaPerf[timeSection] += (vita_perf_now() - _vitaPerfStart) * 8; \
    } else { \
        code; \
    } \
}

// Audio statistics, filled in by buffer_audio() and printed (then reset) with the heartbeat
struct VitaAudioStats {
    int peak;             // loudest sample the game synthesized since the last heartbeat (0 - 32767)
    unsigned int buffers; // number of audio buffers generated since the last heartbeat
    int buffered;         // samples waiting in the output queue
    float masterVolume;
    bool muted;
    bool nullDevice;      // true if the audio device could not be opened
    // native backend (audio_vita.c)
    unsigned int blocks;    // blocks handed to sceAudioOutOutput since the last heartbeat
    unsigned int underruns; // blocks that had to be silence because the game delivered too little
    int outputError;        // last error code returned by sceAudioOutOutput, 0 if none
};
extern struct VitaAudioStats gVitaAudioStats;

#ifdef __cplusplus
}
#endif

#endif // TARGET_VITA

#endif // VITA_PLATFORM_H
