// Engine test suite. Build with sanitizers for the full robustness run:
//   clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra engine/keyboard_slm_engine.cpp engine/test_engine.cpp -o engine/test_engine
#include "keyboard_slm_engine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace keyboard_slm;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_fail;                                                                 \
            std::cerr << "  FAIL line " << __LINE__ << ": " << #cond << "\n";         \
        }                                                                             \
    } while (0)

static std::string top_words(const std::vector<Prediction>& p) {
    std::string s;
    for (const auto& x : p) s += (s.empty() ? "" : ",") + x.word;
    return s;
}
static bool in_top(const std::vector<Prediction>& p, const std::string& w) {
    for (const auto& x : p) if (x.word == w) return true;
    return false;
}

// Deterministic RNG for reproducible fuzz / synthetic data.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>(s >> 33); }
    double uni() { return next() / 2147483648.0; }
};

static std::string synth_word(int i) {
    static const char* cons[] = {"k", "g", "ch", "j", "t", "d", "n", "p", "b", "m", "y", "r", "l", "v", "s", "h"};
    static const char* vow[] = {"a", "i", "u", "e", "o", "aa", "ee"};
    std::string w;
    int x = i + 7;
    int syl = 2 + (i % 3);
    for (int k = 0; k < syl; ++k) {
        w += cons[x % 16]; x /= 3; x += i;
        w += vow[(x + k) % 7];
        x = x * 31 + 17;
        if (x < 0) x = -x;
    }
    // make every index map to a distinct letters-only word (base-26 suffix)
    int n = i + 1;
    w += "q";
    while (n > 0) { w.push_back(static_cast<char>('a' + n % 26)); n /= 26; }
    return w;
}

static const char* kSampleChat = R"(
16/10/2026, 09:10 pm - Messages and calls are end-to-end encrypted.
16/10/2026, 09:12 pm - Vikas: bro em chestunnav ipudu
16/10/2026, 09:13 pm - Rahul: nothing bro office lo unna
16/10/2026, 09:14 pm - Vikas: avunu bro repu kaluddama
16/10/2026, 09:15 pm - Vikas: em chestunnav ra inka
16/10/2026, 09:16 pm - Vikas: thinava leda inka
16/10/2026, 09:17 pm - Vikas: bro em chestunnav Weekend plan enti
16/10/2026, 09:18 pm - Vikas: bhai kya kar raha hai aaj
16/10/2026, 09:19 pm - Vikas: kya kar raha hai bro
16/10/2026, 09:20 pm - Vikas: mai abhi ghar ja raha hu
16/10/2026, 09:21 pm - Vikas: avunu ra nenu kuda ostunna
16/10/2026, 09:22 pm - Vikas: <Media omitted>
16/10/2026, 09:23 pm - Vikas: my otp is 849201 and link https://example.com/secret
16/10/2026, 09:24 pm - Vikas: call me after 10 mins bro
16/10/2026, 09:25 pm - Vikas: thinava bro
)";

static void test_phonetic() {
    std::cout << "[phonetic keys]\n";
    const char* pairs[][2] = {{"kya", "kyaaa"}, {"avunu", "avunnu"}, {"bhi", "bi"}, {"thinnava", "tinnava"},
                              {"waise", "vaise"}, {"meena", "mina"}, {"koora", "kura"}};
    for (auto& p : pairs) CHECK(PersonalSLM::phonetic_key(p[0]) == PersonalSLM::phonetic_key(p[1]));
    CHECK(PersonalSLM::phonetic_key("cat") != PersonalSLM::phonetic_key("dog"));
    CHECK(PersonalSLM::phonetic_key("") == "");
    // multi-byte letters must survive intact (no byte-level collapsing)
    std::string te = "\xE0\xA4\xA4\xE0\xA4\xA4"; // two Devanagari 'ta' (contains A4 A4 byte pairs)
    CHECK(PersonalSLM::phonetic_key(te) == te);
}

