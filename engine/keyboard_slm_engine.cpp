#include "keyboard_slm_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace keyboard_slm {

namespace {

constexpr size_t kMaxTokenCps = 32;
constexpr char kSep = '\x1f';
const std::string kBos(1, '\x02'); // sentence-start context marker (never a real token)

// ------------------------------------------------------------------------------------
// UTF-8 + character classification
// ------------------------------------------------------------------------------------

// Decode one code point starting at s[i], advancing i. Invalid bytes yield U+FFFD.
char32_t decode_utf8(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    int len = 0;
    char32_t cp = 0;
    char32_t min_cp = 0;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min_cp = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min_cp = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min_cp = 0x10000; }
    else { ++i; return 0xFFFD; }
    if (i + static_cast<size_t>(len) > s.size()) { ++i; return 0xFFFD; }
    for (int k = 1; k < len; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
        if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ++i; return 0xFFFD; }
    i += static_cast<size_t>(len);
    return cp;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

enum class Cp { Letter, Apostrophe, Digit, Skip, Other };

// Classify a code point. `out` receives the (lower-cased) letter to keep.
Cp classify(char32_t cp, char32_t& out) {
    out = cp;
    if (cp < 0x80) {
        if (cp >= 'a' && cp <= 'z') return Cp::Letter;
        if (cp >= 'A' && cp <= 'Z') { out = cp + 32; return Cp::Letter; }
        if (cp >= '0' && cp <= '9') return Cp::Digit;
        if (cp == '\'') return Cp::Apostrophe;
        return Cp::Other;
    }
    if (cp == 0x2019 || cp == 0x2018 || cp == 0x02BC) { out = '\''; return Cp::Apostrophe; }
    // Invisible / formatting characters: ignored without splitting a word.
    if (cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
        cp == 0x2060 || cp == 0xFEFF || (cp >= 0xFE00 && cp <= 0xFE0F)) {
        return Cp::Skip;
    }
    if (cp >= 0xC0 && cp <= 0xFF) { // Latin-1 letters
        if (cp == 0xD7 || cp == 0xF7) return Cp::Other;
        if (cp <= 0xDE) out = cp + 32;
        return Cp::Letter;
    }
    if (cp >= 0x100 && cp <= 0x24F) return Cp::Letter; // Latin extended
    if (cp >= 0x300 && cp <= 0x36F) return Cp::Skip;   // combining marks
    if (cp >= 0x400 && cp <= 0x4FF) {                  // Cyrillic
        if (cp >= 0x410 && cp <= 0x42F) out = cp + 32;
        else if (cp <= 0x40F) out = cp + 80;
        return Cp::Letter;
    }
    if (cp >= 0x600 && cp <= 0x6FF) {                  // Arabic / Urdu
        if ((cp >= 0x660 && cp <= 0x669) || (cp >= 0x6F0 && cp <= 0x6F9)) return Cp::Digit;
        if (cp == 0x60C || cp == 0x61B || cp == 0x61F || (cp >= 0x66A && cp <= 0x66D) || cp == 0x6D4)
            return Cp::Other;
        if ((cp >= 0x610 && cp <= 0x61A) || (cp >= 0x64B && cp <= 0x65F) || cp == 0x670) return Cp::Skip;
        return Cp::Letter;
    }
    if (cp >= 0x900 && cp <= 0xDFF) {                  // Devanagari .. Malayalam
        unsigned off = static_cast<unsigned>(cp & 0x7F);
        if (off >= 0x66 && off <= 0x6F) return Cp::Digit;
        if (cp == 0x964 || cp == 0x965) return Cp::Other; // danda
        return Cp::Letter;
    }
    return Cp::Other; // emoji, symbols, other scripts
}

bool is_space_cp(char32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0xA0 || cp == 0x202F ||
           cp == 0x2009 || cp == 0x3000;
}

bool is_clause_break_cp(char32_t cp) {
    return cp == '.' || cp == '!' || cp == '?' || cp == ',' || cp == ';' || cp == '\n' ||
           cp == 0x2026 || cp == 0x964 || cp == 0x965;
}

// Trim apostrophes, collapse 3+ repeats to 2, enforce length. Returns "" if unusable.
std::string finish_token(const std::vector<char32_t>& in) {
    size_t b = 0, e = in.size();
    while (b < e && in[b] == '\'') ++b;
    while (e > b && in[e - 1] == '\'') --e;
    std::vector<char32_t> out;
    out.reserve(e - b);
    for (size_t k = b; k < e; ++k) {
        size_t n = out.size();
        if (n >= 2 && out[n - 1] == in[k] && out[n - 2] == in[k]) continue;
        out.push_back(in[k]);
    }
    if (out.empty() || out.size() > kMaxTokenCps) return "";
    std::string s;
    for (char32_t c : out) append_utf8(s, c);
    return s;
}

bool has_digit(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        char32_t out;
        if (classify(decode_utf8(s, i), out) == Cp::Digit) return true;
    }
    return false;
}

std::string ascii_lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return r;
}

