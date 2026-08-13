// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file wish_rmi_jni.cpp
 * @brief JNI glue for `com.bdg.wish.{Client,Proxy}`.
 *
 * `Client` wraps `wish_client_c.h` (session lifecycle, template
 * registration/instantiation, object instantiation, embedded apps, file
 * transfer, logging); `Proxy` wraps `rmi_c.h`'s `rmi_proxy_*` functions
 * (`bison_jni.cpp`'s counterpart in bison's own Android binding is
 * `rmi_jni.cpp`, which this mirrors) plus `rmi_proxy_on_event()`, which
 * bison's own Java binding does not yet expose but this one needs: driving
 * a UI template's button clicks and other server-pushed events is this
 * binding's whole point. Only the synchronous (non-`_async`) operations are
 * bound, matching bison's own documented gap for `rmi_future_handle`.
 *
 * Two upcall trampolines cross from C back into Java, both needing a
 * `JNIEnv` obtained via `AttachCurrentThread` because they fire on the
 * library's internal RMI worker thread rather than the Java thread that
 * originated the call (`wish_client_run_with_params`'s and
 * `rmi_proxy_on_event`'s doc comments both say so) -- the same pattern
 * `bison_jni.cpp`'s `method_trampoline` uses for `BisonMethod` callbacks.
 */

#include <jni.h>

#include <vector>

#include "jni_util.hpp"

using namespace bdg::wish::jni;

namespace {

JavaVM* g_jvm = nullptr;
jclass g_dynamic_class = nullptr;
jmethodID g_dynamic_wrap_borrowed = nullptr;
jclass g_event_handler_class = nullptr;
jmethodID g_event_handler_on_event = nullptr;
jclass g_session_callback_class = nullptr;
jmethodID g_session_callback_run = nullptr;

JNIEnv* attach(bool* attached) {
  JNIEnv* env = nullptr;
  *attached = false;
  if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK || !env) return nullptr;
    *attached = true;
  }
  return env;
}

/// Context for one `wish_client_run_with_params()` call; lives only for the
/// duration of that (blocking) call, freed by `nativeRunWithParams` after it
/// returns.
struct session_ctx {
  jobject callback;   // global ref to the SessionCallback instance
  jobject client_obj; // global ref to the Client instance run() was called on
};

void JNICALL session_trampoline(wish_client_handle, void* user) {
  auto* ctx = static_cast<session_ctx*>(user);
  bool attached = false;
  JNIEnv* env = attach(&attached);
  if (!env) return;

  env->CallVoidMethod(ctx->callback, g_session_callback_run, ctx->client_obj);
  // wish_session_fn cannot propagate an exception across the C ABI -- like
  // BisonMethod callbacks (see bison_jni.cpp), a Java exception here has
  // nowhere to go but be logged and dropped.
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    log_error("wish_jni", "SessionCallback threw; the exception could not cross the C ABI and was dropped");
  }

  if (attached) g_jvm->DetachCurrentThread();
}

/// Context captured for one `Proxy.onEvent()` registration; leaked for the
/// process lifetime, matching bison_jni.cpp's `method_ctx` -- `rmi_c.h` has
/// no unregister call to hang a destructor off of.
struct event_ctx {
  jobject callback; // global ref to the EventHandler instance
};

void JNICALL event_trampoline(bison_handle params, void* user) {
  auto* ctx = static_cast<event_ctx*>(user);
  bool attached = false;
  JNIEnv* env = attach(&attached);
  if (!env) return;

  // params is borrowed (see rmi_proxy_on_event()'s doc comment) -- wrap it
  // the same way bison_jni.cpp's method_trampoline wraps self/params/result,
  // so the Java-side Dynamic must not (and structurally cannot, via
  // Dynamic.close()'s owned check) release it.
  jobject j_params = env->CallStaticObjectMethod(g_dynamic_class, g_dynamic_wrap_borrowed, to_jlong(params));

  env->CallVoidMethod(ctx->callback, g_event_handler_on_event, j_params);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    log_error("wish_jni", "EventHandler threw; the exception could not cross the C ABI and was dropped");
  }

  if (j_params) env->DeleteLocalRef(j_params);
  if (attached) g_jvm->DetachCurrentThread();
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  g_jvm = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

  jclass dynamic_local = env->FindClass("com/bdg/wish/Dynamic");
  if (!dynamic_local) return JNI_ERR;
  g_dynamic_class = static_cast<jclass>(env->NewGlobalRef(dynamic_local));
  g_dynamic_wrap_borrowed =
      env->GetStaticMethodID(g_dynamic_class, "wrapBorrowed", "(J)Lcom/bdg/wish/Dynamic;");

  jclass event_local = env->FindClass("com/bdg/wish/EventHandler");
  if (!event_local) return JNI_ERR;
  g_event_handler_class = static_cast<jclass>(env->NewGlobalRef(event_local));
  g_event_handler_on_event = env->GetMethodID(g_event_handler_class, "onEvent", "(Lcom/bdg/wish/Dynamic;)V");

  jclass session_local = env->FindClass("com/bdg/wish/SessionCallback");
  if (!session_local) return JNI_ERR;
  g_session_callback_class = static_cast<jclass>(env->NewGlobalRef(session_local));
  g_session_callback_run = env->GetMethodID(g_session_callback_class, "run", "(Lcom/bdg/wish/Client;)V");

  return JNI_VERSION_1_6;
}

