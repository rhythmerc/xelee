/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Android platform hooks that have to go through the Java side. The CMake
 * glob (CMakeLists.txt:141) compiles this everywhere, so everything outside
 * __ANDROID__ is a no-op and callers need no #ifdef. */
#include "android_hooks.h"

#if defined(__ANDROID__)

#include "pc.h"

#include <SDL3/SDL_system.h>
#include <jni.h>
#include <cstdlib>
#include <mutex>

/* Every signature below was taken from android-34 android.jar with
 * `javap -s -constants`, not from memory:
 *   Context.getApplicationContext   ()Landroid/content/Context;
 *   Context.getSystemService        (Ljava/lang/String;)Ljava/lang/Object;
 *   Context.WIFI_SERVICE            = "wifi"
 *   WifiManager.createMulticastLock
 * (Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;
 *   MulticastLock.setReferenceCounted (Z)V
 *   MulticastLock.acquire           ()V
 *   MulticastLock.release           ()V
 * The lock needs android.permission.CHANGE_WIFI_MULTICAST_STATE
 * (AndroidManifest.xml:16); it is a `normal` permission, so it is granted at
 * install and there is nothing to request at runtime. */

namespace {

std::mutex g_mutex;
int g_depth;    /* acquire() nesting, guarded by g_mutex */
jobject g_lock; /* global ref: WifiManager$MulticastLock */
jmethodID g_acquire;
jmethodID g_release;
bool g_failed; /* the lookup failed once: do not report it again */

/* True (and clears the exception) when the last JNI call left one pending. */
bool threw(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck() == JNI_FALSE) {
        return false;
    }
    env->ExceptionDescribe(); /* the stack trace goes to logcat */
    env->ExceptionClear();
    pc_log_line("lan: multicast lock: %s threw", what);
    return true;
}

/* Create the lock once and keep it: SDL's activity is a Context, so
 * getApplicationContext() -> getSystemService("wifi") ->
 * createMulticastLock("melee-lan"). Local refs live in a pushed frame,
 * including the one SDL_GetAndroidActivity() hands back. */
bool lock_open(JNIEnv* env) {
    if (g_lock != nullptr) {
        return true;
    }
    if (g_failed) {
        return false;
    }
    if (env->PushLocalFrame(16) != JNI_OK) {
        threw(env, "PushLocalFrame");
        g_failed = true;
        return false;
    }
    bool ok = [env]() -> bool {
        jobject activity = (jobject)SDL_GetAndroidActivity();
        if (activity == nullptr) {
            pc_log_line(
                "lan: no Android activity: no multicast lock, mDNS answers may be filtered");
            return false;
        }
        jclass ctx_cls = env->FindClass("android/content/Context");
        jclass wifi_cls = env->FindClass("android/net/wifi/WifiManager");
        jclass lock_cls = env->FindClass("android/net/wifi/WifiManager$MulticastLock");
        if (ctx_cls == nullptr || wifi_cls == nullptr || lock_cls == nullptr) {
            return false;
        }
        jmethodID app_ctx =
            env->GetMethodID(ctx_cls, "getApplicationContext", "()Landroid/content/Context;");
        jmethodID get_svc =
            env->GetMethodID(ctx_cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        jmethodID create = env->GetMethodID(wifi_cls, "createMulticastLock",
            "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
        jmethodID counted = env->GetMethodID(lock_cls, "setReferenceCounted", "(Z)V");
        g_acquire = env->GetMethodID(lock_cls, "acquire", "()V");
        g_release = env->GetMethodID(lock_cls, "release", "()V");
        if (app_ctx == nullptr || get_svc == nullptr || create == nullptr || counted == nullptr ||
            g_acquire == nullptr || g_release == nullptr)
        {
            return false;
        }
        jstring service = env->NewStringUTF("wifi"); /* Context.WIFI_SERVICE */
        jstring tag = env->NewStringUTF("melee-lan");
        if (service == nullptr || tag == nullptr) {
            return false;
        }
        /* From the application context: before API 24 a manager taken from
         * the activity context keeps the activity alive. */
        jobject app = env->CallObjectMethod(activity, app_ctx);
        if (threw(env, "getApplicationContext") || app == nullptr) {
            return false;
        }
        jobject wifi = env->CallObjectMethod(app, get_svc, service);
        if (threw(env, "getSystemService")) {
            return false;
        }
        if (wifi == nullptr) { /* Wi-Fi-less device, or the service is gone */
            pc_log_line("lan: no WifiManager: no multicast lock, mDNS answers may be filtered");
            return false;
        }
        jobject lock = env->CallObjectMethod(wifi, create, tag);
        if (threw(env, "createMulticastLock") || lock == nullptr) {
            return false;
        }
        /* The nesting is counted here (android_multicast.h), so the Java
         * lock stays a plain on/off and an unbalanced release can never
         * throw out of MulticastLock.release(). */
        env->CallVoidMethod(lock, counted, JNI_FALSE);
        if (threw(env, "setReferenceCounted")) {
            return false;
        }
        g_lock = env->NewGlobalRef(lock);
        return g_lock != nullptr;
    }();
    if (!ok) {
        threw(env, "multicast lock lookup");
    }
    env->PopLocalFrame(nullptr);
    g_failed = !ok;
    return ok;
}

}  // namespace

void pc_android_multicast_lock_acquire(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_depth++ > 0) {
        return;
    }
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    if (env == nullptr || !lock_open(env)) {
        return; /* the depth still counts, so the release below stays balanced */
    }
    env->CallVoidMethod(g_lock, g_acquire);
    if (!threw(env, "MulticastLock.acquire")) {
        pc_log_line("lan: multicast lock held");
    }
}

