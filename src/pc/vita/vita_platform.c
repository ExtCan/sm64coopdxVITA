#ifdef TARGET_VITA

// PS Vita platform glue: process setup, memory budget, logging, fatal errors and the
// stubs for desktop-only features (update checker, Mumble link).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/stat.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <vitaGL.h>

#include "vita_platform.h"
#include "pc/update_checker.h"
#include "pc/mumble/mumble.h"

  ///////////////////
 // memory budget //
///////////////////

// The Vita gives an application a fixed budget (param.sfo ATTRIBUTE2=12 asks for the biggest one,
// 365 MiB of main memory). Everything below has to fit in there together with the executable
// itself, the remainder is taken by vitaGL for vertex data, shaders and textures that overflow
// the 112 MiB of video memory.

// malloc()/new: game pools, Lua, DynOS, mods, the ROM, decoded audio...
unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;
// heap of the system's own libc (SceLibc), used by system modules
unsigned int sceLibcHeapSize = 4 * 1024 * 1024;
// the game recurses fairly deep (geo layouts, Lua, DynOS parsers)
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
unsigned int _pthread_stack_default_user = 2 * 1024 * 1024;

#define VITA_NET_MEMORY_SIZE (1 * 1024 * 1024)

// VitaSDK's pthread_attr_init() leaves the stack size at 0, which pthread_create() then rounds up
// to a mere 4 KiB. Every thread that is created with an attribute object (the game's loading
// thread, miniaudio's worker threads) would overflow its stack right away, so the linker is told
// to route pthread_attr_init() through here (-Wl,--wrap=pthread_attr_init) to apply a real default.
#include <pthread.h>
int __real_pthread_attr_init(pthread_attr_t *attr);
int __wrap_pthread_attr_init(pthread_attr_t *attr) {
    int ret = __real_pthread_attr_init(attr);
    if (ret == 0) {
        pthread_attr_setstacksize(attr, _pthread_stack_default_user);
    }
    return ret;
}

struct VitaAudioStats gVitaAudioStats = { 0 };

unsigned long long gVitaPerf[VITA_PERF_MAX] = { 0 };
#define sPerf gVitaPerf

unsigned long long vita_perf_now(void) {
    return sceKernelGetProcessTimeWide();
}

static int sDebugExitAfterSeconds = 0;
static int sDebugWarpAfterSeconds = 0;
static int sDebugWarpLevel = 0;
static SceUID sMainThreadId = -1;
static bool sGlReady = false;
static volatile bool sFatalPending = false;
static char sFatalMessage[512] = "";

  /////////////
 // logging //
/////////////

static void vita_mkdir(const char *path) {
    sceIoMkdir(path, 0777);
}

static void vita_init_logging(void) {
    // keep the previous log around, it is the only trace of a crash on the last run
    rename(VITA_USER_PATH "/" VITA_LOG_FILE, VITA_USER_PATH "/vita_log_prev.txt");

    if (freopen(VITA_USER_PATH "/" VITA_LOG_FILE, "w", stdout)) {
        setvbuf(stdout, NULL, _IOLBF, 1024);
    }
    // a separate file: the same file can not be opened twice for writing
    if (freopen(VITA_USER_PATH "/vita_errors.txt", "w", stderr)) {
        setvbuf(stderr, NULL, _IONBF, 0);
    }
}

void vita_log_memory(const char *tag) {
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    sceKernelGetFreeMemorySize(&info);

    struct mallinfo mi = mallinfo();
    printf("[vita] memory (%s): heap used %u KiB (peak arena %u KiB) of %u KiB | free user %d KiB, cdram %d KiB, phycont %d KiB\n",
        tag,
        (unsigned int)(mi.uordblks / 1024), (unsigned int)(mi.arena / 1024), (unsigned int)(_newlib_heap_size_user / 1024),
        info.size_user / 1024, info.size_cdram / 1024, info.size_phycont / 1024);
    if (sGlReady) {
        printf("[vita] vitaGL pools free: ram %u KiB, vram %u KiB, phycont %u KiB\n",
            (unsigned int)(vglMemFree(VGL_MEM_RAM) / 1024),
            (unsigned int)(vglMemFree(VGL_MEM_VRAM) / 1024),
            (unsigned int)(vglMemFree(VGL_MEM_PHYCONT) / 1024));
    }
    fflush(stdout);
}

  //////////
 // init //