static void test_normalization() {
    std::cout << "[text normalization & privacy filters]\n";
    CHECK(PersonalSLM::normalize_token("Don\xE2\x80\x99t") == "don't");
    CHECK(PersonalSLM::normalize_token("HELLO!!") == "hello");
    CHECK(PersonalSLM::normalize_token("haaaiii") == "haaii");
    CHECK(PersonalSLM::normalize_token("\xE2\x80\x8E" "bro") == "bro");           // invisible LRM
    CHECK(PersonalSLM::normalize_token("'quoted'") == "quoted");
    CHECK(PersonalSLM::normalize_token("12345") == "");
    std::string long_word;
    for (int i = 0; i < 100; ++i) long_word += "ab";
    CHECK(PersonalSLM::normalize_token(long_word) == "");                          // absurdly long
    CHECK(PersonalSLM::normalize_token(std::string(200, 'a')) == "aa");            // key-hold collapsed
    CHECK(PersonalSLM::normalize_token("caf\xC3\x89") == "caf\xC3\xA9");           // Latin-1 lower-casing

    auto c1 = PersonalSLM::tokenize_message("ok/yes \xF0\x9F\x98\x82 bro-bro");
    CHECK(c1.size() == 1 && c1[0].size() == 4);                                    // emoji + / - split words
    auto c2 = PersonalSLM::tokenize_message("call 9876543210 now");
    CHECK(c2.size() == 1 && c2[0].size() == 2);
    auto c3 = PersonalSLM::tokenize_message("see https://a.com/secret now");
    CHECK(c3.size() == 2);                                                         // URL = clause break, nothing leaks
    for (const auto& c : c3) for (const auto& t : c) CHECK(t != "com" && t != "secret" && t != "a");
    auto c4 = PersonalSLM::tokenize_message("hi, how are you? fine. ok");
    CHECK(c4.size() == 4);
    auto c5 = PersonalSLM::tokenize_message("\xE0\xA4\xAE\xE0\xA5\x88\xE0\xA4\x82 \xE0\xA4\x98\xE0\xA4\xB0 \xE0\xA4\x9C\xE0\xA4\xBE");
    CHECK(c5.size() == 1 && c5[0].size() == 3);                                    // Devanagari words kept
    auto c6 = PersonalSLM::tokenize_message("mail me at a@b.com asap");
    for (const auto& c : c6) for (const auto& t : c) CHECK(t != "com" && t != "b");

    PersonalSLM e;
    e.learn_sentence("my pass is hunter42 and www.x.com ok");
    CHECK(!e.contains("hunter") && !e.contains("hunter42") && !e.contains("com"));
    {
        PersonalSLM z;
        z.learn_word("", "", "hunter42");
        z.learn_word("", "", "pass123");
        z.learn_word("", "pass123", "ok");          // sensitive previous word leaves no context trace
        z.learn_word("hunter42", "x7", "ok");
        auto zs = z.get_stats();
        CHECK(zs.unique_words == 1 && z.contains("ok") && !z.contains("pass") && !z.contains("hunter") && !z.contains("x"));
        CHECK(zs.bigram_contexts == 1 && zs.trigram_entries == 0 && z.validate()); // only the sentence-start context
    }
    e.learn_word("", "", "bro");
    CHECK(e.predict("", "", "849").empty());
    CHECK(e.predict("", "", "br0").empty());
    CHECK(e.predict("", "", "abc123").empty());
    CHECK(e.predict("", "", "(").empty());
    CHECK(e.validate());
}

static void test_chat_parsing() {
    std::cout << "[chat parsing]\n";
    std::string chat =
        "12/10/2026, 9:10 pm - Messages and calls are end-to-end encrypted.\n"
        "12/10/2026, 9:12 pm - Vikas: bro em chestunnav\n"
        "second line from vikas\n"
        "12/10/2026, 9:13 pm - Rahul: nothing bro\n"
        "rahul continuation must never be learned as Vikas\n"
        "12/10/2026, 9:14 pm - Vikas: <Media omitted>\n"
        "\xE2\x80\x8E[12/10/26, 9:15:00 PM] Rahul: \xE2\x80\x8Eimage omitted\r\n"
        "[12/10/26, 9:16:00 PM] Vikas: avunu bro\r\r\n"
        "12/10/2026, 9:17 pm - Vikas: You deleted this message\n"
        "12/10/2026, 9:18 pm - Rahul created group \"x\"\n"
        "ghost line after a system message\n";
    auto v = PersonalSLM::extract_chat_messages(chat, "Vikas");
    CHECK(v.size() == 2);
    if (v.size() == 2) {
        CHECK(v[0] == "bro em chestunnav\nsecond line from vikas");
        CHECK(v[1] == "avunu bro");
    }
    auto r = PersonalSLM::extract_chat_messages(chat, "rahul"); // case-insensitive
    CHECK(r.size() == 1 && r[0].find("rahul continuation") != std::string::npos);
    CHECK(PersonalSLM::extract_chat_messages(chat, "Vik").size() == 2);   // substring fallback
    CHECK(PersonalSLM::extract_chat_messages(chat, "Nobody").empty());
    CHECK(PersonalSLM::extract_chat_messages(chat, "").size() == 3);

    auto senders = PersonalSLM::list_chat_senders(chat);
    CHECK(senders.size() == 2 && senders[0].name == "Vikas" && senders[0].message_count == 2 &&
          senders[1].name == "Rahul" && senders[1].message_count == 1);

    PersonalSLM e;
    CHECK(e.import_chat_txt(chat, "Vikas") == 2);
    CHECK(e.contains("chestunnav") && e.contains("avunu") && !e.contains("nothing"));
    CHECK(PersonalSLM::extract_chat_messages("", "x").empty());
    CHECK(PersonalSLM::extract_chat_messages("garbage\n\n\x01\x02 no header at all", "x").empty());
}

