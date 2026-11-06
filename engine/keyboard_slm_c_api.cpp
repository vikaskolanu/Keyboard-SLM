#include "keyboard_slm_c_api.h"
#include "keyboard_slm_engine.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

using namespace keyboard_slm;

struct kslm_engine {
    PersonalSLM engine;
    std::mutex mtx;
};

namespace {

char* duplicate_string(const std::string& str) {
    char* copy = static_cast<char*>(std::malloc(str.size() + 1));
    if (copy) {
        std::memcpy(copy, str.c_str(), str.size() + 1);
    }
    return copy;
}

std::string escape_json(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) <= 0x1f) {
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                } else {
                    o << c;
                }
        }
    }
    return o.str();
}

} // namespace

extern "C" {

kslm_engine_t* kslm_create(void) {
    return new (std::nothrow) kslm_engine();
}

void kslm_destroy(kslm_engine_t* engine) {
    delete engine;
}

void kslm_learn_sentence(kslm_engine_t* engine, const char* sentence, uint32_t weight) {
    if (!engine || !sentence) return;
    std::lock_guard<std::mutex> lock(engine->mtx);
    engine->engine.learn_sentence(sentence, weight);
}

void kslm_learn_word(kslm_engine_t* engine, const char* prev2, const char* prev1, const char* word, uint32_t weight) {
    if (!engine || !word) return;
    std::lock_guard<std::mutex> lock(engine->mtx);
    engine->engine.learn_word(prev2 ? prev2 : "", prev1 ? prev1 : "", word, weight);
}

int kslm_forget_word(kslm_engine_t* engine, const char* word) {
    if (!engine || !word) return 0;
    std::lock_guard<std::mutex> lock(engine->mtx);
    return engine->engine.forget_word(word) ? 1 : 0;
}

int kslm_contains_word(kslm_engine_t* engine, const char* word) {
    if (!engine || !word) return 0;
    std::lock_guard<std::mutex> lock(engine->mtx);
    return engine->engine.contains(word) ? 1 : 0;
}

void kslm_clear(kslm_engine_t* engine) {
    if (!engine) return;
    std::lock_guard<std::mutex> lock(engine->mtx);
    engine->engine.clear();
}

char* kslm_predict_json(kslm_engine_t* engine, const char* prev2, const char* prev1, const char* prefix, size_t top_k) {
    if (!engine) return duplicate_string("[]");
    std::lock_guard<std::mutex> lock(engine->mtx);

    auto preds = engine->engine.predict(
        prev2 ? prev2 : "",
        prev1 ? prev1 : "",
        prefix ? prefix : "",
        top_k
    );

    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < preds.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{"
           << "\"word\":\"" << escape_json(preds[i].word) << "\","
           << "\"score\":" << std::fixed << std::setprecision(4) << preds[i].score << ","
           << "\"source\":\"" << escape_json(preds[i].source) << "\""
           << "}";
    }
    ss << "]";
    return duplicate_string(ss.str());
}

char* kslm_list_senders_json(const char* chat_content) {
    if (!chat_content) return duplicate_string("[]");
    auto senders = PersonalSLM::list_chat_senders(chat_content);
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < senders.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{"
           << "\"name\":\"" << escape_json(senders[i].name) << "\","
           << "\"count\":" << senders[i].message_count
           << "}";
    }
    ss << "]";
    return duplicate_string(ss.str());
}

size_t kslm_import_chat(kslm_engine_t* engine, const char* chat_content, const char* target_sender) {
    if (!engine || !chat_content) return 0;
    std::lock_guard<std::mutex> lock(engine->mtx);
    return engine->engine.import_chat_txt(chat_content, target_sender ? target_sender : "");
}

char* kslm_get_stats_json(kslm_engine_t* engine) {
    if (!engine) return duplicate_string("{}");
    std::lock_guard<std::mutex> lock(engine->mtx);
    auto stats = engine->engine.get_stats();

    std::ostringstream ss;
    ss << "{"
       << "\"unique_words\":" << stats.unique_words << ","
       << "\"total_tokens\":" << stats.total_tokens_learned << ","
       << "\"bigram_contexts\":" << stats.bigram_contexts << ","
       << "\"trigram_contexts\":" << stats.trigram_contexts << ","
       << "\"bigram_entries\":" << stats.bigram_entries << ","
       << "\"trigram_entries\":" << stats.trigram_entries << ","
       << "\"memory_bytes\":" << stats.estimated_memory_bytes << ","
       << "\"memory_kb\":" << std::fixed << std::setprecision(1) << (stats.estimated_memory_bytes / 1024.0)
       << "}";
    return duplicate_string(ss.str());
}

int kslm_save(kslm_engine_t* engine, const char* filepath) {
    if (!engine || !filepath) return 0;
    std::lock_guard<std::mutex> lock(engine->mtx);
    return engine->engine.save_model(filepath) ? 1 : 0;
}

int kslm_load(kslm_engine_t* engine, const char* filepath) {
    if (!engine || !filepath) return 0;
    std::lock_guard<std::mutex> lock(engine->mtx);
    return engine->engine.load_model(filepath) ? 1 : 0;
}

void kslm_free_string(char* s) {
    std::free(s);
}

} // extern "C"
