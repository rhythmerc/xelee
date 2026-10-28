/* SPDX-License-Identifier: GPL-2.0-or-later */
/* HTTPS for the Slippi account flow on Android, which has no libcurl: the
 * platform's own java.net.HttpURLConnection over JNI (slp_http.c calls it).
 * Signatures are those of android-34's java.net / java.io classes. Runs on the
 * caller's thread; SDL attaches that thread to the VM. Everything outside
 * __ANDROID__ is empty. */
#if defined(__ANDROID__)

#include <SDL3/SDL_system.h>
#include <android/log.h>
#include <jni.h>
#include <cstring>
#include <initializer_list>
#include <string>

namespace {

/* A pending Java exception, cleared and logged: the failure's reason
 * ("Cleartext HTTP traffic ... not permitted", a timeout) is otherwise lost. */
bool threw(JNIEnv* env) {
    if (env->ExceptionCheck() == JNI_FALSE) {
        return false;
    }
    jthrowable e = env->ExceptionOccurred();
    env->ExceptionClear();
    jclass cls = e != nullptr ? env->GetObjectClass(e) : nullptr;
    jmethodID to_string = cls != nullptr ? env->GetMethodID(cls, "toString", "()Ljava/lang/String;") : nullptr;
    jstring text = to_string != nullptr ? static_cast<jstring>(env->CallObjectMethod(e, to_string)) : nullptr;
    env->ExceptionClear();
    const char* c = text != nullptr ? env->GetStringUTFChars(text, nullptr) : nullptr;
    __android_log_print(ANDROID_LOG_INFO, "Melee", "http: %s", c != nullptr ? c : "a Java exception");
    if (c != nullptr) {
        env->ReleaseStringUTFChars(text, c);
    }
    for (jobject o : {static_cast<jobject>(text), static_cast<jobject>(cls), static_cast<jobject>(e)}) {
        if (o != nullptr) {
            env->DeleteLocalRef(o);
        }
    }
    return true;
}

} // namespace

/* Same contract as the curl path: true when a response came back (any
 * status), false on a transport failure. `out` is NUL-terminated and
 * truncated to fit. */
