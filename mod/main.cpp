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
            logff("[BG] getLibBase(%s)=0x%lX", name, (unsigned long)base);
            return base;
        }
    }
    fclose(f);
    return 0;
}

static volatile int g_resumed = 0;
static volatile int g_running = 1;
static JavaVM* g_jvm = nullptr;
static jclass  g_cls = nullptr;
static jmethodID g_showDialog = nullptr;

static void (*orig_AndroidPause)() = nullptr;
static void hook_AndroidPause() {
    logff("[BG] AndroidPause hooked!");
    g_resumed = 1;
    if (orig_AndroidPause) orig_AndroidPause();
}

static void showDialogJNI(int id, int style, const char* title, const char* msg, const char* btn1, const char* btn2) {
    if (!g_jvm || !g_cls || !g_showDialog) {
        logff("[BG] JNI tidak siap");
        return;
    }
    JNIEnv* env = nullptr;
    bool attached = false;
    int status = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    if (!env) { logff("[BG] env null"); return; }

    jstring jtitle = env->NewStringUTF(title);
    jstring jmsg   = env->NewStringUTF(msg);
    jstring jbtn1  = env->NewStringUTF(btn1);
    jstring jbtn2  = env->NewStringUTF(btn2);

    env->CallStaticVoidMethod(g_cls, g_showDialog, id, style, jtitle, jmsg, jbtn1, jbtn2);

    env->DeleteLocalRef(jtitle);
    env->DeleteLocalRef(jmsg);
    env->DeleteLocalRef(jbtn1);
    env->DeleteLocalRef(jbtn2);

    if (attached) g_jvm->DetachCurrentThread();
    logff("[BG] showDialogJNI done");
}

static void* resume_thread(void*) {
    logff("[BG] resume_thread started");
    while (g_running) {
        if (g_resumed) {
            g_resumed = 0;
            usleep(500000);
            logff("[BG] Resume! Tampilkan dialog...");
            showDialogJNI(99, 0, "Selamat Datang", "Kamu baru saja kembali ke game!", "OK", "");
        }
        usleep(200000);
    }
    return nullptr;
}

static void initJNI() {
    // Dapatkan JVM via libnativehelper atau libandroid_runtime
    void* h = dlopen("libnativehelper.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libandroid_runtime.so", RTLD_NOW | RTLD_GLOBAL);

    auto getVMs = (jint(*)(JavaVM**, jsize, jsize*))nullptr;
    if (h) getVMs = (jint(*)(JavaVM**, jsize, jsize*))dlsym(h, "JNI_GetCreatedJavaVMs");

    // Fallback: cari di libart.so
    if (!getVMs) {
        void* hart = dlopen("libart.so", RTLD_NOW | RTLD_GLOBAL);
        if (!hart) hart = dlopen("/apex/com.android.art/lib/libart.so", RTLD_NOW | RTLD_GLOBAL);
        if (hart) getVMs = (jint(*)(JavaVM**, jsize, jsize*))dlsym(hart, "JNI_GetCreatedJavaVMs");
    }

    if (!getVMs) { logff("[BG] ERROR: JNI_GetCreatedJavaVMs tidak ditemukan"); return; }

    jsize count = 0;
    getVMs(&g_jvm, 1, &count);
    if (!g_jvm || count == 0) { logff("[BG] ERROR: JVM tidak ditemukan"); return; }
    logff("[BG] JVM OK");

    JNIEnv* env = nullptr;
    g_jvm->AttachCurrentThread(&env, nullptr);
    if (!env) { logff("[BG] ERROR: env null"); return; }

    // Cari class GTASA via ActivityThread
    jclass actThread = env->FindClass("android/app/ActivityThread");
    jmethodID curApp = env->GetStaticMethodID(actThread, "currentApplication", "()Landroid/app/Application;");
    jobject app = env->CallStaticObjectMethod(actThread, curApp);
    jclass appCls = env->GetObjectClass(app);
    jmethodID getClassLoader = env->GetMethodID(appCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject classLoader = env->CallObjectMethod(app, getClassLoader);
    jclass loaderCls = env->GetObjectClass(classLoader);
    jmethodID loadClass = env->GetMethodID(loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");

    jstring className = env->NewStringUTF("com.arizona.game.GTASA");
    jclass gtasaCls = (jclass)env->CallObjectMethod(classLoader, loadClass, className);
    env->DeleteLocalRef(className);

    if (!gtasaCls) { logff("[BG] ERROR: class GTASA tidak ditemukan"); return; }
    logff("[BG] class GTASA OK");

    g_cls = (jclass)env->NewGlobalRef(gtasaCls);

    // Cari method showDialog
    g_showDialog = env->GetStaticMethodID(g_cls, "showDialog",
        "(IILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (!g_showDialog) {
        logff("[BG] WARN: showDialog static tidak ditemukan, coba instance method");
        // Coba nama lain
        g_showDialog = env->GetStaticMethodID(g_cls, "ShowDialog",
            "(IILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    }
    logff("[BG] g_showDialog=%p", (void*)g_showDialog);
    g_jvm->DetachCurrentThread();
}

extern "C" {

EXPORT void* __GetModInfo() {
    static const char* info = "bgdetect|1.4|Background Resume Detector|brruham";
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

    initJNI();

    pthread_t tid;
    pthread_create(&tid, nullptr, resume_thread, nullptr);
    pthread_detach(tid);

    logff("[BG] OnModLoad SELESAI");
}

} // extern "C"