void pc_android_multicast_lock_release(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_depth == 0 || --g_depth > 0 || g_lock == nullptr) {
        return;
    }
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    if (env == nullptr) {
        return;
    }
    env->CallVoidMethod(g_lock, g_release);
    if (!threw(env, "MulticastLock.release")) {
        pc_log_line("lan: multicast lock released");
    }
}

void pc_android_set_launcher_active(bool active) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = env != nullptr ? (jobject)SDL_GetAndroidActivity() : nullptr;
    if (env == nullptr || activity == nullptr) {
        return;
    }
    /* MeleeActivity.setLauncherActive posts to the UI thread itself, so this
     * is safe from the SDL thread. */
    jclass cls = env->GetObjectClass(activity);
    jmethodID set = cls != nullptr ? env->GetMethodID(cls, "setLauncherActive", "(Z)V") : nullptr;
    if (set != nullptr) {
        env->CallVoidMethod(activity, set, active ? JNI_TRUE : JNI_FALSE);
        threw(env, "setLauncherActive");
    }
    if (cls != nullptr) {
        env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(activity);
}

/* Call a method taking nothing and returning an int (or a boolean, as 0/1)
 * on the current activity; `fallback` when it can't. */
static int call_activity_int(const char* name, const char* sig, int fallback) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = env != nullptr ? (jobject)SDL_GetAndroidActivity() : nullptr;
    if (env == nullptr || activity == nullptr) {
        return fallback;
    }
    int value = fallback;
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = cls != nullptr ? env->GetMethodID(cls, name, sig) : nullptr;
    if (method != nullptr) {
        const int got = sig[2] == 'Z' ? (env->CallBooleanMethod(activity, method) ? 1 : 0)
                                      : static_cast<int>(env->CallIntMethod(activity, method));
        if (!threw(env, name)) {
            value = got;
        }
    } else {
        env->ExceptionClear();
    }
    if (cls != nullptr) {
        env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(activity);
    return value;
}

/* Call a void method on the current activity (it posts to its UI thread). */
static bool call_activity(const char* name, const char* sig, const char* arg) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = env != nullptr ? (jobject)SDL_GetAndroidActivity() : nullptr;
    if (env == nullptr || activity == nullptr) {
        return false;
    }
    bool ok = false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = cls != nullptr ? env->GetMethodID(cls, name, sig) : nullptr;
    if (method != nullptr) {
        if (arg != nullptr) {
            jstring jarg = env->NewStringUTF(arg);
            env->CallVoidMethod(activity, method, jarg);
            env->DeleteLocalRef(jarg);
        } else {
            env->CallVoidMethod(activity, method);
        }
        ok = !threw(env, name);
    } else {
        env->ExceptionClear();
    }
    if (cls != nullptr) {
        env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(activity);
    return ok;
}

bool pc_android_is_xr_activity(void) {
    const char* v = getenv("MELEE_XR_ACTIVITY");
    return v != nullptr && v[0] == '1';
}

bool pc_android_launch_xr(const char* disc) {
    return disc != nullptr && disc[0] != '\0' && call_activity("launchXrGame", "(Ljava/lang/String;)V", disc);
}

void pc_android_launch_panel(void) {
    call_activity("launchPanel", "()V", nullptr);
}

bool pc_android_restart(const char* disc) {
    return call_activity("restartGame", "(Ljava/lang/String;)V", disc != nullptr ? disc : "");
}