static PersonalSLM sample_engine() {
    PersonalSLM e;
    e.import_chat_txt(kSampleChat, "Vikas");
    return e;
}

static void test_predictions() {
    std::cout << "[predictions]\n";
    PersonalSLM e = sample_engine();
    CHECK(!e.contains("849201") && !e.contains("secret") && !e.contains("example"));

    auto a = e.predict("bro", "em", "", 3);
    CHECK(!a.empty() && a[0].word == "chestunnav");
    auto b = e.predict("kya", "kar", "", 3);
    CHECK(!b.empty() && b[0].word == "raha");
    auto c = e.predict("kar", "raha", "", 3);
    CHECK(!c.empty() && c[0].word == "hai");
    auto d = e.predict("", "em", "ch", 3);
    CHECK(!d.empty() && d[0].word == "chestunnav");
    auto f = e.predict("", "", "tina", 3);
    CHECK(!f.empty() && f[0].word == "thinava");
    auto g = e.predict("", "", "kyaa", 3);              // spelling variant of "kya"
    CHECK(!g.empty() && g[0].word == "kya");

    // sentence start: most common opener first
    PersonalSLM s;
    s.learn_sentence("hi da");
    s.learn_sentence("hi da");
    s.learn_sentence("ok da");
    auto st = s.predict("", "", "", 3);
    CHECK(!st.empty() && st[0].word == "hi");

    // online learning: a phrase seen three times is predicted at once
    e.learn_sentence("enna panra macha");
    e.learn_sentence("enna panra bro");
    e.learn_sentence("ippo enna panra macha");
    auto t = e.predict("enna", "panra", "", 3);
    CHECK(!t.empty() && t[0].word == "macha");

    // never suggests the word just typed; results are unique and at most top_k
    auto u = e.predict("", "bro", "", 3);
    CHECK(!in_top(u, "bro") && u.size() <= 3);
    CHECK(e.predict("", "", "", 0).empty());
    CHECK(PersonalSLM().predict("a", "b", "c").empty());
    std::string long_pref;
    for (int i = 0; i < 250; ++i) long_pref += "ab";
    CHECK(e.predict("", "", long_pref).empty());
    CHECK(e.validate());
}

static void test_fuzzy_fill_only() {
    std::cout << "[typo matching only fills empty slots]\n";
    PersonalSLM e;
    for (int i = 0; i < 4; ++i) e.learn_sentence("avasaram");
    e.learn_sentence("avathala");
    e.learn_sentence("available");
    for (int i = 0; i < 20; ++i) e.learn_sentence("asala");        // popular fuzzy neighbour of "ava.."
    auto full = e.predict("", "", "ava", 3);
    CHECK(top_words(full) == "avasaram,avathala,available" || in_top(full, "avasaram"));
    for (const auto& p : full) CHECK(p.source.find("fuzzy") == std::string::npos);
    CHECK(!in_top(full, "asala"));

    auto typo = e.predict("", "", "avesa", 3);                      // no prefix match: fuzzy may fill
    CHECK(in_top(typo, "avasaram"));
    bool fuzzy_seen = false;
    for (const auto& p : typo) if (p.source.find("fuzzy") != std::string::npos) fuzzy_seen = true;
    CHECK(fuzzy_seen);
}

