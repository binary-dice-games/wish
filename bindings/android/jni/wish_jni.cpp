// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file wish_jni.cpp
 * @brief JNI glue for `com.bdg.wish.{Key,Dynamic}`.
 *
 * Every `Java_*` export here does the same three things: unpack JNI
 * arguments into C types, call one `bison_c.h` function, and either return
 * a value or throw `BisonException` via `throw_bison_exception()`. Mirrors
 * bison's own `bindings/android/jni/bison_jni.cpp`, minus the class/method
 * registry section (`bison_add_class`/`bison_add_method`/`bison_call`):
 * this binding is a wish *client* only (like the C#/Python calculator and
 * nano examples) and never hosts locally-registered classes or methods
 * of its own, so `Dynamic` here is a plain value type -- built and read,
 * never dispatched into.
 */

#include <jni.h>

#include <vector>

#include "jni_util.hpp"

using namespace bdg::wish::jni;

namespace {

// Two-call-convention helper for bison_get_vector_*(h, name, buf, buf_len,
// &len): query the length with buf=NULL, then fetch into a right-sized
// buffer.
template <typename Elem, typename Fn>
std::vector<Elem> read_vector(bison_handle h, bison_hash name, Fn fn, JNIEnv* env, bool* ok) {
  size_t len = 0;
  bison_error err = fn(h, name, static_cast<Elem*>(nullptr), 0, &len);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    *ok = false;
    return {};
  }
  std::vector<Elem> buf(len);
  if (len > 0) {
    err = fn(h, name, buf.data(), len, &len);
    if (err != BISON_OK) {
      throw_bison_exception(env, err);
      *ok = false;
      return {};
    }
  }
  *ok = true;
  return buf;
}

}  // namespace