// ─── Client lifecycle ───────────────────────────────────────────────────

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Client_nativeTcpCreate(JNIEnv* env, jclass, jstring host, jint port) {
  jstring_view h(env, host);
  return to_jlong(wish_client_tcp_create(h.c_str(), static_cast<uint16_t>(port)));
}

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Client_nativeTlsCreate(JNIEnv* env, jclass, jstring host, jint port) {
  jstring_view h(env, host);
  return to_jlong(wish_client_tls_create(h.c_str(), static_cast<uint16_t>(port)));
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Client_nativeDestroy(JNIEnv*, jclass, jlong handle) {
  wish_client_destroy(from_jlong<wish_client_handle>(handle));
}

JNIEXPORT jint JNICALL Java_com_bdg_wish_Client_nativeRunWithParams(
    JNIEnv* env, jclass, jlong handle, jobject client_obj, jobject callback, jlong params_handle) {
  session_ctx ctx{env->NewGlobalRef(callback), env->NewGlobalRef(client_obj)};
  wish_error err = wish_client_run_with_params(
      from_jlong<wish_client_handle>(handle), &session_trampoline, &ctx, from_jlong<bison_handle>(params_handle));
  env->DeleteGlobalRef(ctx.callback);
  env->DeleteGlobalRef(ctx.client_obj);
  return static_cast<jint>(err);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Client_nativeWait(JNIEnv*, jclass, jlong handle) {
  wish_client_wait(from_jlong<wish_client_handle>(handle));
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Client_nativeQuit(JNIEnv*, jclass, jlong handle) {
  wish_client_quit(from_jlong<wish_client_handle>(handle));
}

JNIEXPORT jstring JNICALL Java_com_bdg_wish_Client_nativeLastError(JNIEnv* env, jclass, jlong handle) {
  return to_jstring(env, wish_last_error(from_jlong<wish_client_handle>(handle)));
}

// ─── Style ──────────────────────────────────────────────────────────────

JNIEXPORT jint JNICALL Java_com_bdg_wish_Client_nativeSetStylePreset(
    JNIEnv* env, jclass, jlong handle, jstring preset) {
  jstring_view p(env, preset);
  return static_cast<jint>(wish_set_style_preset(from_jlong<wish_client_handle>(handle), p.c_str()));
}

// ─── Template management ────────────────────────────────────────────────

JNIEXPORT jint JNICALL Java_com_bdg_wish_Client_nativeRegisterTemplate(
    JNIEnv* env, jclass, jlong handle, jstring name, jstring descriptor) {
  jstring_view n(env, name);
  jstring_view d(env, descriptor);
  return static_cast<jint>(wish_register_template(from_jlong<wish_client_handle>(handle), n.c_str(), d.c_str()));
}

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Client_nativeInstantiateTemplate(
    JNIEnv* env, jclass, jlong handle, jstring name, jstring prefix) {
  jstring_view n(env, name);
  jstring_view p(env, prefix);
  return to_jlong(wish_instantiate_template(from_jlong<wish_client_handle>(handle), n.c_str(), p.c_str()));
}

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Client_nativeProxyGet(JNIEnv* env, jclass, jlong handle, jstring dotPath) {
  jstring_view p(env, dotPath);
  return to_jlong(wish_proxy_get(from_jlong<wish_client_handle>(handle), p.c_str()));
}

JNIEXPORT jint JNICALL Java_com_bdg_wish_Client_nativeRelease(JNIEnv* env, jclass, jlong handle, jstring prefix) {
  jstring_view p(env, prefix);
  return static_cast<jint>(wish_release(from_jlong<wish_client_handle>(handle), p.c_str()));
}

// ─── Object instantiation ───────────────────────────────────────────────

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Client_nativeInstantiate(
    JNIEnv*, jclass, jlong handle, jint ns_hash, jint class_hash, jlong params_handle) {
  return to_jlong(wish_instantiate(
      from_jlong<wish_client_handle>(handle), static_cast<wish_hash>(ns_hash), static_cast<wish_hash>(class_hash),
      from_jlong<bison_handle>(params_handle)));
}

// ─── Embedded apps ──────────────────────────────────────────────────────

JNIEXPORT jstring JNICALL Java_com_bdg_wish_Client_nativeListApps(JNIEnv* env, jclass) {
  char* out = nullptr;
  wish_error err = wish_list_apps(&out);
  if (err != WISH_OK) {
    throw_wish_exception(env, err, "list_apps");
    return nullptr;
  }
  jstring result = to_jstring(env, out);
  bison_free_string(out);
  return result;
}

JNIEXPORT jint JNICALL Java_com_bdg_wish_Client_nativeRunApp(
    JNIEnv* env, jclass, jlong handle, jstring app_name, jobjectArray args) {
  jstring_view name(env, app_name);
  jsize nargs = args ? env->GetArrayLength(args) : 0;
  // jstring_view is move/copy-disabled (see its declaration), so it can't
  // live in a std::vector here -- hold the jstring local refs and their
  // UTF-8 buffers directly instead, and release both only after the call.
  std::vector<jstring> jargs(nargs);
  std::vector<const char*> argv(nargs);
  for (jsize i = 0; i < nargs; ++i) {
    jargs[i] = static_cast<jstring>(env->GetObjectArrayElement(args, i));
    argv[i] = jargs[i] ? env->GetStringUTFChars(jargs[i], nullptr) : "";
  }
  wish_error err = wish_run_app(
      from_jlong<wish_client_handle>(handle), name.c_str(), argv.data(), static_cast<size_t>(nargs));
  for (jsize i = 0; i < nargs; ++i) {
    if (jargs[i]) {
      env->ReleaseStringUTFChars(jargs[i], argv[i]);
      env->DeleteLocalRef(jargs[i]);
    }
  }
  return static_cast<jint>(err);
}

// ─── File transfer ──────────────────────────────────────────────────────

JNIEXPORT jint JNICALL Java_com_bdg_wish_Client_nativeUploadFile(
    JNIEnv* env, jclass, jlong handle, jstring name, jbyteArray data) {
  jstring_view n(env, name);
  jsize len = data ? env->GetArrayLength(data) : 0;
  std::vector<jbyte> buf(len);
  if (len > 0) env->GetByteArrayRegion(data, 0, len, buf.data());
  return static_cast<jint>(wish_upload_file(
      from_jlong<wish_client_handle>(handle), n.c_str(), reinterpret_cast<const char*>(buf.data()),
      static_cast<size_t>(len)));
}

JNIEXPORT jbyteArray JNICALL Java_com_bdg_wish_Client_nativeDownloadFile(
    JNIEnv* env, jclass, jlong handle, jstring name) {
  jstring_view n(env, name);
  char* data = nullptr;
  size_t len = 0;
  wish_error err = wish_download_file(from_jlong<wish_client_handle>(handle), n.c_str(), &data, &len);
  if (err != WISH_OK) {
    throw_wish_exception(env, err, "download_file");
    return nullptr;
  }
  jbyteArray out = env->NewByteArray(static_cast<jsize>(len));
  if (len > 0) env->SetByteArrayRegion(out, 0, static_cast<jsize>(len), reinterpret_cast<const jbyte*>(data));
  bison_free_string(data);
  return out;
}

// ─── Logging ────────────────────────────────────────────────────────────

JNIEXPORT jint JNICALL Java_com_bdg_wish_Client_nativeLog(
    JNIEnv* env, jclass, jlong handle, jstring level, jstring msg) {
  jstring_view l(env, level);
  jstring_view m(env, msg);
  return static_cast<jint>(wish_log(from_jlong<wish_client_handle>(handle), l.c_str(), m.c_str()));
}

// ─── Proxy ────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_wish_Proxy_nativeSet(
    JNIEnv* env, jclass, jlong handle, jlong fields_handle, jlong timeout_ms) {
  rmi_error err = rmi_proxy_set(
      from_jlong<rmi_proxy_handle>(handle), from_jlong<bison_handle>(fields_handle), timeout_ms);
  if (err != RMI_OK) throw_rmi_exception(env, err);
}

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Proxy_nativeGet(
    JNIEnv* env, jclass, jlong handle, jlong projection_handle, jlong timeout_ms) {
  bison_handle result = nullptr;
  rmi_error err = rmi_proxy_get(
      from_jlong<rmi_proxy_handle>(handle), from_jlong<bison_handle>(projection_handle), &result, timeout_ms);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(result);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Proxy_nativeClear(JNIEnv* env, jclass, jlong handle, jlong timeout_ms) {
  rmi_error err = rmi_proxy_clear(from_jlong<rmi_proxy_handle>(handle), timeout_ms);
  if (err != RMI_OK) throw_rmi_exception(env, err);
}

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Proxy_nativeCall(
    JNIEnv* env, jclass, jlong handle, jint method_hash, jlong params_handle, jlong timeout_ms) {
  bison_handle result = nullptr;
  rmi_error err = rmi_proxy_call(
      from_jlong<rmi_proxy_handle>(handle), static_cast<bison_hash>(method_hash),
      from_jlong<bison_handle>(params_handle), &result, timeout_ms);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(result);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Proxy_nativeOnEvent(
    JNIEnv* env, jclass, jlong handle, jint event_name_hash, jobject callback) {
  auto* ctx = new event_ctx{env->NewGlobalRef(callback)};
  rmi_error err = rmi_proxy_on_event(
      from_jlong<rmi_proxy_handle>(handle), static_cast<bison_hash>(event_name_hash), &event_trampoline, ctx);
  if (err != RMI_OK) {
    env->DeleteGlobalRef(ctx->callback);
    delete ctx;
    throw_rmi_exception(env, err);
  }
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Proxy_nativeRelease(JNIEnv*, jclass, jlong handle) {
  rmi_proxy_release(from_jlong<rmi_proxy_handle>(handle));
}

}  // extern "C"