static void test_persistence() {
    std::cout << "[persistence]\n";
    PersonalSLM e = sample_engine();
    e.learn_sentence("enna panra macha");
    std::string blob = e.serialize();

    PersonalSLM r;
    CHECK(r.deserialize(blob));
    CHECK(r.validate());
    auto s1 = e.get_stats(), s2 = r.get_stats();
    CHECK(s1.unique_words == s2.unique_words && s1.bigram_entries == s2.bigram_entries &&
          s1.trigram_entries == s2.trigram_entries && s1.total_tokens_learned == s2.total_tokens_learned);
    const char* ctxs[][2] = {{"", ""}, {"", "bro"}, {"bro", "em"}, {"kya", "kar"}, {"kar", "raha"}, {"enna", "panra"}, {"", "avunu"}};
    const char* prefs[] = {"", "c", "ch", "ra", "kyaa", "tin", "x"};
    bool identical = true;
    for (auto& c : ctxs)
        for (auto p : prefs) {
            auto a = e.predict(c[0], c[1], p, 5), b = r.predict(c[0], c[1], p, 5);
            if (a.size() != b.size()) { identical = false; continue; }
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].word != b[i].word || a[i].score != b[i].score) identical = false;
        }
    CHECK(identical);                                   // reloaded model predicts exactly the same

    // corruption / truncation / garbage never load and never damage the live engine
    auto before = top_words(r.predict("bro", "em", "", 3));
    for (size_t pos : {size_t(0), size_t(5), size_t(9), size_t(13), size_t(16), blob.size() / 2, blob.size() - 1}) {
        std::string bad = blob;
        bad[pos] = static_cast<char>(bad[pos] ^ 0x5A);
        CHECK(!r.deserialize(bad));
    }
    CHECK(!r.deserialize(blob.substr(0, blob.size() - 1)));
    CHECK(!r.deserialize(blob.substr(0, 10)));
    CHECK(!r.deserialize(""));
    CHECK(!r.deserialize(blob + "x"));
    CHECK(!r.deserialize(std::string(64, '\xFF')));
    CHECK(top_words(r.predict("bro", "em", "", 3)) == before && r.validate());

    // file round trip, atomic write leaves no temp file, bad paths fail cleanly
    const std::string path = "/tmp/kslm_test_model.bin";
    CHECK(e.save_model(path));
    FILE* tmpf = std::fopen((path + ".tmp").c_str(), "rb");
    CHECK(tmpf == nullptr);
    if (tmpf) std::fclose(tmpf);
    PersonalSLM l;
    CHECK(l.load_model(path) && l.validate());
    CHECK(top_words(l.predict("bro", "em", "", 3)) == top_words(e.predict("bro", "em", "", 3)));
    CHECK(!l.load_model("/nonexistent_dir/none.bin"));
    CHECK(!e.save_model("/nonexistent_dir/none.bin"));
    std::remove(path.c_str());
}

static void test_forget_and_clear() {
    std::cout << "[forget / clear]\n";
    PersonalSLM e = sample_engine();
    CHECK(e.contains("bro"));
    CHECK(e.forget_word("Bro"));
    CHECK(!e.contains("bro") && !e.forget_word("bro") && e.validate());
    CHECK(!in_top(e.predict("", "em", "", 10), "bro"));
    CHECK(!in_top(e.predict("", "", "br", 10), "bro"));
    CHECK(e.forget_word("em") && e.validate());
    e.clear();
    CHECK(e.get_stats().unique_words == 0 && e.validate());
    CHECK(e.predict("", "", "a").empty());
}

static void test_bounds_and_decay() {
    std::cout << "[bounded memory + count decay]\n";
    PersonalSLM e;
    e.limits.max_vocab = 300;
    e.limits.max_bigram_entries = 2000;
    e.limits.max_trigram_entries = 3000;
    e.limits.decay_after_tokens = 20000;
    Rng rng(42);
    std::string prev;
    size_t max_vocab_seen = 0, max_bi = 0, max_tri = 0;
    for (int i = 0; i < 80000; ++i) {
        int rank = static_cast<int>(std::pow(2500.0, rng.uni())) - 1;
        std::string w = synth_word(rank);
        e.learn_word("", prev, w);
        if (rng.next() % 12 == 0) prev.clear(); else prev = w;
        if (i % 500 == 0) {
            auto s = e.get_stats();
            max_vocab_seen = std::max(max_vocab_seen, s.unique_words);
            max_bi = std::max(max_bi, s.bigram_entries);
            max_tri = std::max(max_tri, s.trigram_entries);
        }
    }
    auto s = e.get_stats();
    std::cout << "  vocab " << s.unique_words << ", bigrams " << s.bigram_entries << ", trigrams "
              << s.trigram_entries << ", tokens " << s.total_tokens_learned << ", ~"
              << s.estimated_memory_bytes / 1024 << " KB\n";
    CHECK(max_vocab_seen <= 300 && max_bi <= 2000 && max_tri <= 3000);
    CHECK(s.total_tokens_learned < 40000);   // decay kept counters bounded
    CHECK(e.validate());
    CHECK(!e.predict("", "", "k", 3).empty());
    CHECK(PersonalSLM().deserialize(e.serialize()));

    // decay must keep recent words usable
    PersonalSLM d;
    d.limits.decay_after_tokens = 50;
    for (int i = 0; i < 40; ++i) d.learn_sentence("haha ok da");
    d.learn_sentence("freshword");
    CHECK(d.contains("freshword") && d.validate());
}

