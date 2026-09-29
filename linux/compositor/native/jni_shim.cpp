#include "android_output.h"
#include "compositor_core.h"

#include <jni.h>
#include <cstdlib>
#include <cstring>

static JavaVM* vm;
static jobject service;
static jmethodID request_method;
static jmethodID close_method;

static JNIEnv* callback_env(bool* attached) {
  *attached = false;
  if (!vm) return nullptr;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
  if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
  *attached = true;
  return env;
}

extern "C" void maton_java_request_window(int id, int width, int height) {
  bool attached;
  JNIEnv* env = callback_env(&attached);
  if (env && service && request_method) {
    env->CallVoidMethod(service, request_method, id, width, height);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (attached) vm->DetachCurrentThread();
}

extern "C" void maton_java_close_window(int id) {
  bool attached;
  JNIEnv* env = callback_env(&attached);
  if (env && service && close_method) {
    env->CallVoidMethod(service, close_method, id);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (attached) vm->DetachCurrentThread();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_matonos_compositor_CompositorService_nativeStart(JNIEnv* env, jclass, jstring socket,
                                                           jstring runtime, jobject owner) {
  env->GetJavaVM(&vm);
  jobject global = env->NewGlobalRef(owner);
  jclass cls = env->GetObjectClass(owner);
  jmethodID request = env->GetMethodID(cls, "onNativeToplevel", "(III)V");
  jmethodID close = env->GetMethodID(cls, "onNativeToplevelClosed", "(I)V");
  env->DeleteLocalRef(cls);
  if (!global || !request || !close) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (global) env->DeleteGlobalRef(global);
    return JNI_FALSE;
  }
  service = global; request_method = request; close_method = close;
  const char* s = socket ? env->GetStringUTFChars(socket, nullptr) : nullptr;
  const char* r = runtime ? env->GetStringUTFChars(runtime, nullptr) : nullptr;
  bool ok = maton_core_start(s ? s : "wayland-0", r ? r : "/data/local/tmp/wayland");
  if (s) env->ReleaseStringUTFChars(socket, s);
  if (r) env->ReleaseStringUTFChars(runtime, r);
  if (!ok) {
    env->DeleteGlobalRef(service); service = nullptr; request_method = close_method = nullptr;
  }
  return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_matonos_compositor_CompositorService_nativeStop(JNIEnv* env, jclass) {
  maton_core_stop();
  if (service) env->DeleteGlobalRef(service);
  service = nullptr; request_method = close_method = nullptr;
}
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_compositor_CompositorService_nativeLaunchDemo(JNIEnv*, jclass) { maton_core_launch_demo(); }
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_compositor_CompositorService_nativeAttach(JNIEnv* env, jclass, jint id,
                                                            jobject surface, jint width, jint height) {
  MatonSurfaceOutput* output = static_cast<MatonSurfaceOutput*>(calloc(1, sizeof(*output)));
  if (!output || !maton_surface_output_init(output, env, surface, "maton-wayland-window")) {
    free(output); return;
  }
  maton_core_attach(id, output, width, height);
}
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_compositor_CompositorService_nativeDetach(JNIEnv*, jclass, jint id) { maton_core_detach(id); }
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_compositor_CompositorService_nativeResize(JNIEnv*, jclass, jint id, jint w, jint h) { maton_core_resize(id,w,h); }
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_compositor_CompositorService_nativeKey(JNIEnv*, jclass, jint id, jint key, jint scan,
                                                         jint action, jint meta, jlong time) {
  maton_core_key(id,key,scan,action,meta,time);
}
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_compositor_CompositorService_nativeMotion(JNIEnv*, jclass, jint id, jfloat x, jfloat y,
                                                            jfloat vs, jfloat hs, jint action,
                                                            jint buttons, jlong time) {
  maton_core_motion(id,x,y,vs,hs,action,buttons,time);
}
