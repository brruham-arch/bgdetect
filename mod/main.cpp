#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#define LOG_TAG "libbgdetect"
#define LOGFILE "/storage/emulated/0/bgdetect_log.txt"
#define EXPORT  __attribute__((visibility("default")))

static void logff(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    FILE* f = fopen(LOGFILE, "a"); if (f) { fprintf(f, "%s\n", buf); fclose(f); }
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "%s", buf);
}

static uintptr_t getLibBase(const char* name) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, name) && strstr(line, "r-xp")) {
            uintptr_t base = (uintptr_t)strtoull(line, nullptr, 16);
            fclose(f);
            logff("[BG] getLibBase(%s)=0x%lX", name, (unsigned long)base);
            return base;
        }
    }
    fclose(f);
    return 0;
}

static volatile int g_resumed = 0;
static volatile int g_running = 1;

typedef void (*ShowDialog_t)(int, int, const char*, const char*, const char*, const char*);
static ShowDialog_t pShowDialog = nullptr;

static void (*orig_AndroidPause)() = nullptr;
static void hook_AndroidPause() {
    logff("[BG] AndroidPause hooked!");
    g_resumed = 1;
    if (orig_AndroidPause) orig_AndroidPause();
}

static void* resume_thread(void*) {
    logff("[BG] resume_thread started");
    while (g_running) {
        if (g_resumed) {
            g_resumed = 0;
            usleep(500000);
            logff("[BG] Resume! Tampilkan dialog...");
            if (pShowDialog) {
                pShowDialog(99, 0,
                    "Selamat Datang",
                    "Kamu baru saja kembali ke game!",
                    "OK", "");
                logff("[BG] Dialog shown");
            } else {
                logff("[BG] pShowDialog null");
            }
        }
        usleep(200000);
    }
    return nullptr;
}

extern "C" {

EXPORT void* __GetModInfo() {
    static const char* info = "bgdetect|1.3|Background Resume Detector|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    g_resumed = 0;
    g_running = 1;
    logff("[BG] OnModPreLoad");
}

EXPORT void OnModLoad() {
    logff("[BG] OnModLoad mulai");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logff("[BG] ERROR: libdobby"); return; }
    auto dobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { logff("[BG] ERROR: DobbyHook sym"); return; }

    uintptr_t gtaBase = getLibBase("libGTASA.so");
    if (!gtaBase) { logff("[BG] ERROR: gtaBase=0"); return; }

    void* target = (void*)(gtaBase + 0x269AF4 + 1);
    logff("[BG] AndroidPause target=%p", target);

    int r = dobbyHook(target, (void*)hook_AndroidPause, (void**)&orig_AndroidPause);
    logff("[BG] DobbyHook result=%d", r);
    if (r != 0) { logff("[BG] ERROR: DobbyHook gagal r=%d", r); return; }
    logff("[BG] Hook AndroidPause OK");

    void* hSamp = dlopen("libsamp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!hSamp) { logff("[BG] ERROR: libsamp"); return; }
    pShowDialog = (ShowDialog_t)dlsym(hSamp, "sampShowDialog");
    logff("[BG] pShowDialog=%p", (void*)pShowDialog);

    pthread_t tid;
    pthread_create(&tid, nullptr, resume_thread, nullptr);
    pthread_detach(tid);

    logff("[BG] OnModLoad SELESAI");
}

} // extern "C"