bool is_url_or_email(const std::string& raw) {
    if (raw.find('@') != std::string::npos) return true;
    std::string l = ascii_lower(raw);
    return l.find("http://") != std::string::npos || l.find("https://") != std::string::npos ||
           l.find("www.") != std::string::npos;
}

// Anything we must never learn from (digits = OTPs / numbers / passwords, URLs, e-mails).
bool is_sensitive_raw(const std::string& raw) { return is_url_or_email(raw) || has_digit(raw); }

// Replace sensitive whitespace-delimited tokens before clause splitting, so that
// "example.com/secret" cannot leak "com" / "secret" after being cut at the dots.
std::string sanitize_message(const std::string& msg) {
    std::string out;
    std::string tok;
    auto flush = [&](char delim) {
        if (!tok.empty()) {
            if (is_url_or_email(tok)) out.push_back('\n');
            else if (has_digit(tok)) out.push_back(' ');
            else { out += tok; out.push_back(' '); }
            tok.clear();
        }
        if (delim == '\n') out.push_back('\n');
    };
    for (size_t i = 0; i < msg.size();) {
        size_t start = i;
        char32_t cp = decode_utf8(msg, i);
        if (is_space_cp(cp)) flush(cp == '\n' ? '\n' : ' ');
        else tok.append(msg, start, i - start);
    }
    flush(' ');
    return out;
}

// ------------------------------------------------------------------------------------
// Chat export parsing
// ------------------------------------------------------------------------------------

struct ParsedMsg {
    std::string sender;
    std::string text;
    bool system = false;
};

std::string strip_invisible_edges(const std::string& s) {
    size_t b = 0;
    auto skippable = [](char32_t cp) {
        return cp == ' ' || cp == '\t' || cp == 0xA0 || cp == 0x202F || (cp >= 0x200B && cp <= 0x200F) ||
               (cp >= 0x202A && cp <= 0x202E) || cp == 0xFEFF;
    };
    while (b < s.size()) {
        size_t j = b;
        if (skippable(decode_utf8(s, j))) b = j; else break;
    }
    std::string r = s.substr(b);
    while (!r.empty()) { // trailing: only ASCII whitespace/CR (cheap and safe)
        unsigned char c = static_cast<unsigned char>(r.back());
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') r.pop_back(); else break;
    }
    return r;
}

bool is_digit_ch(char c) { return c >= '0' && c <= '9'; }

// Returns the index where "Sender: text" starts if `t` begins with a chat header, else npos.
size_t find_header_end(const std::string& t) {
    if (t.size() < 6) return std::string::npos;
    if (t[0] == '[') { // iOS: "[16/10/26, 11:30:12 PM] Sender: text"
        if (!is_digit_ch(t[1])) return std::string::npos;
        size_t close = t.find("] ");
        if (close == std::string::npos || close > 48) return std::string::npos;
        return close + 2;
    }
    if (!is_digit_ch(t[0])) return std::string::npos;
    size_t i = 0;
    auto digits = [&](size_t lo, size_t hi) {
        size_t n = 0;
        while (i < t.size() && is_digit_ch(t[i]) && n < hi) { ++i; ++n; }
        return n >= lo;
    };
    if (!digits(1, 4)) return std::string::npos;
    if (i >= t.size() || !(t[i] == '/' || t[i] == '.' || t[i] == '-')) return std::string::npos;
    char sep = t[i++];
    if (!digits(1, 2)) return std::string::npos;
    if (i >= t.size() || t[i] != sep) return std::string::npos;
    ++i;
    if (!digits(2, 4)) return std::string::npos;
    size_t dash = t.find(" - ", i); // Android: "16/10/2026, 11:30 pm - Sender: text"
    if (dash == std::string::npos || dash > 48) return std::string::npos;
    return dash + 3;
}

bool is_junk_message(const std::string& text) {
    std::string l = ascii_lower(strip_invisible_edges(text));
    if (l.empty() || l == "null") return true;
    if (l.find("<media omitted>") != std::string::npos || l.find("<attached:") != std::string::npos ||
        l.find("this message was deleted") != std::string::npos ||
        l.find("you deleted this message") != std::string::npos ||
        l.find("missed voice call") != std::string::npos ||
        l.find("missed video call") != std::string::npos ||
        l.find("waiting for this message") != std::string::npos ||
        l.find("(file attached)") != std::string::npos ||
        l.find("end-to-end encrypted") != std::string::npos) {
        return true;
    }
    // iOS placeholders: "image omitted", "document omitted", ...
    if (l.size() < 80 && (l.find(" omitted") != std::string::npos)) return true;
    return false;
}