extern "C" bool pc_http_android(const char* url, const char* body, size_t body_len, const char* content_type,
                               const char* bearer, bool post, int read_timeout_ms, char* out, size_t cap,
                               int* status) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr || url == nullptr || out == nullptr || cap == 0) {
        return false;
    }
    bool ok = false;
    size_t len = 0;
    if (env->PushLocalFrame(32) != JNI_OK) {
        env->ExceptionClear();
        return false;
    }
    jclass url_cls = env->FindClass("java/net/URL");
    jclass conn_cls = env->FindClass("java/net/HttpURLConnection");
    jclass out_cls = env->FindClass("java/io/OutputStream");
    jclass in_cls = env->FindClass("java/io/InputStream");
    if (url_cls == nullptr || conn_cls == nullptr || out_cls == nullptr || in_cls == nullptr || threw(env)) {
        env->PopLocalFrame(nullptr);
        return false;
    }
    jmethodID url_new = env->GetMethodID(url_cls, "<init>", "(Ljava/lang/String;)V");
    jmethodID open = env->GetMethodID(url_cls, "openConnection", "()Ljava/net/URLConnection;");
    jmethodID set_method = env->GetMethodID(conn_cls, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID set_prop = env->GetMethodID(conn_cls, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID set_conn_to = env->GetMethodID(conn_cls, "setConnectTimeout", "(I)V");
    jmethodID set_read_to = env->GetMethodID(conn_cls, "setReadTimeout", "(I)V");
    jmethodID set_do_out = env->GetMethodID(conn_cls, "setDoOutput", "(Z)V");
    jmethodID get_out = env->GetMethodID(conn_cls, "getOutputStream", "()Ljava/io/OutputStream;");
    jmethodID get_code = env->GetMethodID(conn_cls, "getResponseCode", "()I");
    jmethodID get_in = env->GetMethodID(conn_cls, "getInputStream", "()Ljava/io/InputStream;");
    jmethodID get_err = env->GetMethodID(conn_cls, "getErrorStream", "()Ljava/io/InputStream;");
    jmethodID disconnect = env->GetMethodID(conn_cls, "disconnect", "()V");
    jmethodID write = env->GetMethodID(out_cls, "write", "([B)V");
    jmethodID close_out = env->GetMethodID(out_cls, "close", "()V");
    jmethodID read = env->GetMethodID(in_cls, "read", "([B)I");
    jmethodID close_in = env->GetMethodID(in_cls, "close", "()V");
    if (threw(env) || !url_new || !open || !set_method || !set_prop || !set_conn_to || !set_read_to ||
        !set_do_out || !get_out || !get_code || !get_in || !get_err || !disconnect || !write || !close_out ||
        !read || !close_in) {
        env->PopLocalFrame(nullptr);
        return false;
    }
    out[0] = '\0';
    jobject conn = nullptr;
    do {
        jstring jurl = env->NewStringUTF(url);
        jobject u = env->NewObject(url_cls, url_new, jurl);
        if (threw(env) || u == nullptr) {
            break;
        }
        conn = env->CallObjectMethod(u, open);
        if (threw(env) || conn == nullptr) {
            conn = nullptr;
            break;
        }
        env->CallVoidMethod(conn, set_method, env->NewStringUTF(post ? "POST" : "GET"));
        env->CallVoidMethod(conn, set_conn_to, 10000);
        env->CallVoidMethod(conn, set_read_to, read_timeout_ms);
        env->CallVoidMethod(conn, set_prop, env->NewStringUTF("User-Agent"), env->NewStringUTF("melee-pc Slippi"));
        if (post) {
            env->CallVoidMethod(conn, set_prop, env->NewStringUTF("Content-Type"),
                                env->NewStringUTF(content_type));
        }
        if (bearer != nullptr && bearer[0] != '\0') {
            std::string auth = std::string("Bearer ") + bearer;
            env->CallVoidMethod(conn, set_prop, env->NewStringUTF("Authorization"),
                                env->NewStringUTF(auth.c_str()));
        }
        if (threw(env)) {
            break;
        }
        if (post && body != nullptr) {
            const jsize n = static_cast<jsize>(body_len);
            jbyteArray bytes = env->NewByteArray(n);
            env->SetByteArrayRegion(bytes, 0, n, reinterpret_cast<const jbyte*>(body));
            env->CallVoidMethod(conn, set_do_out, JNI_TRUE);
            jobject os = env->CallObjectMethod(conn, get_out);
            if (threw(env) || os == nullptr) {
                break;
            }
            env->CallVoidMethod(os, write, bytes);
            env->CallVoidMethod(os, close_out);
            if (threw(env)) {
                break;
            }
        }
        const jint code = env->CallIntMethod(conn, get_code);
        if (threw(env)) {
            break;
        }
        if (status != nullptr) {
            *status = code;
        }
        jobject is = env->CallObjectMethod(conn, code >= 400 ? get_err : get_in);
        if (threw(env)) {
            break;
        }
        ok = true; /* a response came back; the body may be empty */
        if (is != nullptr) {
            jbyteArray buf = env->NewByteArray(4096);
            for (;;) {
                const jint n = env->CallIntMethod(is, read, buf);
                if (threw(env) || n < 0) {
                    break;
                }
                const size_t room = cap - 1 - len;
                const size_t take = static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room;
                if (take > 0) {
                    env->GetByteArrayRegion(buf, 0, static_cast<jsize>(take), reinterpret_cast<jbyte*>(out + len));
                    len += take;
                }
            }
            out[len] = '\0';
            env->CallVoidMethod(is, close_in);
            threw(env);
        }
    } while (false);
    if (conn != nullptr) {
        env->CallVoidMethod(conn, disconnect);
        threw(env);
    }
    env->PopLocalFrame(nullptr);
    return ok;
}

extern "C" bool pc_slippi_http_android(const char* url, const char* body, const char* bearer, bool post,
                                      char* out, size_t cap, int* status) {
    return pc_http_android(url, body, body ? strlen(body) : 0, "application/json", bearer, post, 15000, out,
                           cap, status);
}

#endif
