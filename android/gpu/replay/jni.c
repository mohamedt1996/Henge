/* SPDX-License-Identifier: GPL-2.0-or-later */
/* dev.henge.android.GpuView's native side. */
#include <android/native_window_jni.h>
#include <jni.h>

#include "replay.h"

static const char *str(JNIEnv *env, jstring s) { return s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL; }
static void unstr(JNIEnv *env, jstring s, const char *c) { if (s && c) (*env)->ReleaseStringUTFChars(env, s, c); }

JNIEXPORT jlong JNICALL Java_dev_henge_android_GpuView_nativeStart(JNIEnv *env, jclass cls, jstring link, jstring tool, jstring cache) {
    (void)cls;
    const char *l = str(env, link), *t = str(env, tool), *c = str(env, cache);
    Replay *r = l ? replay_start(l, NULL, t, c) : NULL;
    unstr(env, link, l), unstr(env, tool, t), unstr(env, cache, c);
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
    replay_stop((Replay *)(intptr_t)handle);
}