//////////

static void vita_init_network(void) {
    static char sNetMemory[VITA_NET_MEMORY_SIZE];

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    SceNetInitParam param;
    param.memory = sNetMemory;
    param.size = sizeof(sNetMemory);
    param.flags = 0;

    int ret = sceNetInit(&param);
    if (ret < 0) { printf("[vita] sceNetInit failed: 0x%08X\n", ret); }

    ret = sceNetCtlInit();
    if (ret < 0) { printf("[vita] sceNetCtlInit failed: 0x%08X\n", ret); }
}

bool vita_get_ip_address(char *out, unsigned int outSize) {
    SceNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) { return false; }
    if (info.ip_address[0] == '\0') { return false; }
    snprintf(out, outSize, "%s", info.ip_address);
    return true;
}

void vita_init(void) {
    sMainThreadId = sceKernelGetThreadId();

    // full speed ahead: 444 MHz CPU, 222 MHz bus/GPU
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    vita_mkdir("ux0:data");
    vita_mkdir(VITA_USER_PATH);
    vita_mkdir(VITA_USER_PATH "/mods");
    vita_mkdir(VITA_USER_PATH "/dynos");
    vita_mkdir(VITA_USER_PATH "/dynos/packs");
    vita_init_logging();

    // optional: VITA_USER_PATH/debug_warp.txt holds "<seconds> <level number>"
    FILE *warpFile = fopen(VITA_USER_PATH "/debug_warp.txt", "r");
    if (warpFile) {
        if (fscanf(warpFile, "%d %d", &sDebugWarpAfterSeconds, &sDebugWarpLevel) != 2) { sDebugWarpAfterSeconds = 0; }
        fclose(warpFile);
    }

    // optional: VITA_USER_PATH/debug_exit_after.txt holds a number of seconds
    FILE *exitFile = fopen(VITA_USER_PATH "/debug_exit_after.txt", "r");
    if (exitFile) {
        if (fscanf(exitFile, "%d", &sDebugExitAfterSeconds) != 1) { sDebugExitAfterSeconds = 0; }
        fclose(exitFile);
    }

    printf("[vita] sm64coopdx starting\n");
    vita_log_memory("startup");

    // the system dialogs (on screen keyboard, message boxes) need these
    SceAppUtilInitParam initParam;
    SceAppUtilBootParam bootParam;
    memset(&initParam, 0, sizeof(initParam));
    memset(&bootParam, 0, sizeof(bootParam));
    sceAppUtilInit(&initParam, &bootParam);

    SceCommonDialogConfigParam dialogParam;
    sceCommonDialogConfigParamInit(&dialogParam);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&dialogParam.language);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&dialogParam.enterButtonAssign);
    sceCommonDialogSetConfigParam(&dialogParam);

    vita_init_network();
}

void vita_gl_pre_init(void) {
    // -O3 style shader optimizations, fast math, but keep full float precision for UVs
    vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, SHARK_ENABLE, SHARK_DISABLE, SHARK_ENABLE);
    // let vitaGL use every memory budget the system offers
    vglUseExtraMem(GL_TRUE);
    // The renderer uploads a small vertex buffer per draw call (GL_STREAM_DRAW). Serve those from
    // vitaGL's per frame scratch pool instead of a locked heap allocation each time
    // (needs a vitaGL built with USE_SCRATCH_MEMORY=1, it is a no-op otherwise).
    vglSetCircularPoolSize(24 * 1024 * 1024); // total, split across the display buffers
    vglSetupScratchMemory(GL_FALSE, GL_TRUE);
    // compiled shaders are cached on the memory card when vitaGL was built with HAVE_SHADER_CACHE=1
    vglSetShaderCachePath(VITA_USER_PATH "/shader_cache");
    vita_log_memory("before GL init");
}

void vita_gl_post_init(void) {
    sGlReady = true;
    vita_log_memory("after GL init");
}

const char *vita_user_path(void) {
    return VITA_USER_PATH;
}

bool vita_debug_autohost(void) {
    struct stat st;
    return stat(VITA_USER_PATH "/debug_autohost.txt", &st) == 0;
}