std::vector<ParsedMsg> parse_chat(const std::string& content) {
    std::vector<ParsedMsg> msgs;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        std::string t = strip_invisible_edges(line);
        size_t body = find_header_end(t);
        if (body != std::string::npos) {
            ParsedMsg m;
            size_t colon = t.find(": ", body);
            if (colon == std::string::npos) {
                m.system = true; // e.g. "X created group", "Messages are end-to-end encrypted"
                m.text = t.substr(body);
            } else {
                m.sender = strip_invisible_edges(t.substr(body, colon - body));
                m.text = strip_invisible_edges(t.substr(colon + 2));
            }
            msgs.push_back(std::move(m));
        } else if (!msgs.empty() && !msgs.back().system && !t.empty()) {
            // continuation of a multi-line message (belongs to the previous sender)
            msgs.back().text += "\n";
            msgs.back().text += t;
        }
    }
    return msgs;
}

std::string canon_name(const std::string& s) { return ascii_lower(strip_invisible_edges(s)); }

// ------------------------------------------------------------------------------------
// Serialization helpers
// ------------------------------------------------------------------------------------

uint32_t crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct Writer {
    std::string buf;
    void varint(uint64_t v) {
        while (v >= 0x80) { buf.push_back(static_cast<char>((v & 0x7F) | 0x80)); v >>= 7; }
        buf.push_back(static_cast<char>(v));
    }
    void str(const std::string& s) { varint(s.size()); buf += s; }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool ok = true;
    Reader(const uint8_t* data, size_t size) : p(data), n(size) {}
    size_t remaining() const { return n - pos; }
    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            if (pos >= n) { ok = false; return 0; }
            uint8_t b = p[pos++];
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    std::string str() {
        uint64_t len = varint();
        if (!ok || len > 256 || len > remaining()) { ok = false; return ""; }
        std::string s(reinterpret_cast<const char*>(p + pos), static_cast<size_t>(len));
        pos += static_cast<size_t>(len);
        return s;
    }
};

void sat_add(uint32_t& a, uint32_t b) {
    uint64_t s = static_cast<uint64_t>(a) + b;
    a = s > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(s);
}

uint32_t tick32(uint64_t t) { return t > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(t); }

// Bounded Damerau-Levenshtein (adjacent transposition counts as one edit).
int edit_distance_bounded(std::string_view a, std::string_view b, int max_dist) {
    int n = static_cast<int>(a.size());
    int m = static_cast<int>(b.size());
    if (std::abs(n - m) > max_dist) return max_dist + 1;
    std::vector<int> prev2(static_cast<size_t>(m) + 1, 0), prev(static_cast<size_t>(m) + 1),
        curr(static_cast<size_t>(m) + 1);
    for (int j = 0; j <= m; ++j) prev[static_cast<size_t>(j)] = j;
    for (int i = 1; i <= n; ++i) {
        curr[0] = i;
        int row_min = curr[0];
        for (int j = 1; j <= m; ++j) {
            size_t sj = static_cast<size_t>(j);
            int cost = (a[static_cast<size_t>(i) - 1] == b[sj - 1]) ? 0 : 1;
            curr[sj] = std::min({prev[sj] + 1, curr[sj - 1] + 1, prev[sj - 1] + cost});
            if (i > 1 && j > 1 && a[static_cast<size_t>(i) - 1] == b[sj - 2] &&
                a[static_cast<size_t>(i) - 2] == b[sj - 1]) {
                curr[sj] = std::min(curr[sj], prev2[sj - 2] + 1);
            }
            row_min = std::min(row_min, curr[sj]);
        }
        if (row_min > max_dist) return max_dist + 1;
        prev2.swap(prev);
        prev.swap(curr);
    }
    return prev[static_cast<size_t>(m)];
}

} // namespace

// ====================================================================================
// Construction / text utilities
// ====================================================================================

PersonalSLM::PersonalSLM() = default;

std::string PersonalSLM::normalize_token(const std::string& raw) {
    std::vector<char32_t> cps;
    for (size_t i = 0; i < raw.size();) {
        char32_t out;
        Cp k = classify(decode_utf8(raw, i), out);
        if (k == Cp::Letter || k == Cp::Apostrophe) cps.push_back(out);
    }
    return finish_token(cps);
}

std::string PersonalSLM::phonetic_key(const std::string& word) {
    std::string w = normalize_token(word);
    if (w.empty()) return "";

    std::string step1;
    step1.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        char c = w[i];
        char next = (i + 1 < w.size()) ? w[i + 1] : '\0';

        // Vowel digraphs: ee -> i, oo -> u, aa -> a, ai -> e, ou -> o
        if (c == 'e' && next == 'e') { step1.push_back('i'); ++i; continue; }
        if (c == 'o' && next == 'o') { step1.push_back('u'); ++i; continue; }
        if (c == 'a' && next == 'a') { step1.push_back('a'); ++i; continue; }
        if (c == 'a' && next == 'i') { step1.push_back('e'); ++i; continue; }
        if (c == 'o' && next == 'u') { step1.push_back('o'); ++i; continue; }

        // Aspirated consonants: bh dh gh jh kh th -> plain; ph -> f ("ch", "sh" stay distinct)
        if (next == 'h' && (c == 'b' || c == 'd' || c == 'g' || c == 'j' || c == 'k' || c == 't')) {
            step1.push_back(c);
            ++i;
            continue;
        }
        if (c == 'p' && next == 'h') { step1.push_back('f'); ++i; continue; }

        if (c == 'w') c = 'v';
        else if (c == 'z') c = 'j';
        else if (c == 'q') c = 'k';
        step1.push_back(c);
    }

    // Collapse repeated ASCII letters ("avunnu" -> "avunu"); never touch multi-byte sequences.
    std::string result;
    result.reserve(step1.size());
    for (char c : step1) {
        if (!result.empty() && result.back() == c && static_cast<unsigned char>(c) < 0x80) continue;
        result.push_back(c);
    }
    return result;
}

