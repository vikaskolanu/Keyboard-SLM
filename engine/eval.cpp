// Held-out evaluation: train on the first fraction of a user's messages (chronological),
// test on the rest (or on a second chat file). Reports top-1/top-3 hit rates for next-word
// and prefix-assisted prediction, in frozen and online-learning modes.
//
// Usage: ./eval chat.txt "Sender Name" [train_fraction] [test_chat.txt]
// Env:   KSLM_TUNE="w3,w2,w1,b3,b2,rec_amp,rec_decay,m_exact,m_phon_eq,m_phon_prefix,m_fuzzy"
#include "keyboard_slm_engine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace keyboard_slm;

struct Counter {
    size_t n = 0, top1 = 0, top3 = 0;
    void add(const std::vector<Prediction>& p, const std::string& target) {
        ++n;
        if (!p.empty() && p[0].word == target) ++top1;
        for (const auto& x : p) if (x.word == target) { ++top3; break; }
    }
    double r1() const { return n ? 100.0 * top1 / n : 0; }
    double r3() const { return n ? 100.0 * top3 / n : 0; }
};

static bool hit(const std::vector<Prediction>& p, const std::string& w) {
    for (const auto& x : p) if (x.word == w) return true;
    return false;
}

// Code-point boundaries of a UTF-8 string (so typing "k" characters never splits a letter).
static std::vector<size_t> boundaries(const std::string& w) {
    std::vector<size_t> b;
    for (size_t i = 1; i < w.size(); ++i)
        if ((static_cast<unsigned char>(w[i]) & 0xC0) != 0x80) b.push_back(i);
    return b;
}

static std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) { std::cerr << "cannot open " << path << "\n"; std::exit(1); }
    std::stringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

static void run(const char* label, PersonalSLM engine, const std::vector<std::string>& test_msgs, bool online) {
    Counter next, pre1, pre2;
    size_t total = 0, oov = 0, letters_total = 0, letters_saved = 0;
    for (const auto& msg : test_msgs) {
        for (const auto& tokens : PersonalSLM::tokenize_message(msg)) {
            std::string p2, p1;
            for (const auto& w : tokens) {
                ++total;
                if (!engine.contains(w)) ++oov;
                auto np = engine.predict(p2, p1, "", 3);
                next.add(np, w);
                size_t typed_bytes = 0;
                bool found = hit(np, w);
                if (!found) {
                    typed_bytes = w.size();
                    auto cuts = boundaries(w);
                    for (size_t idx = 0; idx < cuts.size(); ++idx) {
                        auto pp = engine.predict(p2, p1, w.substr(0, cuts[idx]), 3);
                        if (idx == 0 && cuts.size() >= 2) pre1.add(pp, w);
                        if (idx == 1 && cuts.size() >= 2) pre2.add(pp, w);
                        if (hit(pp, w)) { typed_bytes = cuts[idx]; break; }
                    }
                }
                letters_total += w.size();
                letters_saved += w.size() - typed_bytes;
                if (online) engine.learn_word(p2, p1, w, 1);
                p2 = p1;
                p1 = w;
            }
        }
    }
    std::cout << std::fixed << std::setprecision(1);
    std::cout << label << " (" << total << " test words, " << (total ? 100.0 * oov / total : 0)
              << "% never seen before)\n"
              << "  next-word      top1 " << next.r1() << "%  top3 " << next.r3() << "%\n"
              << "  after 1 char   top1 " << pre1.r1() << "%  top3 " << pre1.r3() << "%\n"
              << "  after 2 chars  top1 " << pre2.r1() << "%  top3 " << pre2.r3() << "%\n"
              << "  letters saved (type until the word is in the top 3): "
              << (letters_total ? 100.0 * letters_saved / letters_total : 0) << "%\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: eval chat.txt \"Sender\" [train_fraction] [test_chat.txt]\n";
        return 1;
    }
    auto msgs = PersonalSLM::extract_chat_messages(read_file(argv[1]), argv[2]);
    double frac = argc > 3 ? std::stod(argv[3]) : 0.8;
    size_t split = static_cast<size_t>(static_cast<double>(msgs.size()) * frac);
    std::vector<std::string> train(msgs.begin(), msgs.begin() + static_cast<std::ptrdiff_t>(split));
    std::vector<std::string> test(msgs.begin() + static_cast<std::ptrdiff_t>(split), msgs.end());
    if (argc > 4) { // cross-chat: train on ALL of file 1, test on file 2
        test = PersonalSLM::extract_chat_messages(read_file(argv[4]), argv[2]);
        train = msgs;
    }
    std::cout << msgs.size() << " messages: " << train.size() << " train / " << test.size() << " test\n";

    PersonalSLM engine;
    if (const char* tv = std::getenv("KSLM_TUNE")) {
        Tuning& t = engine.tuning;
        std::sscanf(tv, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf", &t.w3, &t.w2, &t.w1, &t.b3, &t.b2,
                    &t.rec_amp, &t.rec_decay, &t.m_exact_word, &t.m_phonetic_equal, &t.m_phonetic_prefix,
                    &t.m_fuzzy);
    }
    for (const auto& m : train) engine.learn_sentence(m);
    auto s = engine.get_stats();
    std::cout << "trained vocab " << s.unique_words << " words, ~" << s.estimated_memory_bytes / 1024
              << " KB\n\n";

    run("FROZEN model", engine, test, false);
    std::cout << "\n";
    run("ONLINE learning", engine, test, true);
    return 0;
}