bool vita_debug_no_shaders(void) {
    static int sNoShaders = -1;
    if (sNoShaders < 0) {
        struct stat st;
        sNoShaders = (stat(VITA_USER_PATH "/debug_no_shaders.txt", &st) == 0) ? 1 : 0;
        if (sNoShaders) { printf("[vita] debug_no_shaders.txt found: shader errors are ignored, nothing will be drawn\n"); }
    }
    return sNoShaders == 1;
}

  //////////////////
 // fatal errors //
//////////////////

static void vita_show_message_dialog(const char *msg) {
    SceMsgDialogUserMessageParam userParam;
    memset(&userParam, 0, sizeof(userParam));
    userParam.msg = (const SceChar8 *)msg;
    userParam.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &userParam;

    if (sceMsgDialogInit(&param) < 0) { return; }

    while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
    }
    sceMsgDialogTerm();
}

void vita_fatal(const char *msg) {
    // always leave a trace on the memory card
    FILE *f = fopen(VITA_USER_PATH "/fatal_error.txt", "w");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
    fflush(stdout);

    if (sGlReady && sceKernelGetThreadId() != sMainThreadId) {
        // only the main thread may touch the screen: hand the message over and park this thread
        snprintf(sFatalMessage, sizeof(sFatalMessage), "%s", msg);
        sFatalPending = true;
        while (true) { sceKernelDelayThread(1000 * 1000); }
    }

    if (sGlReady) {
        char text[512];
        snprintf(text, sizeof(text), "%s", msg);
        vita_show_message_dialog(text);
    }

    sceKernelExitProcess(0);
    while (true) { }
}