std::vector<std::vector<std::string>> PersonalSLM::tokenize_message(const std::string& sentence) {
    std::string clean = sanitize_message(sentence);
    std::vector<std::vector<std::string>> clauses;
    std::vector<std::string> clause;
    std::vector<char32_t> run;

    auto flush_run = [&]() {
        if (run.empty()) return;
        std::string tok = finish_token(run);
        run.clear();
        if (!tok.empty()) clause.push_back(std::move(tok));
    };
    auto flush_clause = [&]() {
        flush_run();
        if (!clause.empty()) { clauses.push_back(std::move(clause)); clause.clear(); }
    };

    for (size_t i = 0; i < clean.size();) {
        char32_t out;
        char32_t cp = decode_utf8(clean, i);
        if (is_clause_break_cp(cp)) { flush_clause(); continue; }
        switch (classify(cp, out)) {
            case Cp::Letter:
            case Cp::Apostrophe: run.push_back(out); break;
            case Cp::Skip: break;
            default: flush_run(); break; // whitespace, symbols, emoji split words
        }
    }
    flush_clause();
    return clauses;
}

// ====================================================================================
// Learning
// ====================================================================================

void PersonalSLM::learn_word(const std::string& prev2, const std::string& prev1,
                             const std::string& word, uint32_t weight) {
    if (weight == 0 || is_sensitive_raw(word)) return;
    std::string w = normalize_token(word);
    if (w.empty()) return;

    global_tick_ += 1;
    total_tokens_ += weight;

    auto& stats = unigrams_[w];
    if (stats.count == 0) stats.phonetic = phonetic_key(w);
    sat_add(stats.count, weight);
    stats.last_seen_tick = global_tick_;

    // A sensitive previous word (digits / URL / e-mail) must leave no trace, not even as
    // context: treat it as a clause break.
    std::string p1 = is_sensitive_raw(prev1) ? "" : normalize_token(prev1);
    std::string p2 = is_sensitive_raw(prev2) ? "" : normalize_token(prev2);
    bool prev2_sensitive = is_sensitive_raw(prev2);
    if (p1.empty()) { p1 = kBos; p2.clear(); }
    else if (p2.empty()) { p2 = prev2_sensitive ? "" : kBos; }

    uint32_t tick = tick32(global_tick_);
    auto bump = [&](NGramTable& table, const std::string& ctx, size_t& entries) {
        auto& e = table[ctx][w];
        if (e.count == 0) ++entries;
        sat_add(e.count, weight);
        e.tick = tick;
    };
    bump(bigrams_, p1, bigram_entries_);
    if (!p2.empty() && p1 != kBos) bump(trigrams_, p2 + kSep + p1, trigram_entries_);

    enforce_limits();
}

void PersonalSLM::learn_sentence(const std::string& sentence, uint32_t weight) {
    for (const auto& clause : tokenize_message(sentence)) {
        std::string prev2, prev1;
        for (const auto& tok : clause) {
            learn_word(prev2, prev1, tok, weight);
            prev2 = prev1;
            prev1 = tok;
        }
    }
}

bool PersonalSLM::contains(const std::string& word) const {
    return unigrams_.count(normalize_token(word)) > 0;
}

void PersonalSLM::clear() {
    unigrams_.clear();
    bigrams_.clear();
    trigrams_.clear();
    global_tick_ = 0;
    total_tokens_ = 0;
    bigram_entries_ = trigram_entries_ = 0;
}

bool PersonalSLM::forget_word(const std::string& word) {
    std::string w = normalize_token(word);
    auto it = unigrams_.find(w);
    if (w.empty() || it == unigrams_.end()) return false;
    total_tokens_ -= std::min<uint64_t>(total_tokens_, it->second.count);
    unigrams_.erase(it);
    purge_words({{w, true}});
    return true;
}

// Remove every n-gram that mentions a removed word (as target or inside a context key).
void PersonalSLM::purge_words(const std::unordered_map<std::string, bool>& removed) {
    auto gone = [&](const std::string& s) { return removed.count(s) > 0; };
    for (auto it = bigrams_.begin(); it != bigrams_.end();) {
        if (gone(it->first)) { it = bigrams_.erase(it); continue; }
        for (auto t = it->second.begin(); t != it->second.end();)
            t = gone(t->first) ? it->second.erase(t) : std::next(t);
        it = it->second.empty() ? bigrams_.erase(it) : std::next(it);
    }
    for (auto it = trigrams_.begin(); it != trigrams_.end();) {
        size_t sep = it->first.find(kSep);
        bool kill = sep == std::string::npos || gone(it->first.substr(0, sep)) ||
                    gone(it->first.substr(sep + 1));
        if (kill) { it = trigrams_.erase(it); continue; }
        for (auto t = it->second.begin(); t != it->second.end();)
            t = gone(t->first) ? it->second.erase(t) : std::next(t);
        it = it->second.empty() ? trigrams_.erase(it) : std::next(it);
    }
    recount_entries();
}