extern "C" {

// ─── Key ────────────────────────────────────────────────────────────────

JNIEXPORT jint JNICALL Java_com_bdg_wish_Key_nativeKey(JNIEnv* env, jclass, jstring name) {
  jstring_view v(env, name);
  return static_cast<jint>(wish_key(v.c_str()));
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Dynamic_nativeCreate(JNIEnv*, jclass, jint class_name_hash) {
  return to_jlong(bison_create(static_cast<bison_hash>(class_name_hash)));
}

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Dynamic_nativeFromJson(JNIEnv* env, jclass, jstring json) {
  jstring_view v(env, json);
  bison_handle h = bison_from_json(v.c_str());
  if (!h) throw_bison_exception(env, BISON_ERR_PARSE);
  return to_jlong(h);
}

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Dynamic_nativeDeserialize(JNIEnv* env, jclass, jbyteArray data) {
  jsize len = data ? env->GetArrayLength(data) : 0;
  std::vector<uint8_t> buf(len);
  if (len > 0) env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(buf.data()));
  bison_handle out = nullptr;
  bison_error err = bison_deserialize(buf.data(), static_cast<size_t>(len), &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return 0;
  }
  return to_jlong(out);
}

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Dynamic_nativeClone(JNIEnv*, jclass, jlong handle) {
  return to_jlong(bison_clone(from_jlong<bison_handle>(handle)));
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeRelease(JNIEnv*, jclass, jlong handle) {
  bison_release(from_jlong<bison_handle>(handle));
}

// ─── Scalar setters ─────────────────────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeSetInt(
    JNIEnv* env, jclass, jlong handle, jint name, jint value) {
  bison_error err = bison_set_int(from_jlong<bison_handle>(handle), name, value);
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeSetFloat(
    JNIEnv* env, jclass, jlong handle, jint name, jfloat value) {
  bison_error err = bison_set_float(from_jlong<bison_handle>(handle), name, value);
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeSetBool(
    JNIEnv* env, jclass, jlong handle, jint name, jboolean value) {
  bison_error err = bison_set_bool(from_jlong<bison_handle>(handle), name, value == JNI_TRUE ? 1 : 0);
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeSetString(
    JNIEnv* env, jclass, jlong handle, jint name, jstring value) {
  jstring_view v(env, value);
  bison_error err = bison_set_string(from_jlong<bison_handle>(handle), name, v.c_str());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeSetKey(
    JNIEnv* env, jclass, jlong handle, jint name, jint value_hash) {
  bison_error err = bison_set_key(from_jlong<bison_handle>(handle), name, static_cast<bison_hash>(value_hash));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeSetObject(
    JNIEnv* env, jclass, jlong handle, jint name, jlong value_handle) {
  bison_error err =
      bison_set_object(from_jlong<bison_handle>(handle), name, from_jlong<bison_handle>(value_handle));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

// ─── Scalar getters ─────────────────────────────────────────────────────

JNIEXPORT jint JNICALL Java_com_bdg_wish_Dynamic_nativeGetInt(JNIEnv* env, jclass, jlong handle, jint name) {
  int32_t out = 0;
  bison_error err = bison_get_int(from_jlong<bison_handle>(handle), name, &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return out;
}

JNIEXPORT jfloat JNICALL Java_com_bdg_wish_Dynamic_nativeGetFloat(JNIEnv* env, jclass, jlong handle, jint name) {
  float out = 0;
  bison_error err = bison_get_float(from_jlong<bison_handle>(handle), name, &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return out;
}

JNIEXPORT jboolean JNICALL Java_com_bdg_wish_Dynamic_nativeGetBool(JNIEnv* env, jclass, jlong handle, jint name) {
  int out = 0;
  bison_error err = bison_get_bool(from_jlong<bison_handle>(handle), name, &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return out != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_bdg_wish_Dynamic_nativeGetString(JNIEnv* env, jclass, jlong handle, jint name) {
  bison_handle h = from_jlong<bison_handle>(handle);
  size_t len = 0;
  bison_error err = bison_get_string(h, name, nullptr, 0, &len);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  std::vector<char> buf(len + 1);
  err = bison_get_string(h, name, buf.data(), buf.size(), &len);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  return env->NewStringUTF(buf.data());
}

JNIEXPORT jint JNICALL Java_com_bdg_wish_Dynamic_nativeGetKey(JNIEnv* env, jclass, jlong handle, jint name) {
  bison_hash out = 0;
  bison_error err = bison_get_key(from_jlong<bison_handle>(handle), name, &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return static_cast<jint>(out);
}

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Dynamic_nativeGetObject(JNIEnv* env, jclass, jlong handle, jint name) {
  bison_handle out = nullptr;
  bison_error err = bison_get_object(from_jlong<bison_handle>(handle), name, &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return 0;
  }
  return to_jlong(out);
}

// ─── Vector fields ─────────────────────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeSetVectorBool(
    JNIEnv* env, jclass, jlong handle, jint name, jbooleanArray values) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jboolean> src(len);
  std::vector<int> ints(len);
  if (len > 0) {
    env->GetBooleanArrayRegion(values, 0, len, src.data());
    for (jsize i = 0; i < len; ++i) ints[i] = src[i] != JNI_FALSE ? 1 : 0;
  }
  bison_error err =
      bison_set_vector_bool(from_jlong<bison_handle>(handle), name, ints.data(), static_cast<size_t>(len));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeSetVectorInt(
    JNIEnv* env, jclass, jlong handle, jint name, jintArray values) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jint> buf(len);
  if (len > 0) env->GetIntArrayRegion(values, 0, len, buf.data());
  bison_error err = bison_set_vector_int(
      from_jlong<bison_handle>(handle), name, reinterpret_cast<const int32_t*>(buf.data()),
      static_cast<size_t>(len));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeSetVectorFloat(
    JNIEnv* env, jclass, jlong handle, jint name, jfloatArray values) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jfloat> buf(len);
  if (len > 0) env->GetFloatArrayRegion(values, 0, len, buf.data());
  bison_error err =
      bison_set_vector_float(from_jlong<bison_handle>(handle), name, buf.data(), static_cast<size_t>(len));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_wish_Dynamic_nativeSetVectorBytes(
    JNIEnv* env, jclass, jlong handle, jint name, jbyteArray values) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jbyte> buf(len);
  if (len > 0) env->GetByteArrayRegion(values, 0, len, buf.data());
  bison_error err = bison_set_vector_bytes(
      from_jlong<bison_handle>(handle), name, reinterpret_cast<const uint8_t*>(buf.data()),
      static_cast<size_t>(len));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT jbooleanArray JNICALL Java_com_bdg_wish_Dynamic_nativeGetVectorBool(
    JNIEnv* env, jclass, jlong handle, jint name) {
  bool ok = false;
  std::vector<int> ints = read_vector<int>(from_jlong<bison_handle>(handle), name, bison_get_vector_bool, env, &ok);
  if (!ok) return nullptr;
  jbooleanArray out = env->NewBooleanArray(static_cast<jsize>(ints.size()));
  std::vector<jboolean> bools(ints.size());
  for (size_t i = 0; i < ints.size(); ++i) bools[i] = ints[i] != 0 ? JNI_TRUE : JNI_FALSE;
  if (!bools.empty()) env->SetBooleanArrayRegion(out, 0, static_cast<jsize>(bools.size()), bools.data());
  return out;
}

JNIEXPORT jintArray JNICALL Java_com_bdg_wish_Dynamic_nativeGetVectorInt(
    JNIEnv* env, jclass, jlong handle, jint name) {
  bool ok = false;
  std::vector<int32_t> vals =
      read_vector<int32_t>(from_jlong<bison_handle>(handle), name, bison_get_vector_int, env, &ok);
  if (!ok) return nullptr;
  jintArray out = env->NewIntArray(static_cast<jsize>(vals.size()));
  if (!vals.empty()) {
    env->SetIntArrayRegion(out, 0, static_cast<jsize>(vals.size()), reinterpret_cast<const jint*>(vals.data()));
  }
  return out;
}

JNIEXPORT jfloatArray JNICALL Java_com_bdg_wish_Dynamic_nativeGetVectorFloat(
    JNIEnv* env, jclass, jlong handle, jint name) {
  bool ok = false;
  std::vector<float> vals =
      read_vector<float>(from_jlong<bison_handle>(handle), name, bison_get_vector_float, env, &ok);
  if (!ok) return nullptr;
  jfloatArray out = env->NewFloatArray(static_cast<jsize>(vals.size()));
  if (!vals.empty()) env->SetFloatArrayRegion(out, 0, static_cast<jsize>(vals.size()), vals.data());
  return out;
}

JNIEXPORT jbyteArray JNICALL Java_com_bdg_wish_Dynamic_nativeGetVectorBytes(
    JNIEnv* env, jclass, jlong handle, jint name) {
  bool ok = false;
  std::vector<uint8_t> vals =
      read_vector<uint8_t>(from_jlong<bison_handle>(handle), name, bison_get_vector_bytes, env, &ok);
  if (!ok) return nullptr;
  jbyteArray out = env->NewByteArray(static_cast<jsize>(vals.size()));
  if (!vals.empty()) {
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(vals.size()), reinterpret_cast<const jbyte*>(vals.data()));
  }
  return out;
}

// ─── Serialization ──────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL Java_com_bdg_wish_Dynamic_nativeSize(JNIEnv*, jclass, jlong handle) {
  return static_cast<jlong>(bison_size(from_jlong<bison_handle>(handle)));
}

JNIEXPORT jbyteArray JNICALL Java_com_bdg_wish_Dynamic_nativeSerialize(JNIEnv* env, jclass, jlong handle) {
  uint8_t* data = nullptr;
  size_t len = 0;
  bison_error err = bison_serialize(from_jlong<bison_handle>(handle), &data, &len);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  jbyteArray out = env->NewByteArray(static_cast<jsize>(len));
  if (len > 0) env->SetByteArrayRegion(out, 0, static_cast<jsize>(len), reinterpret_cast<const jbyte*>(data));
  bison_free_buffer(data);
  return out;
}

JNIEXPORT jstring JNICALL Java_com_bdg_wish_Dynamic_nativeToJson(JNIEnv* env, jclass, jlong handle, jint indent) {
  char* out = nullptr;
  bison_error err = bison_to_json(from_jlong<bison_handle>(handle), indent, &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  jstring result = env->NewStringUTF(out);
  bison_free_string(out);
  return result;
}

}  // extern "C"