static void test_fuzz() {
    std::cout << "[fuzz: random bytes must never crash or corrupt state]\n";
    PersonalSLM e = sample_engine();
    Rng rng(7);
    const std::string kAlphabet = "abcdefghij \n.,!?'\xE0\xA4\x95";
    auto rand_str = [&](size_t max_len) {
        std::string s(rng.next() % (max_len + 1), '\0');
        for (auto& c : s) {
            uint32_t r = rng.next();
            c = (r % 4 == 0) ? static_cast<char>(r >> 8) : kAlphabet[r % kAlphabet.size()];
        }
        return s;
    };
    for (int i = 0; i < 3000; ++i) {
        e.learn_sentence(rand_str(120));
        e.learn_word(rand_str(8), rand_str(8), rand_str(8), rng.next() % 3);
        (void)e.predict(rand_str(8), rand_str(8), rand_str(6), rng.next() % 5);
        (void)PersonalSLM::extract_chat_messages(rand_str(200), rand_str(5));
        (void)PersonalSLM::list_chat_senders(rand_str(200));
        if (i % 7 == 0) e.forget_word(rand_str(5));
    }
    CHECK(e.validate());

    std::string blob = e.serialize();
    PersonalSLM t;
    for (int i = 0; i < 2000; ++i) {
        std::string bad = blob;
        for (int k = 0; k < 1 + i % 4; ++k) bad[rng.next() % bad.size()] = static_cast<char>(rng.next());
        if (i % 5 == 0) bad.resize(rng.next() % bad.size());
        t.deserialize(bad);          // almost always rejected by CRC; must never crash
        t.deserialize(rand_str(64));
    }
    CHECK(t.validate());
}

static void test_determinism() {
    std::cout << "[determinism]\n";
    PersonalSLM a = sample_engine(), b = sample_engine();
    CHECK(a.serialize().size() == b.serialize().size());
    auto pa = a.predict("bro", "em", "c", 3), pb = b.predict("bro", "em", "c", 3);
    CHECK(top_words(pa) == top_words(pb));
}

static void test_latency() {
    std::cout << "[latency on a full-size model]\n";
    PersonalSLM e; // default limits: 5000 words
    Rng rng(99);
    std::string prev;
    for (int i = 0; i < 250000; ++i) {
        int rank = static_cast<int>(std::pow(9000.0, rng.uni())) - 1;
        std::string w = synth_word(rank);
        e.learn_word("", prev, w);
        if (rng.next() % 10 == 0) prev.clear(); else prev = w;
    }
    auto s = e.get_stats();
    std::cout << "  model: " << s.unique_words << " words, " << s.bigram_entries << " bigrams, "
              << s.trigram_entries << " trigrams, ~" << s.estimated_memory_bytes / 1024 << " KB\n";
    CHECK(s.estimated_memory_bytes < 40u * 1024u * 1024u);

    std::vector<double> times;
    for (int i = 0; i < 3000; ++i) {
        std::string p1 = synth_word(static_cast<int>(rng.next() % 500));
        std::string p2 = synth_word(static_cast<int>(rng.next() % 500));
        std::string w = synth_word(static_cast<int>(rng.next() % 3000));
        std::string pref = w.substr(0, rng.next() % 4);
        if (i % 10 == 0) pref = "zzq" + pref; // forces the typo-matching pass
        auto t0 = std::chrono::high_resolution_clock::now();
        auto r = e.predict(p2, p1, pref, 3);
        auto t1 = std::chrono::high_resolution_clock::now();
        times.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        (void)r;
    }
    std::sort(times.begin(), times.end());
    double avg = 0;
    for (double t : times) avg += t;
    avg /= times.size();
    std::cout << std::fixed << std::setprecision(1) << "  predict(): avg " << avg << " us, p99 "
              << times[times.size() * 99 / 100] << " us, max " << times.back() << " us\n";
    CHECK(times[times.size() * 99 / 100] < 20000); // budget: well under a frame (generous, sanitizer-safe)
}

int main() {
    test_phonetic();
    test_normalization();
    test_chat_parsing();
    test_predictions();
    test_fuzzy_fill_only();
    test_persistence();
    test_forget_and_clear();
    test_bounds_and_decay();
    test_fuzz();
    test_determinism();
    test_latency();
    std::cout << "\n" << g_checks << " checks, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