bool pc_android_download(const char* url, const char* path) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = env != nullptr ? (jobject)SDL_GetAndroidActivity() : nullptr;
    if (env == nullptr || activity == nullptr || url == nullptr || path == nullptr) {
        return false;
    }
    bool ok = false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID method =
        cls != nullptr ? env->GetMethodID(cls, "downloadFile", "(Ljava/lang/String;Ljava/lang/String;)Z") : nullptr;
    if (method != nullptr) {
        jstring jurl = env->NewStringUTF(url);
        jstring jpath = env->NewStringUTF(path);
        ok = env->CallBooleanMethod(activity, method, jurl, jpath) == JNI_TRUE;
        ok = ok && !threw(env, "downloadFile");
        env->DeleteLocalRef(jurl);
        env->DeleteLocalRef(jpath);
    } else {
        env->ExceptionClear();
    }
    if (cls != nullptr) {
        env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(activity);
    return ok;
}

bool pc_android_install_apk(const char* path) {
    return path != nullptr && path[0] != '\0' && call_activity("installApk", "(Ljava/lang/String;)V", path);
}

#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>

/* dev.melee.RoomCapture (the Quest build's room capture, in the immersive
 * game's process): whether a capture runs, the camera's calibration, and
 * each frame's grid of mean colour, for XR room lighting
 * (aurora_xr_room_capture_*). */
extern "C" JNIEXPORT jint JNICALL Java_dev_melee_RoomCapture_nativeWanted(JNIEnv*, jclass) {
    return aurora_xr_room_capture_wanted();
}

extern "C" JNIEXPORT void JNICALL Java_dev_melee_RoomCapture_nativeCamera(JNIEnv* env, jclass, jfloatArray rotation,
    jfloatArray translation, jfloatArray intrinsics, jint sensor_w, jint sensor_h, jint width, jint height) {
    float q[4] = {0.f, 0.f, 0.f, 1.f}, t[3] = {0.f, 0.f, 0.f}, k[5] = {0.f, 0.f, 0.f, 0.f, 0.f};
    if (env->GetArrayLength(rotation) >= 4)
        env->GetFloatArrayRegion(rotation, 0, 4, q);
    if (env->GetArrayLength(translation) >= 3)
        env->GetFloatArrayRegion(translation, 0, 3, t);
    if (env->GetArrayLength(intrinsics) >= 4)
        env->GetFloatArrayRegion(intrinsics, 0, 4, k);
    aurora_xr_room_capture_camera(q, t, k, sensor_w, sensor_h, width, height);
}

extern "C" JNIEXPORT void JNICALL Java_dev_melee_RoomCapture_nativeFrame(JNIEnv* env, jclass, jfloatArray rgb,
    jint cols, jint rows, jfloat exposure_ms, jfloat iso) {
    const jsize n = env->GetArrayLength(rgb);
    if (cols <= 0 || rows <= 0 || n < cols * rows * 4)
        return;
    jfloat* p = env->GetFloatArrayElements(rgb, nullptr);
    if (p == nullptr)
        return;
    aurora_xr_room_capture_frame(p, cols, rows, exposure_ms, iso);
    env->ReleaseFloatArrayElements(rgb, p, JNI_ABORT);
}
#endif

bool pc_android_headset_camera_granted(void) {
    return call_activity_int("hasHeadsetCamera", "()Z", 0) != 0;
}

void pc_android_headset_camera_request(void) {
    call_activity("requestHeadsetCamera", "()V", nullptr);
}

int pc_android_headset_camera_result(void) {
    return call_activity_int("headsetCameraResult", "()I", 0);
}

void pc_android_open_app_settings(void) {
    call_activity("openAppSettings", "()V", nullptr);
}

#else

void pc_android_multicast_lock_acquire(void) {}
void pc_android_multicast_lock_release(void) {}

void pc_android_set_launcher_active(bool active) {
    (void)active;
}

bool pc_android_is_xr_activity(void) {
    return false;
}

bool pc_android_launch_xr(const char* disc) {
    (void)disc;
    return false;
}

void pc_android_launch_panel(void) {}

bool pc_android_restart(const char* disc) {
    (void)disc;
    return false;
}

bool pc_android_download(const char* url, const char* path) {
    (void)url;
    (void)path;
    return false;
}

bool pc_android_install_apk(const char* path) {
    (void)path;
    return false;
}

bool pc_android_headset_camera_granted(void) {
    return true;
}

void pc_android_headset_camera_request(void) {}

int pc_android_headset_camera_result(void) {
    return 1;
}

void pc_android_open_app_settings(void) {}

#endif

