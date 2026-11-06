// Interactive sandbox for the PersonalSLM engine.
// Usage: ./cli [model.bin]
// Commands:
//   :senders <chat.txt>            list who is in a WhatsApp export
//   :import <chat.txt> [sender]    learn from a chat export (only that sender's messages)
//   :forget <word>                 remove a word from the model
//   :stats  :save  :quit
// Anything else is a line of text. Predictions are shown for the last word as a prefix
// (line doesn't end in a space) or as the next word (line ends in a space).
// The line is learned when you press Enter.
#include "keyboard_slm_engine.h"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace keyboard_slm;

static std::vector<std::string> split(const std::string& s) {
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string t;
    while (iss >> t) out.push_back(t);
    return out;
}

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream buf;
    buf << f.rdbuf();
    out = buf.str();
    return true;
}

int main(int argc, char** argv) {
    PersonalSLM engine;
    std::string path = argc > 1 ? argv[1] : "personal_model.bin";
    if (engine.load_model(path)) std::cout << "Loaded model from " << path << "\n";

    std::cout << "Type text. Enter learns it. Trailing space = next-word mode. :quit to exit.\n";
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        if (line == ":quit") break;
        if (line == ":save") { std::cout << (engine.save_model(path) ? "saved\n" : "save failed\n"); continue; }
        if (line == ":stats") {
            auto s = engine.get_stats();
            std::cout << s.unique_words << " words, " << s.total_tokens_learned << " tokens, "
                      << s.bigram_entries << " bigrams, " << s.trigram_entries << " trigrams, ~"
                      << s.estimated_memory_bytes / 1024 << " KB\n";
            continue;
        }
        if (line.rfind(":forget ", 0) == 0) {
            std::cout << (engine.forget_word(line.substr(8)) ? "forgotten\n" : "not found\n");
            continue;
        }
        if (line.rfind(":senders ", 0) == 0) {
            std::string content;
            if (!read_file(line.substr(9), content)) { std::cout << "cannot open file\n"; continue; }
            for (const auto& s : PersonalSLM::list_chat_senders(content))
                std::cout << "  " << s.message_count << "\t" << s.name << "\n";
            continue;
        }
        if (line.rfind(":import ", 0) == 0) {
            std::string rest = line.substr(8);
            std::string file = rest, sender;
            size_t sp = rest.find(' ');
            if (sp != std::string::npos) { file = rest.substr(0, sp); sender = rest.substr(sp + 1); }
            std::string content;
            if (!read_file(file, content)) { std::cout << "cannot open file\n"; continue; }
            std::cout << "learned " << engine.import_chat_txt(content, sender) << " messages\n";
            continue;
        }

        auto words = split(line);
        bool next_word_mode = !line.empty() && line.back() == ' ';
        std::string prefix = (!next_word_mode && !words.empty()) ? words.back() : "";
        size_t ctx_end = words.size() - ((!next_word_mode && !words.empty()) ? 1 : 0);
        std::string p1 = ctx_end >= 1 ? words[ctx_end - 1] : "";
        std::string p2 = ctx_end >= 2 ? words[ctx_end - 2] : "";
        for (const auto& p : engine.predict(p2, p1, prefix, 3))
            std::cout << "  " << p.word << "  [" << p.source << "]\n";

        engine.learn_sentence(line);
    }
    engine.save_model(path);
    return 0;
}
