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

static volatile int g_resumed = 0;
static volatile int g_running = 1;
static JavaVM*   g_jvm        = nullptr;
static jclass    g_cls        = nullptr;
static jmethodID g_showDialog = nullptr;
static volatile int g_jni_ready = 0;

static void (*orig_AndroidPause)() = nullptr;
static void hook_AndroidPause() {
    g_resumed = 1;
    if (orig_AndroidPause) orig_AndroidPause();
}

// Dump semua method class ke log
static void dumpMethods(JNIEnv* env, jclass cls) {
    jclass clsCls = env->FindClass("java/lang/Class");
    jmethodID getMethods = env->GetMethodID(clsCls, "getMethods", "()[Ljava/lang/reflect/Method;");
    jobjectArray methods = (jobjectArray)env->CallObjectMethod(cls, getMethods);
    if (!methods) { logff("[BG] getMethods null"); return; }

    jclass methodCls = env->FindClass("java/lang/reflect/Method");
    jmethodID getName = env->GetMethodID(methodCls, "getName", "()Ljava/lang/String;");
    jmethodID toString = env->GetMethodID(methodCls, "toString", "()Ljava/lang/String;");

    int len = env->GetArrayLength(methods);
    logff("[BG] Total methods: %d", len);
    for (int i = 0; i < len; i++) {
        jobject m = env->GetObjectArrayElement(methods, i);
        jstring nameStr = (jstring)env->CallObjectMethod(m, toString);
        const char* c = env->GetStringUTFChars(nameStr, nullptr);
        if (strstr(c, "ialog") || strstr(c, "show") || strstr(c, "Show")) {
            logff("[BG] METHOD[%d]: %s", i, c);
        }
        env->ReleaseStringUTFChars(nameStr, c);
        env->DeleteLocalRef(nameStr);
        env->DeleteLocalRef(m);
    }
}

static void initJNI() {
    void* h = dlopen("libnativehelper.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("/apex/com.android.art/lib/libart.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { logff("[BG] ERROR: libnativehelper/libart"); return; }

    auto getVMs = (jint(*)(JavaVM**, jsize, jsize*))dlsym(h, "JNI_GetCreatedJavaVMs");
    if (!getVMs) { logff("[BG] ERROR: JNI_GetCreatedJavaVMs"); return; }

    jsize count = 0;
    getVMs(&g_jvm, 1, &count);
    if (!g_jvm || count == 0) { logff("[BG] ERROR: JVM null"); return; }
    logff("[BG] JVM OK");

    JNIEnv* env = nullptr;
    g_jvm->AttachCurrentThread(&env, nullptr);
    if (!env) { logff("[BG] ERROR: env null"); return; }

    jclass atCls = env->FindClass("android/app/ActivityThread");
    jmethodID curApp = env->GetStaticMethodID(atCls, "currentApplication", "()Landroid/app/Application;");
    jobject app = env->CallStaticObjectMethod(atCls, curApp);
    jclass appCls = env->GetObjectClass(app);
    jmethodID getCL = env->GetMethodID(appCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject cl = env->CallObjectMethod(app, getCL);
    jclass clCls = env->GetObjectClass(cl);
    jmethodID loadClass = env->GetMethodID(clCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");

    jstring cn = env->NewStringUTF("com.arizona.game.GTASA");
    jclass gtasaCls = (jclass)env->CallObjectMethod(cl, loadClass, cn);
    env->DeleteLocalRef(cn);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }

    if (!gtasaCls) { logff("[BG] ERROR: class GTASA null"); g_jvm->DetachCurrentThread(); return; }
    g_cls = (jclass)env->NewGlobalRef(gtasaCls);
    logff("[BG] class GTASA OK - dumping methods...");

    // Dump semua method untuk cari nama yang benar
    dumpMethods(env, g_cls);

    g_jvm->DetachCurrentThread();
    g_jni_ready = 1;
}

static void* resume_thread(void*) {
    logff("[BG] resume_thread started");
    usleep(5000000); // 5 detik
    initJNI();

    while (g_running) {
        if (g_resumed) {
            g_resumed = 0;
            usleep(500000);
            logff("[BG] Resume detected! g_showDialog=%p", (void*)g_showDialog);
        }
        usleep(200000);
    }
    return nullptr;
}

extern "C" {

EXPORT void* __GetModInfo() {
    static const char* info = "bgdetect|1.6|Background Resume Detector|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    g_resumed = 0;
    g_running = 1;
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
