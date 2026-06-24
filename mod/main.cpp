#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <jni.h>

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
            return base;
        }
    }
    fclose(f);
    return 0;
}

static volatile int g_resumed  = 0;
static volatile int g_running  = 1;
static JavaVM*      g_jvm      = nullptr;
static jobject      g_activity = nullptr; // instance method butuh object
static jclass       g_cls      = nullptr;
static jmethodID    g_showPlayerDialog = nullptr;
static volatile int g_jni_ready = 0;

static void (*orig_AndroidPause)() = nullptr;
static void hook_AndroidPause() {
    g_resumed = 1;
    if (orig_AndroidPause) orig_AndroidPause();
}

static jbyteArray strToBytes(JNIEnv* env, const char* str) {
    int len = strlen(str);
    jbyteArray arr = env->NewByteArray(len);
    env->SetByteArrayRegion(arr, 0, len, (jbyte*)str);
    return arr;
}

static void showDialog(int id, int style, const char* title, const char* msg, const char* btn1, const char* btn2) {
    if (!g_jni_ready || !g_jvm || !g_activity || !g_showPlayerDialog) {
        logff("[BG] JNI belum siap");
        return;
    }
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    if (!env) return;

    jbyteArray jTitle = strToBytes(env, title);
    jbyteArray jMsg   = strToBytes(env, msg);
    jbyteArray jBtn1  = strToBytes(env, btn1);
    jbyteArray jBtn2  = strToBytes(env, btn2);

    // void showPlayerDialog(int id, int style, byte[] title, byte[] msg, byte[] btn1, byte[] btn2)
    env->CallVoidMethod(g_activity, g_showPlayerDialog, id, style, jTitle, jMsg, jBtn1, jBtn2);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); logff("[BG] Exception showPlayerDialog"); }

    env->DeleteLocalRef(jTitle);
    env->DeleteLocalRef(jMsg);
    env->DeleteLocalRef(jBtn1);
    env->DeleteLocalRef(jBtn2);

    if (attached) g_jvm->DetachCurrentThread();
    logff("[BG] showPlayerDialog done");
}

static void initJNI() {
    void* h = dlopen("libnativehelper.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("/apex/com.android.art/lib/libart.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { logff("[BG] ERROR: libnativehelper"); return; }

    auto getVMs = (jint(*)(JavaVM**, jsize, jsize*))dlsym(h, "JNI_GetCreatedJavaVMs");
    if (!getVMs) { logff("[BG] ERROR: JNI_GetCreatedJavaVMs"); return; }

    jsize count = 0;
    getVMs(&g_jvm, 1, &count);
    if (!g_jvm || count == 0) { logff("[BG] ERROR: JVM null"); return; }
    logff("[BG] JVM OK");

    JNIEnv* env = nullptr;
    g_jvm->AttachCurrentThread(&env, nullptr);
    if (!env) { logff("[BG] ERROR: env null"); return; }

    // Dapatkan Activity instance via ActivityThread
    jclass atCls = env->FindClass("android/app/ActivityThread");
    jmethodID curActivity = env->GetStaticMethodID(atCls, "currentActivity", "()Landroid/app/Activity;");
    jobject activity = env->CallStaticObjectMethod(atCls, curActivity);
    if (!activity) { logff("[BG] ERROR: activity null"); g_jvm->DetachCurrentThread(); return; }
    g_activity = env->NewGlobalRef(activity);
    logff("[BG] activity OK");

    g_cls = (jclass)env->NewGlobalRef(env->GetObjectClass(activity));

    // showPlayerDialog(int, int, byte[], byte[], byte[], byte[])
    g_showPlayerDialog = env->GetMethodID(g_cls, "showPlayerDialog", "(II[B[B[B[B)V");
    if (!g_showPlayerDialog) {
        env->ExceptionClear();
        logff("[BG] ERROR: showPlayerDialog method null");
        g_jvm->DetachCurrentThread();
        return;
    }
    logff("[BG] showPlayerDialog method OK");

    g_jvm->DetachCurrentThread();
    g_jni_ready = 1;
    logff("[BG] JNI siap!");
}

static void* resume_thread(void*) {
    logff("[BG] resume_thread started");
    usleep(5000000); // 5 detik tunggu game boot
    initJNI();

    while (g_running) {
        if (g_resumed) {
            g_resumed = 0;
            usleep(500000);
            logff("[BG] Resume detected!");
            showDialog(99, 0, "Selamat Datang", "Kamu baru saja kembali ke game!", "OK", "");
        }
        usleep(200000);
    }
    return nullptr;
}

extern "C" {

EXPORT void* __GetModInfo() {
    static const char* info = "bgdetect|1.7|Background Resume Detector|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    g_resumed   = 0;
    g_running   = 1;
    g_jni_ready = 0;
    logff("[BG] OnModPreLoad");
}

EXPORT void OnModLoad() {
    logff("[BG] OnModLoad mulai");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logff("[BG] ERROR: libdobby"); return; }
    auto dobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { logff("[BG] ERROR: DobbyHook"); return; }

    uintptr_t gtaBase = getLibBase("libGTASA.so");
    if (!gtaBase) { logff("[BG] ERROR: gtaBase=0"); return; }

    void* target = (void*)(gtaBase + 0x269AF4 + 1);
    int r = dobbyHook(target, (void*)hook_AndroidPause, (void**)&orig_AndroidPause);
    logff("[BG] DobbyHook result=%d", r);
    if (r != 0) { logff("[BG] ERROR: hook gagal"); return; }
    logff("[BG] Hook OK");

    pthread_t tid;
    pthread_create(&tid, nullptr, resume_thread, nullptr);
    pthread_detach(tid);

    logff("[BG] OnModLoad SELESAI");
}

} // extern "C"