void PersonalSLM::recount_entries() {
    bigram_entries_ = 0;
    for (const auto& c : bigrams_) bigram_entries_ += c.second.size();
    trigram_entries_ = 0;
    for (const auto& c : trigrams_) trigram_entries_ += c.second.size();
}

void PersonalSLM::enforce_limits() {
    if (unigrams_.size() > limits.max_vocab) prune_vocabulary();
    if (bigram_entries_ > limits.max_bigram_entries) prune_table(bigrams_, limits.max_bigram_entries, bigram_entries_);
    if (trigram_entries_ > limits.max_trigram_entries) prune_table(trigrams_, limits.max_trigram_entries, trigram_entries_);
    if (total_tokens_ >= limits.decay_after_tokens) apply_decay();
}

void PersonalSLM::prune_vocabulary() {
    size_t target = limits.max_vocab - limits.max_vocab / 10;
    if (unigrams_.size() <= target) return;
    struct Entry { const std::string* word; double utility; };
    std::vector<Entry> entries;
    entries.reserve(unigrams_.size());
    for (const auto& kv : unigrams_) {
        entries.push_back({&kv.first, static_cast<double>(kv.second.count) *
                                           compute_recency_boost(kv.second.last_seen_tick)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.utility != b.utility) return a.utility > b.utility;
        return *a.word < *b.word;
    });
    std::unordered_map<std::string, bool> removed;
    for (size_t i = target; i < entries.size(); ++i) removed.emplace(*entries[i].word, true);
    for (const auto& kv : removed) {
        auto it = unigrams_.find(kv.first);
        total_tokens_ -= std::min<uint64_t>(total_tokens_, it->second.count);
        unigrams_.erase(it);
    }
    purge_words(removed);
}

// Drop the weakest n-gram entries (lowest count, then oldest) until 90% of the cap remains.
void PersonalSLM::prune_table(NGramTable& table, size_t cap, size_t& entries) {
    if (entries <= cap) return;
    size_t target = cap - cap / 10;
    std::vector<uint64_t> keys;
    keys.reserve(entries);
    for (const auto& c : table)
        for (const auto& e : c.second)
            keys.push_back((static_cast<uint64_t>(e.second.count) << 32) | e.second.tick);
    if (keys.size() <= target) return;
    size_t remove_n = keys.size() - target;
    std::nth_element(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(remove_n), keys.end());
    uint64_t cutoff = keys[remove_n];

    auto sweep = [&](bool inclusive) {
        for (auto it = table.begin(); it != table.end();) {
            for (auto t = it->second.begin(); t != it->second.end();) {
                uint64_t k = (static_cast<uint64_t>(t->second.count) << 32) | t->second.tick;
                t = (inclusive ? k <= cutoff : k < cutoff) ? it->second.erase(t) : std::next(t);
            }
            it = it->second.empty() ? table.erase(it) : std::next(it);
        }
    };
    sweep(false);
    size_t n = 0;
    for (const auto& c : table) n += c.second.size();
    if (n > cap) { sweep(true); n = 0; for (const auto& c : table) n += c.second.size(); }
    entries = n;
}

// Halve every count so the model keeps adapting (and counters can never overflow).
void PersonalSLM::apply_decay() {
    const uint64_t recent_window = 20000;
    auto recent = [&](uint64_t tick) { return global_tick_ - std::min(global_tick_, tick) <= recent_window; };

    std::unordered_map<std::string, bool> removed;
    uint64_t total = 0;
    for (auto it = unigrams_.begin(); it != unigrams_.end();) {
        uint32_t c = it->second.count / 2;
        if (c == 0) {
            if (recent(it->second.last_seen_tick)) c = 1;
            else { removed.emplace(it->first, true); it = unigrams_.erase(it); continue; }
        }
        it->second.count = c;
        total += c;
        ++it;
    }
    total_tokens_ = total;

    auto decay_table = [&](NGramTable& table) {
        for (auto it = table.begin(); it != table.end();) {
            for (auto t = it->second.begin(); t != it->second.end();) {
                uint32_t c = t->second.count / 2;
                if (c == 0 && recent(t->second.tick)) c = 1;
                if (c == 0) { t = it->second.erase(t); continue; }
                t->second.count = c;
                ++t;
            }
            it = it->second.empty() ? table.erase(it) : std::next(it);
        }
    };
    decay_table(bigrams_);
    decay_table(trigrams_);
    purge_words(removed); // also recounts entries
}

// ====================================================================================
// Chat import
// ====================================================================================

