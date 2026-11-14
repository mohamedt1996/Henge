/* SPDX-License-Identifier: GPL-2.0-or-later */
/* dev.henge.android.GpuView's native side. */
#include <android/native_window_jni.h>
#include <jni.h>

#include "replay.h"
#ifdef HENGE_REIMS
#include "../reims/reims_host.h"
#endif

static const char *str(JNIEnv *env, jstring s) { return s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL; }
static void unstr(JNIEnv *env, jstring s, const char *c) { if (s && c) (*env)->ReleaseStringUTFChars(env, s, c); }

JNIEXPORT jlong JNICALL Java_dev_henge_android_GpuView_nativeStart(JNIEnv *env, jclass cls, jstring link, jstring tool, jstring cache) {
    (void)cls;
    const char *l = str(env, link), *t = str(env, tool), *c = str(env, cache);
    Replay *r = l ? replay_start(l, NULL, t, c) : NULL;
    unstr(env, link, l), unstr(env, tool, t), unstr(env, cache, c);
#ifdef HENGE_REIMS
    if (r && reims_host_running()) reims_host_set_sink(r); /* Graphics: reims: this screen shows its frames */
#endif
    return (jlong)(intptr_t)r;
}

JNIEXPORT void JNICALL Java_dev_henge_android_GpuView_nativeSurface(JNIEnv *env, jclass cls, jlong handle, jobject surface) {
    (void)cls;
    Replay *r = (Replay *)(intptr_t)handle;
    if (!r) return;
    replay_set_window(r, surface ? ANativeWindow_fromSurface(env, surface) : NULL);
}

JNIEXPORT void JNICALL Java_dev_henge_android_GpuView_nativeStop(JNIEnv *env, jclass cls, jlong handle) {
    (void)env, (void)cls;
#ifdef HENGE_REIMS
    reims_host_set_sink(NULL);
#endif
    replay_stop((Replay *)(intptr_t)handle);
}

/* Graphics: reims: the GPU device for the VM the service runs, from its start to its end. */
JNIEXPORT jboolean JNICALL Java_dev_henge_android_GpuView_nativeReimsStart(JNIEnv *env, jclass cls, jstring path, jstring log, jstring home, jstring vulkan) {
    (void)cls;
#ifdef HENGE_REIMS
    const char *p = str(env, path), *l = str(env, log), *h = str(env, home), *v = str(env, vulkan);
    int ok = p && reims_host_start(p, l, h, v) == 0;
    unstr(env, path, p), unstr(env, log, l), unstr(env, home, h), unstr(env, vulkan, v);
    return ok ? JNI_TRUE : JNI_FALSE;
#else
    (void)env, (void)path, (void)log, (void)home, (void)vulkan;
    return JNI_FALSE; /* this build has no reims-vgpu */
#endif
}

JNIEXPORT void JNICALL Java_dev_henge_android_GpuView_nativeReimsStop(JNIEnv *env, jclass cls) {
    (void)env, (void)cls;
#ifdef HENGE_REIMS
    reims_host_stop();
#endif
}

/* Settings > Mouse: Android's pointer (reims_host_native_pointer). */
JNIEXPORT void JNICALL Java_dev_henge_android_GpuView_nativeReimsNativePointer(JNIEnv *env, jclass cls, jboolean on) {
    (void)env, (void)cls;
#ifdef HENGE_REIMS
    reims_host_native_pointer(on == JNI_TRUE);
#else
    (void)on;
#endif
}

/* The Mac's pointer glyph for Android's pointer: premultiplied RGBA when it changed since knownSeq, else
   null; meta (6 ints): seq, width, height, hot x, hot y, shown. */
JNIEXPORT jbyteArray JNICALL Java_dev_henge_android_GpuView_nativeReimsCursor(JNIEnv *env, jclass cls, jint knownSeq, jintArray meta) {
    (void)cls;
#ifdef HENGE_REIMS
    static unsigned char buf[512 * 512 * 4];
    int m[6] = {0};
    int n = reims_host_cursor((unsigned)knownSeq, buf, (int)sizeof buf, m);
    if (meta && (*env)->GetArrayLength(env, meta) >= 6) (*env)->SetIntArrayRegion(env, meta, 0, 6, m);
    if (n <= 0) return NULL;
    jbyteArray a = (*env)->NewByteArray(env, n);
    if (a) (*env)->SetByteArrayRegion(env, a, 0, n, (const jbyte *)buf);
    return a;
#else
    (void)env, (void)knownSeq, (void)meta;
    return NULL;
#endif
}
