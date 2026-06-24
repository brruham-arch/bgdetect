#include <stdint.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdarg.h>

#define LOG_TAG "libbgdetect"
#define LOGFILE "/storage/emulated/0/bgdetect_log.txt"
#define FLAG_FILE "/storage/emulated/0/Android/media/com.sampmobilerp.game/monetloader/bg_resumed.txt"
#define EXPORT __attribute__((visibility("default")))

static void logff(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    FILE* f = fopen(LOGFILE, "a"); if (f) { fprintf(f, "%s\n", buf); fclose(f); }
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "%s", buf);
}

static void writeFlag(int val) {
    FILE* f = fopen(FLAG_FILE, "w");
    if (f) { fprintf(f, "%d", val); fclose(f); }
}

static void (*orig_SetAndroidPaused)(bool) = nullptr;
static void hook_SetAndroidPaused(bool paused) {
    logff("[BG] SetAndroidPaused(%d)", (int)paused);
    if (!paused) {
        writeFlag(1);
        logff("[BG] Resume detected!");
    } else {
        writeFlag(0);
        logff("[BG] Game masuk background.");
    }
    if (orig_SetAndroidPaused) orig_SetAndroidPaused(paused);
}

extern "C" {

EXPORT void* __GetModInfo() {
    static const char* info = "bgdetect|1.0|Background Resume Detector|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    writeFlag(0);
    logff("[BG] OnModPreLoad");
}

EXPORT void OnModLoad() {
    logff("[BG] OnModLoad mulai");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logff("[BG] ERROR: libdobby"); return; }

    auto dobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { logff("[BG] ERROR: DobbyHook sym"); return; }

    void* hGTASA = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD);
    if (!hGTASA) { logff("[BG] ERROR: libGTASA"); return; }
    uintptr_t base = (uintptr_t)hGTASA;
    logff("[BG] libGTASA base=0x%lX", (unsigned long)base);

    void* target = (void*)(base + 0x269af4);
    logff("[BG] target=%p", target);

    int r = dobbyHook(target, (void*)hook_SetAndroidPaused, (void**)&orig_SetAndroidPaused);
    if (r != 0) { logff("[BG] ERROR: DobbyHook gagal r=%d", r); return; }

    logff("[BG] OnModLoad SELESAI");
}

} // extern "C"
