#ifndef KEYBOARD_SLM_C_API_H
#define KEYBOARD_SLM_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque engine pointer
typedef struct kslm_engine kslm_engine_t;

// Lifecycle
kslm_engine_t* kslm_create(void);
void kslm_destroy(kslm_engine_t* engine);

// Learning
void kslm_learn_sentence(kslm_engine_t* engine, const char* sentence, uint32_t weight);
void kslm_learn_word(kslm_engine_t* engine, const char* prev2, const char* prev1, const char* word, uint32_t weight);
int kslm_forget_word(kslm_engine_t* engine, const char* word);
int kslm_contains_word(kslm_engine_t* engine, const char* word);
void kslm_clear(kslm_engine_t* engine);

// Prediction: returns a JSON array of predictions, e.g. [{"word":"chesthunav","score":1.25,"source":"trigram"}, ...]
// Caller must free returned string using kslm_free_string().
char* kslm_predict_json(kslm_engine_t* engine, const char* prev2, const char* prev1, const char* prefix, size_t top_k);

// Chat export utilities:
// kslm_list_senders_json returns a JSON array: [{"name":"Vikas Kolanu","count":3742}, ...]
char* kslm_list_senders_json(const char* chat_content);
size_t kslm_import_chat(kslm_engine_t* engine, const char* chat_content, const char* target_sender);

// Telemetry & stats: returns JSON: {"unique_words":..., "total_tokens":..., "bigram_entries":..., "trigram_entries":..., "memory_kb":...}
char* kslm_get_stats_json(kslm_engine_t* engine);

// Persistence
int kslm_save(kslm_engine_t* engine, const char* filepath);
int kslm_load(kslm_engine_t* engine, const char* filepath);

// Free heap-allocated strings returned by kslm_*_json functions
void kslm_free_string(char* s);

#ifdef __cplusplus
}
#endif

#endif // KEYBOARD_SLM_C_API_H