void vita_update(void) {
    static unsigned int sFrames = 0;
    static unsigned int sLastFrames = 0;
    static SceUInt64 sLastTime = 0;
    if ((++sFrames % 30) == 0) {
        // heartbeat: frames rendered and memory use, every 10 seconds. The log file is pushed
        // out to the memory card at the same time, so that it survives a hard crash.
        SceUInt64 now = sceKernelGetProcessTimeWide();
        if (sLastTime == 0) { sLastTime = now; }
        if (now - sLastTime >= 10 * 1000 * 1000) {
            char tag[64];
            float seconds = (float)(now - sLastTime) / 1000000.0f;
            snprintf(tag, sizeof(tag), "frame %u, %.1f fps", sFrames, (float)(sFrames - sLastFrames) / seconds);
            vita_log_memory(tag);
            printf("[vita] audio: %u buffers, peak %d, queued %d samples, master volume %.2f | output: %u blocks, %u underruns, last error 0x%08X%s%s\n",
                gVitaAudioStats.buffers, gVitaAudioStats.peak, gVitaAudioStats.buffered,
                (double) gVitaAudioStats.masterVolume,
                gVitaAudioStats.blocks, gVitaAudioStats.underruns, (unsigned int) gVitaAudioStats.outputError,
                gVitaAudioStats.muted ? ", MUTED" : "",
                gVitaAudioStats.nullDevice ? ", NO AUDIO DEVICE" : "");
            gVitaAudioStats.peak = 0;
            gVitaAudioStats.buffers = 0;
            gVitaAudioStats.blocks = 0;
            gVitaAudioStats.underruns = 0;

            // where the time of an average game tick (33.3 ms budget) went
            unsigned long long ticks = sPerf[VITA_PERF_TICKS] ? sPerf[VITA_PERF_TICKS] : 1;
            printf("[vita] perf: %llu ticks, %llu frames | ms per tick: game %.1f, lua %.1f, audio %.1f, network %.1f, gfx %.1f, swap %.1f, idle %.1f\n",
                sPerf[VITA_PERF_TICKS], sPerf[VITA_PERF_FRAMES],
                (double) sPerf[VITA_PERF_GAME]    / 1000.0 / (double) ticks,
                (double) sPerf[VITA_PERF_LUA]     / 1000.0 / (double) ticks,
                (double) sPerf[VITA_PERF_AUDIO]   / 1000.0 / (double) ticks,
                (double) sPerf[VITA_PERF_NETWORK] / 1000.0 / (double) ticks,
                (double) sPerf[VITA_PERF_GFX]     / 1000.0 / (double) ticks,
                (double) sPerf[VITA_PERF_SWAP]    / 1000.0 / (double) ticks,
                (double) sPerf[VITA_PERF_DELAY]   / 1000.0 / (double) ticks);

            // what the renderer did per drawn frame (the draw time is part of "gfx" above)
            unsigned long long frames = sPerf[VITA_PERF_FRAMES] ? sPerf[VITA_PERF_FRAMES] : 1;
            printf("[vita] gfx per frame: %.1f ms total, %.1f ms in vitaGL draws, %llu draw calls, %llu triangles | texture uploads %llu (%.1f ms), shader builds %llu (%.1f ms)\n",
                (double) sPerf[VITA_PERF_GFX]  / 1000.0 / (double) frames,
                (double) sPerf[VITA_PERF_DRAW] / 1000.0 / (double) frames,
                sPerf[VITA_PERF_DRAW_CALLS] / frames,
                sPerf[VITA_PERF_TRIANGLES] / frames,
                sPerf[VITA_PERF_TEX_UPLOADS], (double) sPerf[VITA_PERF_TEX_UPLOAD] / 1000.0,
                sPerf[VITA_PERF_SHADER_BUILDS], (double) sPerf[VITA_PERF_SHADER_BUILD] / 1000.0);
            printf("[vita] gfx breakdown per frame: events %.1f ms, interpolation %.1f ms, vertices %.1f ms (%llu in %llu batches), triangles %.1f ms (%llu before culling), matrices %.1f ms (%llu)\n",
                (double) sPerf[VITA_PERF_EVENTS]   / 1000.0 / (double) frames,
                (double) sPerf[VITA_PERF_INTERP]   / 1000.0 / (double) frames,
                (double) sPerf[VITA_PERF_VTX_TIME] / 1000.0 / (double) frames,
                sPerf[VITA_PERF_VTX_COUNT] / frames, sPerf[VITA_PERF_VTX_CALLS] / frames,
                (double) sPerf[VITA_PERF_TRI_TIME] / 1000.0 / (double) frames,
                sPerf[VITA_PERF_TRI_CALLS] / frames,
                (double) sPerf[VITA_PERF_MTX_TIME] / 1000.0 / (double) frames,
                sPerf[VITA_PERF_MTX_CALLS] / frames);
            memset(sPerf, 0, sizeof(sPerf));
            sLastTime = now;
            sLastFrames = sFrames;
            fsync(fileno(stdout));
        }

        // debugging aid for automated emulator runs: warp into a level after N seconds
        if (sDebugWarpAfterSeconds > 0 && now >= (SceUInt64) sDebugWarpAfterSeconds * 1000 * 1000) {
            extern bool dynos_warp_to_level(int aLevel, int aArea, int aAct);
            bool warped = dynos_warp_to_level(sDebugWarpLevel, 1, 1);
            printf("[vita] debug warp to level %d: %s\n", sDebugWarpLevel, warped ? "ok" : "failed");
            sDebugWarpAfterSeconds = 0;
        }

        // debugging aid for automated emulator runs: quit cleanly after N seconds
        if (sDebugExitAfterSeconds > 0 && now >= (SceUInt64) sDebugExitAfterSeconds * 1000 * 1000) {
            vita_log_memory("debug exit");
            fclose(stdout);
            sceKernelExitProcess(0);
        }
    }

    if (sFatalPending) {
        sFatalPending = false;
        vita_show_message_dialog(sFatalMessage);
        sceKernelExitProcess(0);
    }
}

// Used by the "no rom detected" screen: true once whenever a new button gets pressed
bool vita_rom_retry_requested(void) {
    static unsigned int sPrevButtons = 0xFFFFFFFF;
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) { return false; }
    unsigned int pressed = pad.buttons & ~sPrevButtons;
    sPrevButtons = pad.buttons;
    return pressed != 0;
}

  ////////////////////////////////
 // desktop-only feature stubs //
////////////////////////////////

// update checker: there is no auto updater on the Vita
bool gUpdateMessage = false;
void show_update_popup(void) { }
void check_for_updates(void) { }

// Mumble positional audio link
void mumble_init(void) { }
void mumble_update(void) { }
void mumble_update_menu(void) { }
bool should_update_context(void) { return false; }

#endif // TARGET_VITA