std::vector<ChatSender> PersonalSLM::list_chat_senders(const std::string& file_content) {
    std::unordered_map<std::string, size_t> counts;
    for (const auto& m : parse_chat(file_content)) {
        if (m.system || is_junk_message(m.text)) continue;
        counts[m.sender]++;
    }
    std::vector<ChatSender> out;
    for (const auto& kv : counts) out.push_back({kv.first, kv.second});
    std::sort(out.begin(), out.end(), [](const ChatSender& a, const ChatSender& b) {
        if (a.message_count != b.message_count) return a.message_count > b.message_count;
        return a.name < b.name;
    });
    return out;
}

std::vector<std::string> PersonalSLM::extract_chat_messages(const std::string& file_content,
                                                            const std::string& target_sender) {
    auto msgs = parse_chat(file_content);
    std::string target = canon_name(target_sender);

    // Exact (case-insensitive) match first; fall back to substring only if nothing matched.
    bool exact_found = false;
    if (!target.empty()) {
        for (const auto& m : msgs)
            if (!m.system && canon_name(m.sender) == target) { exact_found = true; break; }
    }

    std::vector<std::string> out;
    for (const auto& m : msgs) {
        if (m.system || is_junk_message(m.text)) continue;
        if (!target.empty()) {
            std::string s = canon_name(m.sender);
            bool match = exact_found ? (s == target) : (s.find(target) != std::string::npos);
            if (!match) continue;
        }
        out.push_back(m.text);
    }
    return out;
}

size_t PersonalSLM::import_chat_txt(const std::string& file_content, const std::string& target_sender) {
    auto messages = extract_chat_messages(file_content, target_sender);
    for (const auto& m : messages) learn_sentence(m, 1);
    return messages.size();
}

// ====================================================================================
// Prediction
// ====================================================================================

double PersonalSLM::compute_recency_boost(uint64_t last_seen) const {
    if (global_tick_ <= last_seen) return 1.0 + tuning.rec_amp;
    double age = static_cast<double>(global_tick_ - last_seen);
    return 1.0 + tuning.rec_amp * std::exp(-age / tuning.rec_decay);
}

namespace {
enum Match : uint8_t { kAll, kPrefix, kExact, kPhoneticEq, kPhoneticPrefix, kFuzzy };
enum CtxHit : uint8_t { kNone, kBi, kTri };

std::string make_label(Match m, CtxHit c) {
    const char* base = "unigram";
    switch (m) {
        case kAll: base = c == kTri ? "trigram" : c == kBi ? "bigram" : "unigram"; return base;
        case kPrefix: base = "prefix"; break;
        case kExact: base = "exact"; break;
        case kPhoneticEq: base = "phonetic"; break;
        case kPhoneticPrefix: base = "phonetic-prefix"; break;
        case kFuzzy: base = "fuzzy"; break;
    }
    std::string s = base;
    if (c == kTri) s += "+trigram";
    else if (c == kBi) s += "+bigram";
    return s;
}
} // namespace

