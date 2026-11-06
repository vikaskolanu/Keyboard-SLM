#include <jni.h>
#include <string>
#include "keyboard_slm_c_api.h"

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_initEngine(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return reinterpret_cast<jlong>(kslm_create());
}

JNIEXPORT void JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_destroyEngine(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env; (void)thiz;
    if (handle != 0) {
        kslm_destroy(reinterpret_cast<kslm_engine_t*>(handle));
    }
}

JNIEXPORT void JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_learnWord(
    JNIEnv *env, jobject thiz, jlong handle, jstring prev2, jstring prev1, jstring word, jint weight) {
    (void)thiz;
    if (handle == 0 || !word) return;
    const char *c_p2 = prev2 ? env->GetStringUTFChars(prev2, nullptr) : "";
    const char *c_p1 = prev1 ? env->GetStringUTFChars(prev1, nullptr) : "";
    const char *c_w = env->GetStringUTFChars(word, nullptr);

    kslm_learn_word(reinterpret_cast<kslm_engine_t*>(handle), c_p2, c_p1, c_w, static_cast<uint32_t>(weight));

    if (prev2) env->ReleaseStringUTFChars(prev2, c_p2);
    if (prev1) env->ReleaseStringUTFChars(prev1, c_p1);
    env->ReleaseStringUTFChars(word, c_w);
}

JNIEXPORT void JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_learnSentence(
    JNIEnv *env, jobject thiz, jlong handle, jstring sentence, jint weight) {
    (void)thiz;
    if (handle == 0 || !sentence) return;
    const char *c_sent = env->GetStringUTFChars(sentence, nullptr);
    kslm_learn_sentence(reinterpret_cast<kslm_engine_t*>(handle), c_sent, static_cast<uint32_t>(weight));
    env->ReleaseStringUTFChars(sentence, c_sent);
}

JNIEXPORT jboolean JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_forgetWord(
    JNIEnv *env, jobject thiz, jlong handle, jstring word) {
    (void)thiz;
    if (handle == 0 || !word) return JNI_FALSE;
    const char *c_w = env->GetStringUTFChars(word, nullptr);
    int ok = kslm_forget_word(reinterpret_cast<kslm_engine_t*>(handle), c_w);
    env->ReleaseStringUTFChars(word, c_w);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_predictJson(
    JNIEnv *env, jobject thiz, jlong handle, jstring prev2, jstring prev1, jstring prefix, jint top_k) {
    (void)thiz;
    if (handle == 0) return env->NewStringUTF("[]");
    const char *c_p2 = prev2 ? env->GetStringUTFChars(prev2, nullptr) : "";
    const char *c_p1 = prev1 ? env->GetStringUTFChars(prev1, nullptr) : "";
    const char *c_pref = prefix ? env->GetStringUTFChars(prefix, nullptr) : "";

    char *json_res = kslm_predict_json(
        reinterpret_cast<kslm_engine_t*>(handle),
        c_p2,
        c_p1,
        c_pref,
        static_cast<size_t>(top_k > 0 ? top_k : 3)
    );

    if (prev2) env->ReleaseStringUTFChars(prev2, c_p2);
    if (prev1) env->ReleaseStringUTFChars(prev1, c_p1);
    if (prefix) env->ReleaseStringUTFChars(prefix, c_pref);

    jstring out = env->NewStringUTF(json_res ? json_res : "[]");
    if (json_res) kslm_free_string(json_res);
    return out;
}

JNIEXPORT jstring JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_listSendersJson(
    JNIEnv *env, jobject thiz, jstring chat_content) {
    (void)thiz;
    if (!chat_content) return env->NewStringUTF("[]");
    const char *c_content = env->GetStringUTFChars(chat_content, nullptr);
    char *json_res = kslm_list_senders_json(c_content);
    env->ReleaseStringUTFChars(chat_content, c_content);

    jstring out = env->NewStringUTF(json_res ? json_res : "[]");
    if (json_res) kslm_free_string(json_res);
    return out;
}

JNIEXPORT jlong JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_importChat(
    JNIEnv *env, jobject thiz, jlong handle, jstring chat_content, jstring target_sender) {
    (void)thiz;
    if (handle == 0 || !chat_content) return 0;
    const char *c_content = env->GetStringUTFChars(chat_content, nullptr);
    const char *c_sender = target_sender ? env->GetStringUTFChars(target_sender, nullptr) : "";

    size_t count = kslm_import_chat(reinterpret_cast<kslm_engine_t*>(handle), c_content, c_sender);

    env->ReleaseStringUTFChars(chat_content, c_content);
    if (target_sender) env->ReleaseStringUTFChars(target_sender, c_sender);
    return static_cast<jlong>(count);
}

JNIEXPORT jstring JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_getStatsJson(
    JNIEnv *env, jobject thiz, jlong handle) {
    (void)thiz;
    if (handle == 0) return env->NewStringUTF("{}");
    char *json_res = kslm_get_stats_json(reinterpret_cast<kslm_engine_t*>(handle));
    jstring out = env->NewStringUTF(json_res ? json_res : "{}");
    if (json_res) kslm_free_string(json_res);
    return out;
}

JNIEXPORT jboolean JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_saveModel(
    JNIEnv *env, jobject thiz, jlong handle, jstring filepath) {
    (void)thiz;
    if (handle == 0 || !filepath) return JNI_FALSE;
    const char *c_path = env->GetStringUTFChars(filepath, nullptr);
    int ok = kslm_save(reinterpret_cast<kslm_engine_t*>(handle), c_path);
    env->ReleaseStringUTFChars(filepath, c_path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_loadModel(
    JNIEnv *env, jobject thiz, jlong handle, jstring filepath) {
    (void)thiz;
    if (handle == 0 || !filepath) return JNI_FALSE;
    const char *c_path = env->GetStringUTFChars(filepath, nullptr);
    int ok = kslm_load(reinterpret_cast<kslm_engine_t*>(handle), c_path);
    env->ReleaseStringUTFChars(filepath, c_path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_antigravity_keyboardslm_app_KeyboardEngineJni_clearModel(
    JNIEnv *env, jobject thiz, jlong handle) {
    (void)env; (void)thiz;
    if (handle != 0) {
        kslm_clear(reinterpret_cast<kslm_engine_t*>(handle));
    }
}

} // extern "C"
