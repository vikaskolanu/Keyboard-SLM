#ifndef KEYBOARD_SLM_ENGINE_H
#define KEYBOARD_SLM_ENGINE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace keyboard_slm {

struct Prediction {
    std::string word;
    double score;
    std::string source; // e.g. "bigram", "prefix+trigram", "phonetic", "fuzzy"
};

struct EngineStats {
    size_t unique_words;
    size_t total_tokens_learned;
    size_t bigram_contexts;
    size_t trigram_contexts;
    size_t bigram_entries;
    size_t trigram_entries;
    size_t estimated_memory_bytes;
};

struct ChatSender {
    std::string name;
    size_t message_count;
};

// Scoring knobs (all tunable from the evaluation harness).
struct Tuning {
    double w3 = 0.5, w2 = 0.3, w1 = 0.2;      // trigram / bigram / unigram weights
    double b3 = 1.0, b2 = 2.0;                // additive smoothing (confidence) for n-grams
    double rec_amp = 1.5, rec_decay = 500.0;  // recency boost: 1 + amp * exp(-age / decay)
    double m_exact_word = 1.25;               // typed prefix equals a whole known word
    double m_phonetic_equal = 0.6;            // typed text sounds identical to a known word
    double m_phonetic_prefix = 0.2;           // typed text sounds like the start of a known word
    double m_fuzzy = 0.1;                     // 1-edit typo match (only used to fill empty slots)
};

// Hard bounds that keep memory predictable on a phone.
struct Limits {
    size_t max_vocab = 5000;
    size_t max_bigram_entries = 50000;
    size_t max_trigram_entries = 70000;
    uint64_t decay_after_tokens = 100000;     // halve all counts after this many tokens
};

// Language-agnostic personal next-word / word-completion model.
//
// Context convention (same for learning and predicting): `prev1` / `prev2` are the one and
// two previous words *within the current sentence*. Empty `prev1` means "start of sentence".
//
// Not thread-safe: callers must serialize access (the C API layer will do this).
class PersonalSLM {
public:
    Tuning tuning;
    Limits limits;

    PersonalSLM();

    // ---- Learning --------------------------------------------------------------------
    void learn_sentence(const std::string& sentence, uint32_t weight = 1);
    void learn_word(const std::string& prev2, const std::string& prev1,
                    const std::string& word, uint32_t weight = 1);
    // Remove a word everywhere (vocabulary + every n-gram that mentions it).
    bool forget_word(const std::string& word);
    bool contains(const std::string& word) const;
    void clear();

    // ---- Chat export import ----------------------------------------------------------
    // Senders found in a WhatsApp export (Android or iOS format), most active first.
    static std::vector<ChatSender> list_chat_senders(const std::string& file_content);
    // Message bodies from `target_sender` (all senders if empty). System lines, media
    // placeholders and deleted-message notices are skipped; multi-line messages are joined.
    static std::vector<std::string> extract_chat_messages(const std::string& file_content,
                                                          const std::string& target_sender = "");
    size_t import_chat_txt(const std::string& file_content, const std::string& target_sender = "");

    // ---- Text utilities --------------------------------------------------------------
    // Split text into clauses of normalized tokens (URLs, e-mails and digit tokens removed).
    static std::vector<std::vector<std::string>> tokenize_message(const std::string& sentence);
    // Lower-cased letters/apostrophes only (empty if the word is unusable).
    static std::string normalize_token(const std::string& raw);
    // Language-agnostic phonetic signature for romanized words.
    static std::string phonetic_key(const std::string& word);

    // ---- Prediction ------------------------------------------------------------------
    std::vector<Prediction> predict(const std::string& prev2, const std::string& prev1,
                                    const std::string& current_prefix, size_t top_k = 3) const;

    // ---- Persistence -----------------------------------------------------------------
    // Versioned, checksummed binary format. Loading never modifies the engine on failure.
    std::string serialize() const;
    bool deserialize(const std::string& data);
    bool save_model(const std::string& filepath) const;  // atomic (temp file + rename)
    bool load_model(const std::string& filepath);

    EngineStats get_stats() const;
    // Internal consistency check (used by tests): every n-gram refers to known words, etc.
    bool validate() const;

private:
    struct WordStats {
        uint32_t count = 0;
        uint64_t last_seen_tick = 0;
        std::string phonetic;
    };
    struct NGramEntry {
        uint32_t count = 0;
        uint32_t tick = 0;
    };
    using NGramTable = std::unordered_map<std::string, std::unordered_map<std::string, NGramEntry>>;

    uint64_t global_tick_ = 0;
    uint64_t total_tokens_ = 0;
    size_t bigram_entries_ = 0;
    size_t trigram_entries_ = 0;

    std::unordered_map<std::string, WordStats> unigrams_;
    NGramTable bigrams_;   // prev1 -> word
    NGramTable trigrams_;  // prev2 \x1f prev1 -> word

    double compute_recency_boost(uint64_t last_seen) const;
    void enforce_limits();
    void prune_vocabulary();
    void prune_table(NGramTable& table, size_t cap, size_t& entries);
    void purge_words(const std::unordered_map<std::string, bool>& removed);
    void apply_decay();
    void recount_entries();
};

} // namespace keyboard_slm

#endif // KEYBOARD_SLM_ENGINE_H