std::vector<Prediction> PersonalSLM::predict(const std::string& prev2, const std::string& prev1,
                                             const std::string& current_prefix, size_t top_k) const {
    std::vector<Prediction> result;
    if (top_k == 0 || unigrams_.empty()) return result;

    // Digits anywhere in the typed text (OTP, password, number): never suggest anything.
    if (has_digit(current_prefix)) return result;
    std::string pref = normalize_token(current_prefix);
    if (pref.empty() && !current_prefix.empty()) return result; // symbols only, or absurdly long

    std::string p2 = normalize_token(prev2);
    std::string p1 = normalize_token(prev1);
    if (p1.empty()) { p1 = kBos; p2.clear(); }
    else if (p2.empty()) { p2 = kBos; }
    std::string pref_ph = pref.empty() ? "" : phonetic_key(pref);

    const std::unordered_map<std::string, NGramEntry>* tri_map = nullptr;
    double tri_total = 0;
    if (!p2.empty() && p1 != kBos) {
        auto it = trigrams_.find(p2 + kSep + p1);
        if (it != trigrams_.end()) {
            tri_map = &it->second;
            for (const auto& kv : *tri_map) tri_total += kv.second.count;
        }
    }
    const std::unordered_map<std::string, NGramEntry>* bi_map = nullptr;
    double bi_total = 0;
    {
        auto it = bigrams_.find(p1);
        if (it != bigrams_.end()) {
            bi_map = &it->second;
            for (const auto& kv : *bi_map) bi_total += kv.second.count;
        }
    }
    const double uni_denom = static_cast<double>(std::max<uint64_t>(total_tokens_, 1));

    struct Cand {
        const std::string* word;
        const WordStats* stats;
        double score;
        Match match;
        CtxHit ctx;
    };
    std::vector<Cand> cands;
    cands.reserve(128);

    auto add = [&](const std::string& word, const WordStats& st, Match match, double mult) {
        double p_tri = 0.0, p_bi = 0.0;
        CtxHit hit = kNone;
        if (tri_map) {
            auto it = tri_map->find(word);
            if (it != tri_map->end()) {
                p_tri = it->second.count / (tri_total + tuning.b3);
                hit = kTri;
            }
        }
        if (bi_map) {
            auto it = bi_map->find(word);
            if (it != bi_map->end()) {
                p_bi = it->second.count / (bi_total + tuning.b2);
                if (hit == kNone) hit = kBi;
            }
        }
        double p_uni = static_cast<double>(st.count) / uni_denom;
        double score = (tuning.w3 * p_tri + tuning.w2 * p_bi + tuning.w1 * p_uni) *
                       compute_recency_boost(st.last_seen_tick) * mult;
        cands.push_back({&word, &st, score, match, hit});
    };

    // Rank candidates, merge spelling variants (same phonetic key), keep the best top_k.
    auto finalize = [&]() {
        auto better = [](const Cand& a, const Cand& b) {
            if (std::abs(a.score - b.score) > 1e-12) return a.score > b.score;
            return *a.word < *b.word;
        };
        auto pick = [&](size_t upto) {
            std::vector<Prediction> out;
            std::unordered_set<std::string> seen;
            for (size_t i = 0; i < upto && out.size() < top_k; ++i) {
                const Cand& c = cands[i];
                const std::string& key = c.stats->phonetic.empty() ? *c.word : c.stats->phonetic;
                if (!seen.insert(key).second) continue;
                out.push_back({*c.word, c.score, make_label(c.match, c.ctx)});
            }
            return out;
        };
        size_t window = std::min(cands.size(), top_k * 8 + 8);
        std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(window), cands.end(), better);
        auto out = pick(window);
        if (out.size() < top_k && window < cands.size()) {
            std::sort(cands.begin(), cands.end(), better);
            out = pick(cands.size());
        }
        return out;
    };

    if (pref.empty()) {
        for (const auto& kv : unigrams_) {
            if (kv.first == p1) continue; // don't repeat the word just typed
            add(kv.first, kv.second, kAll, 1.0);
        }
        return finalize();
    }

    // Pass 1: exact prefix, then phonetic matches.
    for (const auto& kv : unigrams_) {
        const std::string& word = kv.first;
        const WordStats& st = kv.second;
        if (word.size() >= pref.size() && word.compare(0, pref.size(), pref) == 0) {
            bool whole = word.size() == pref.size();
            add(word, st, whole ? kExact : kPrefix, whole ? tuning.m_exact_word : 1.0);
        } else if (!pref_ph.empty() && !st.phonetic.empty() && st.phonetic.size() >= pref_ph.size() &&
                   st.phonetic.compare(0, pref_ph.size(), pref_ph) == 0) {
            bool eq = st.phonetic.size() == pref_ph.size();
            add(word, st, eq ? kPhoneticEq : kPhoneticPrefix, eq ? tuning.m_phonetic_equal : tuning.m_phonetic_prefix);
        }
    }
    result = finalize();

    // Pass 2: typo-tolerant matches are only allowed to FILL empty slots, never to outrank
    // real prefix/phonetic matches.
    if (result.size() < top_k && pref.size() >= 3) {
        std::unordered_set<const std::string*> have;
        for (const auto& c : cands) have.insert(c.word);
        for (const auto& kv : unigrams_) {
            const std::string& word = kv.first;
            if (have.count(&word) || word.size() + 1 < pref.size()) continue;
            std::string_view slice(word.data(), std::min(word.size(), pref.size()));
            if (edit_distance_bounded(pref, slice, 1) <= 1) add(word, kv.second, kFuzzy, tuning.m_fuzzy);
        }
        result = finalize();
    }
    return result;
}

// ====================================================================================
// Persistence
// ====================================================================================

namespace {
constexpr uint32_t kMagic = 0x4D4C534Bu; // "KSLM" little-endian
constexpr uint32_t kVersion = 2;
constexpr size_t kHeaderSize = 16;
constexpr size_t kMaxFileSize = 128u * 1024u * 1024u;

void put_u32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
} // namespace

std::string PersonalSLM::serialize() const {
    Writer w;
    w.varint(global_tick_);
    w.varint(total_tokens_);

    w.varint(unigrams_.size());
    for (const auto& kv : unigrams_) {
        w.str(kv.first);
        w.varint(kv.second.count);
        w.varint(kv.second.last_seen_tick);
    }
    auto write_table = [&](const NGramTable& table) {
        w.varint(table.size());
        for (const auto& c : table) {
            w.str(c.first);
            w.varint(c.second.size());
            for (const auto& e : c.second) {
                w.str(e.first);
                w.varint(e.second.count);
                w.varint(e.second.tick);
            }
        }
    };
    write_table(bigrams_);
    write_table(trigrams_);

    std::string out;
    put_u32(out, kMagic);
    put_u32(out, kVersion);
    put_u32(out, static_cast<uint32_t>(w.buf.size()));
    put_u32(out, crc32(reinterpret_cast<const uint8_t*>(w.buf.data()), w.buf.size()));
    out += w.buf;
    return out;
}

bool PersonalSLM::deserialize(const std::string& data) {
    if (data.size() < kHeaderSize || data.size() > kMaxFileSize) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    if (get_u32(p) != kMagic || get_u32(p + 4) != kVersion) return false;
    size_t payload = get_u32(p + 8);
    if (payload != data.size() - kHeaderSize) return false;
    if (crc32(p + kHeaderSize, payload) != get_u32(p + 12)) return false;

    PersonalSLM tmp;
    tmp.tuning = tuning;
    tmp.limits = limits;
    Reader r(p + kHeaderSize, payload);
    tmp.global_tick_ = r.varint();
    tmp.total_tokens_ = r.varint();

    uint64_t n_uni = r.varint();
    if (!r.ok || n_uni > r.remaining() / 3) return false;
    for (uint64_t i = 0; i < n_uni; ++i) {
        std::string w = r.str();
        uint64_t count = r.varint();
        uint64_t tick = r.varint();
        if (!r.ok || w.empty() || count == 0 || count > 0xFFFFFFFFull) return false;
        WordStats st;
        st.count = static_cast<uint32_t>(count);
        st.last_seen_tick = tick;
        st.phonetic = phonetic_key(w);
        tmp.unigrams_[w] = std::move(st);
    }
    auto read_table = [&](NGramTable& table) {
        uint64_t n_ctx = r.varint();
        if (!r.ok || n_ctx > r.remaining() / 3) return false;
        for (uint64_t i = 0; i < n_ctx; ++i) {
            std::string ctx = r.str();
            uint64_t n = r.varint();
            if (!r.ok || ctx.empty() || n == 0 || n > r.remaining() / 3) return false;
            auto& inner = table[ctx];
            for (uint64_t j = 0; j < n; ++j) {
                std::string w = r.str();
                uint64_t count = r.varint();
                uint64_t tick = r.varint();
                if (!r.ok || w.empty() || count == 0 || count > 0xFFFFFFFFull || tick > 0xFFFFFFFFull) return false;
                inner[w] = NGramEntry{static_cast<uint32_t>(count), static_cast<uint32_t>(tick)};
            }
        }
        return true;
    };
    if (!read_table(tmp.bigrams_) || !read_table(tmp.trigrams_)) return false;
    if (!r.ok || r.pos != payload) return false;

    tmp.recount_entries();
    if (!tmp.validate()) return false;
    *this = std::move(tmp);
    return true;
}

bool PersonalSLM::save_model(const std::string& filepath) const {
    std::string data = serialize();
    std::string tmp_path = filepath + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out.good()) { out.close(); std::remove(tmp_path.c_str()); return false; }
    }
    if (std::rename(tmp_path.c_str(), filepath.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

bool PersonalSLM::load_model(const std::string& filepath) {
    std::ifstream in(filepath, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    return deserialize(ss.str());
}

// ====================================================================================
// Introspection
// ====================================================================================

bool PersonalSLM::validate() const {
    size_t bi = 0, tri = 0;
    for (const auto& c : bigrams_) {
        if (c.second.empty()) return false;
        for (const auto& e : c.second) {
            if (!unigrams_.count(e.first) || e.second.count == 0) return false;
            ++bi;
        }
    }
    for (const auto& c : trigrams_) {
        size_t sep = c.first.find(kSep);
        if (sep == std::string::npos) return false;
        std::string a = c.first.substr(0, sep), b = c.first.substr(sep + 1);
        if (a.empty() || b.empty() || c.second.empty()) return false;
        for (const auto& e : c.second) {
            if (!unigrams_.count(e.first) || e.second.count == 0) return false;
            ++tri;
        }
    }
    if (bi != bigram_entries_ || tri != trigram_entries_) return false;
    for (const auto& kv : unigrams_) {
        if (kv.second.count == 0 || kv.second.phonetic != phonetic_key(kv.first)) return false;
    }
    return true;
}

EngineStats PersonalSLM::get_stats() const {
    EngineStats s{};
    s.unique_words = unigrams_.size();
    s.total_tokens_learned = static_cast<size_t>(total_tokens_);
    s.bigram_contexts = bigrams_.size();
    s.trigram_contexts = trigrams_.size();
    s.bigram_entries = bigram_entries_;
    s.trigram_entries = trigram_entries_;

    // Rough estimate: payload + ~48 bytes of hash-node overhead per entry.
    size_t bytes = sizeof(*this);
    for (const auto& kv : unigrams_) bytes += kv.first.capacity() + kv.second.phonetic.capacity() + sizeof(WordStats) + 48;
    auto table_bytes = [](const NGramTable& t) {
        size_t b = 0;
        for (const auto& c : t) {
            b += c.first.capacity() + 96;
            for (const auto& e : c.second) b += e.first.capacity() + sizeof(NGramEntry) + 48;
        }
        return b;
    };
    bytes += table_bytes(bigrams_) + table_bytes(trigrams_);
    s.estimated_memory_bytes = bytes;
    return s;
}

} // namespace keyboard_slm
