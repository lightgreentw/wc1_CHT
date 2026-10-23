// wc1cht_trans.inc — 翻譯表：載入、查表、模板比對、詞彙補強
//
// 由 wc1cht.cpp 在翻譯相關函式的位置 #include。本檔不依賴 Windows API，
// 需要主程式先提供：wclog()、wclog_err()、str_replace()、unescape_field()、escape_field()。
//
// 翻譯檔有兩種格式，載入時自動判斷：
//
// 【純文字格式】（預設）每行「英文<TAB>中文」，# 開頭為註解。載入時在記憶體裡把英文換成雜湊，
//   之後的查表一律用雜湊，不再保留英文。
//
// 【雜湊格式】檔案開頭有一行 "#wc1cht:hash=fnv1a64"，之後每行：
//     <16 位十六進位雜湊><TAB>中文[<TAB>附註]            一般條目（雜湊 = 英文原文的 FNV-1a 64 位元）
//     @<片段>+<片段>+…<TAB>中文<TAB>p=<優先序>           含變數的模板條目
//   片段：固定文字寫成「位元組數.雜湊」（例 12.0123456789abcdef），變數寫成「$X」。
//   附註（一般條目，逗號分隔）：
//     c           這條是「不分大小寫」比對的代表，不分大小寫的雜湊與前面的雜湊相同
//     c=<雜湊>    同上，但不分大小寫（小寫化後）的雜湊是這個值
//     t=<位元組>  這條也是詞彙補強用的詞彙，原文長度為這麼多位元組（需同時有 c）
//   檔案不含任何英文原文。以 tools/wc1cht_hash_tool.py 由純文字格式轉出。
//
// 【除錯用的原文對照】雜湊格式的檔案沒有英文，查表本身也不需要英文。要在 log 看到「這個雜湊對應哪一句原文」時，
//   在 wc1cht.ini 的 [Log] 設 Enable=1、ShowSource=1、SourceFile=（含英文的純文字翻譯檔），
//   載入時會另外建一份「雜湊 → 英文」的對照，只用來寫 log（命中記錄、模板名稱、未命中清單），不影響查表結果。
//
// 雜湊與正規化規則（改動任何一項，所有雜湊都會失效，要同時改 tools/wc1cht_hash_tool.py 與檔頭版本）：
//   - 雜湊 = FNV-1a 64 位元，對還原跳脫字元（\n \t \\）後的原文位元組計算
//   - 不分大小寫的雜湊 = 對「ASCII 英文字母轉小寫」後的位元組計算

#ifndef WC1CHT_TRANS_INC
#define WC1CHT_TRANS_INC

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

// ── 雜湊 ──
static inline uint64_t wct_hash(const char* p, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; i++) { h ^= (uint64_t)(unsigned char)p[i]; h *= 1099511628211ULL; }
    return h;
}
static inline uint64_t wct_hash(const std::string& s) { return wct_hash(s.data(), s.size()); }
static std::string wct_lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}
static bool wct_parse_hex(const char* s, size_t n, uint64_t& out) {
    if (n != 16) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < 16; i++) {
        char c = s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | (uint64_t)d;
    }
    out = v;
    return true;
}

static int letter_count_simple(const std::string& s) { int n = 0; for (char c : s) if (isalpha((unsigned char)c)) n++; return n; }

// ── 翻譯表 ──
// 一般條目：雜湊 → 中文
static std::unordered_map<uint64_t, std::string> g_trans_map;   // 原文的雜湊
static std::unordered_map<uint64_t, std::string> g_trans_ci;    // 小寫化原文的雜湊（不分大小寫比對）
// 詞彙（名稱、短詞）：用於
//   1. 模板變數（$S、$R…）抓到的英文、說話者名稱，大小寫不同也要翻得到（遊戲的呼號是全大寫 HUNTER，表裡可能是 Hunter）
//   2. 整句沒翻譯時的「詞彙替換」："Talk to HUNTER." → "Talk to 獵人."
struct TermInfo { uint32_t len; std::string zh; };
static std::unordered_map<uint64_t, TermInfo> g_terms;          // 小寫化原文的雜湊
static std::vector<uint32_t> g_term_lens;                       // 詞彙的位元組長度（不重複，由長到短）

// ── 含「執行期變數」的翻譯條目（模板比對）──
//
// $C（呼叫代號）和 $N（姓氏）遊戲會先展開成玩家名字再交給 hook，
// 我們用玩家資料把名字換回 $C/$N，就能精確比對（見 make_template_key）。
// 但 $T（時間，如 06:00）、$D（日期，如 2654.110）這類變數，值是遊戲算出來的，
// hook 只會看到展開後的結果，無法還原成 "$T hours,$D."。
// 所以這種條目不放進精確比對表，改成「模板」：把英文拆成「固定文字 + 變數」，
// 看到文字時反過來比對，抓出變數的值，再填進中文翻譯裡同名的 $T/$D。
// 模板裡的固定文字只保留「位元組數 + 雜湊」，比對時對文字的同一段算雜湊來比。
//
//   $T = 時間  d{1,2}:dd        $D = 日期  dddd.ddd
//   其他 $X（大寫字母，C/N 除外）= 萬用，抓到下一段固定文字之前的任意內容
//
// 變數兩側的空白不要求一致（原始資料常寫成 "hours,$D." 但展開後會變成 "hours, 2654.110."）。

struct PatSeg { bool is_var = false; char var = 0; uint32_t len = 0; uint64_t hash = 0; };
struct TransPattern { std::vector<PatSeg> segs; std::string zh; std::string key; int prio = 0; };
static std::vector<TransPattern> g_patterns;
static std::unordered_map<std::string, int> g_pat_cache;     // 比對結果快取（HUD 每幀重畫同樣的字）

// ── 除錯用：原文對照（雜湊 → 英文），只在 ini 的 [Log] ShowSource=1 時載入 ──
static bool g_wct_show_source = false;                          // 由主程式依 ini 設定（需同時開 log）
static std::string g_wct_source_path;                           // 含英文的純文字翻譯檔完整路徑
static std::unordered_map<uint64_t, std::string> g_src_text;    // 原文的雜湊 → 英文原文
static std::unordered_map<std::string, std::string> g_src_pat;  // 模板鍵（@片段+片段…）→ 英文模板
static int g_wct_last_pat = -1;                                 // 最近一次 match_patterns 命中的模板編號（-1 = 沒命中）

static const char* wct_source_of(uint64_t h) {
    auto it = g_src_text.find(h);
    return it == g_src_text.end() ? "" : it->second.c_str();
}

// 一般條目查表（精確）。hash_out 非空時回傳命中的雜湊（除錯用）
static bool trans_exact_find(const std::string& key, std::string& out, uint64_t* hash_out = nullptr) {
    uint64_t h = wct_hash(key);
    auto it = g_trans_map.find(h);
    if (it == g_trans_map.end()) return false;
    out = it->second;
    if (hash_out) *hash_out = h;
    return true;
}

// 詞彙表查詢：先精確、再不分大小寫。用在「模板變數抓到的英文」與「說話者名稱」
static bool glossary_lookup(const std::string& key, std::string& out) {
    if (trans_exact_find(key, out)) return true;
    auto ci = g_trans_ci.find(wct_hash(wct_lower(key)));
    if (ci != g_trans_ci.end()) { out = ci->second; return true; }
    return false;
}

static bool is_pattern_var(char c) { return c >= 'A' && c <= 'Z' && c != 'C' && c != 'N'; }

// 模板的鍵（雜湊格式檔案裡第 1 欄的寫法）：@片段+片段…，固定文字「位元組數.雜湊」、變數「$X」
static std::string wct_pattern_key(const std::vector<PatSeg>& segs) {
    std::string k = "@";
    for (size_t i = 0; i < segs.size(); i++) {
        if (i) k += "+";
        if (segs[i].is_var) { k += "$"; k += segs[i].var; }
        else {
            char b[40]; snprintf(b, sizeof(b), "%u.%016llx", (unsigned)segs[i].len, (unsigned long long)segs[i].hash);
            k += b;
        }
    }
    return k;
}

// 由英文原文（純文字格式）建模板：拆成固定文字與變數，固定文字轉成「位元組數 + 雜湊」
static bool build_pattern(const std::string& key, const std::string& zh, TransPattern& P) {
    struct Piece { bool is_var = false; char var = 0; std::string lit; };
    std::vector<Piece> segs;
    std::string lit;
    bool has_var = false;
    for (size_t i = 0; i < key.size(); i++) {
        if (key[i] == '$' && i + 1 < key.size() && is_pattern_var(key[i + 1])) {
            if (!lit.empty()) { Piece g; g.lit = lit; segs.push_back(g); lit.clear(); }
            Piece v; v.is_var = true; v.var = key[i + 1]; segs.push_back(v);
            has_var = true; i++;
        } else lit += key[i];
    }
    if (!lit.empty()) { Piece g; g.lit = lit; segs.push_back(g); }
    if (!has_var) return false;

    // 緊貼著變數那一側的空白由比對器處理，固定文字裡先去掉
    for (size_t i = 0; i < segs.size(); i++) {
        if (segs[i].is_var) continue;
        std::string& L = segs[i].lit;
        if (i > 0 && segs[i - 1].is_var)
            while (!L.empty() && (L[0] == ' ' || L[0] == '\t')) L.erase(0, 1);
        if (i + 1 < segs.size() && segs[i + 1].is_var)
            while (!L.empty() && (L.back() == ' ' || L.back() == '\t')) L.pop_back();
    }
    P.segs.clear();
    for (auto& g : segs) {
        if (!g.is_var && g.lit.empty()) continue;
        PatSeg s;
        s.is_var = g.is_var; s.var = g.var;
        if (!g.is_var) { s.len = (uint32_t)g.lit.size(); s.hash = wct_hash(g.lit); }
        P.segs.push_back(s);
    }
    P.zh = zh; P.key = key;
    return true;
}

typedef std::vector<std::pair<char, std::string>> PatCaps;

static void pat_skip_ws(const char* s, size_t& p) { while (s[p] == ' ' || s[p] == '\t') p++; }

// 文字 s 從 pos 開始的 g.len 個位元組，雜湊是否等於固定文字的雜湊（不會讀過字串結尾）
static bool pat_lit_eq(const char* s, size_t pos, const PatSeg& g) {
    for (uint32_t i = 0; i < g.len; i++) if (s[pos + i] == 0) return false;
    return wct_hash(s + pos, g.len) == g.hash;
}

static bool pat_match(const std::vector<PatSeg>& segs, size_t si, const char* s, size_t pos, PatCaps& caps) {
    if (si == segs.size()) { pat_skip_ws(s, pos); return s[pos] == 0; }
    const PatSeg& g = segs[si];
    if (!g.is_var) {
        if (si > 0 && segs[si - 1].is_var) pat_skip_ws(s, pos);
        if (!pat_lit_eq(s, pos, g)) return false;
        return pat_match(segs, si + 1, s, pos + g.len, caps);
    }
    pat_skip_ws(s, pos);
    auto try_len = [&](size_t len) -> bool {
        caps.push_back(std::make_pair(g.var, std::string(s + pos, len)));
        if (pat_match(segs, si + 1, s, pos + len, caps)) return true;
        caps.pop_back();
        return false;
    };
    if (g.var == 'I') {                                   // 整數（-?d+），例如選單編號、數量
        size_t p = pos;
        if (s[p] == '-') p++;
        size_t d0 = p;
        while (isdigit((unsigned char)s[p])) p++;
        if (p == d0) return false;
        return try_len(p - pos);
    }
    if (g.var == 'T') {                                   // d{1,2}:dd
        size_t p = pos; int d = 0;
        while (isdigit((unsigned char)s[p]) && d < 2) { p++; d++; }
        if (d == 0 || s[p] != ':') return false;
        p++;
        if (!isdigit((unsigned char)s[p]) || !isdigit((unsigned char)s[p + 1])) return false;
        return try_len(p + 2 - pos);
    }
    if (g.var == 'D') {                                   // d{1,4}.d{1,3}
        size_t p = pos; int d = 0, e = 0;
        while (isdigit((unsigned char)s[p]) && d < 4) { p++; d++; }
        if (d == 0 || s[p] != '.') return false;
        p++;
        while (isdigit((unsigned char)s[p]) && e < 3) { p++; e++; }
        if (e == 0) return false;
        return try_len(p - pos);
    }
    size_t remain = strlen(s + pos);                      // 其他變數：萬用（由短到長試）
    for (size_t len = 1; len <= remain; len++) if (try_len(len)) return true;
    return false;
}

static bool match_patterns(const std::string& text, std::string& zh_out) {
    g_wct_last_pat = -1;
    if (g_patterns.empty()) return false;
    int idx = -2;
    PatCaps caps;
    auto c = g_pat_cache.find(text);
    if (c != g_pat_cache.end()) idx = c->second;
    if (idx == -2) {
        idx = -1;
        for (size_t i = 0; i < g_patterns.size(); i++) {
            caps.clear();
            if (pat_match(g_patterns[i].segs, 0, text.c_str(), 0, caps)) { idx = (int)i; break; }
        }
        if (g_pat_cache.size() > 4000) g_pat_cache.clear();
        g_pat_cache[text] = idx;
    } else if (idx >= 0) {
        caps.clear();
        if (!pat_match(g_patterns[idx].segs, 0, text.c_str(), 0, caps)) return false;
    }
    if (idx < 0) return false;
    g_wct_last_pat = idx;
    zh_out = g_patterns[idx].zh;
    // 同一個變數在英文模板裡出現多次 → 依出現順序逐個填入譯文裡的 $I；
    // 只出現一次的變數照舊：譯文裡所有同名的 $X 都換成它。
    int var_total[128] = {0};
    for (const PatSeg& sg : g_patterns[idx].segs) if (sg.is_var && (unsigned char)sg.var < 128) var_total[(unsigned char)sg.var]++;
    for (auto& kv : caps) {
        std::string val = kv.second;
        if (kv.first != 'T' && kv.first != 'D' && kv.first != 'I') {   // $R 軍階、$S 名稱…：抓到的英文若在翻譯表裡有對應（詞彙表），就換成中文（整數 $I 不查詞彙表）
            std::string gz;
            if (glossary_lookup(val, gz)) val = gz;
        }
        std::string tok = std::string("$") + kv.first;
        if ((unsigned char)kv.first < 128 && var_total[(unsigned char)kv.first] > 1) {
            size_t at = zh_out.find(tok);
            if (at != std::string::npos) zh_out.replace(at, tok.size(), val);
        } else {
            zh_out = str_replace(zh_out, tok, val);
        }
    }
    return true;
}

// ── 載入 ──

// 能當「詞彙」的條目：2～32 字、單行、不含變數、不含句子標點、不以句點結尾
static bool wct_term_ok(const std::string& k) {
    if (k.size() < 2 || k.size() > 32) return false;
    if (k.find_first_of("\n\r$,;:!?") != std::string::npos) return false;
    if (k.back() == '.' || k.back() == ' ') return false;
    if (letter_count_simple(k) < 2) return false;
    return true;
}

static void wct_finish_terms() {
    std::vector<uint32_t> lens;
    for (const auto& kv : g_terms) lens.push_back(kv.second.len);
    std::sort(lens.begin(), lens.end(), [](uint32_t a, uint32_t b) { return a > b; });
    lens.erase(std::unique(lens.begin(), lens.end()), lens.end());
    g_term_lens = lens;
}

// 純文字格式：英文 → 雜湊，並依載入順序決定「不分大小寫」與「詞彙」的代表（同一個小寫寫法，檔案裡先出現的算）
static void wct_load_text(char* line, int& count, int& pat_count, int& empty_zh) {
    std::unordered_map<std::string, std::string> tmp;     // 英文 → 中文（後出現的覆蓋前面的）
    std::vector<std::string> order;                       // 英文（依第一次出現的順序）
    auto put = [&](const std::string& k, const std::string& z) {
        auto it = tmp.find(k);
        if (it == tmp.end()) { order.push_back(k); tmp[k] = z; } else it->second = z;
    };
    while (*line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* cr = strchr(line, '\r');
        if (cr) *cr = 0;

        if (line[0] && line[0] != '#') {
            char* tab = strchr(line, '\t');
            if (tab) {
                std::string eng = unescape_field(std::string(line, tab));
                std::string zh_raw(tab + 1);
                size_t tab2 = zh_raw.find('\t');             // 第 3 欄以後（例如 wc1cht_missed.tsv 的來源標記）忽略
                if (tab2 != std::string::npos) zh_raw.erase(tab2);
                std::string zh = unescape_field(zh_raw);
                if (zh.empty()) { empty_zh++; }              // 還沒翻的條目：不要讓字消失，維持顯示英文
                else {
                    TransPattern P;
                    if (build_pattern(eng, zh, P)) { P.prio = (int)g_patterns.size(); g_patterns.push_back(P); pat_count++; }
                    else {
                        put(eng, zh);
                        std::string trimmed = eng;
                        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
                        if (!trimmed.empty() && trimmed != eng && tmp.find(trimmed) == tmp.end())
                            put(trimmed, zh);                // 遊戲字串有時多帶結尾空白（80-byte 定長記錄尤其常見）
                    }
                    count++;
                }
            }
        }
        line = nl ? nl + 1 : line + strlen(line);
    }
    for (const std::string& k : order) {
        const std::string& zh = tmp[k];
        g_trans_map[wct_hash(k)] = zh;
        std::string low = wct_lower(k);
        uint64_t lh = wct_hash(low);
        if (g_trans_ci.find(lh) == g_trans_ci.end()) g_trans_ci[lh] = zh;
        if (wct_term_ok(k) && g_terms.find(lh) == g_terms.end()) g_terms[lh] = TermInfo{ (uint32_t)k.size(), zh };
    }
}

// 雜湊格式
static void wct_load_hashed(char* line, int& count, int& pat_count, int& empty_zh, int& bad) {
    auto note_bad = [&](const char* why, const char* text) {
        if (bad < 5) wclog_err("load_translation_table: 雜湊格式的行無法解讀（%s）：%.40s", why, text);
        bad++;
    };
    while (*line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* cr = strchr(line, '\r');
        if (cr) *cr = 0;

        if (line[0] && line[0] != '#') {
            char* tab = strchr(line, '\t');
            if (!tab) { note_bad("沒有 Tab", line); }
            else {
                std::string keyf(line, tab);
                std::string rest(tab + 1);
                std::string meta;
                size_t t2 = rest.find('\t');
                if (t2 != std::string::npos) { meta = rest.substr(t2 + 1); rest.erase(t2); }
                std::string zh = unescape_field(rest);
                if (zh.empty()) { empty_zh++; }
                else if (!keyf.empty() && keyf[0] == '@') {            // 模板
                    TransPattern P;
                    bool ok = true;
                    size_t p = 1;
                    while (p <= keyf.size() && ok) {
                        size_t q = keyf.find('+', p);
                        std::string tok = keyf.substr(p, q == std::string::npos ? std::string::npos : q - p);
                        PatSeg s;
                        if (tok.size() == 2 && tok[0] == '$' && tok[1] >= 'A' && tok[1] <= 'Z') { s.is_var = true; s.var = tok[1]; }
                        else {
                            size_t dot = tok.find('.');
                            if (dot == std::string::npos || dot == 0 || dot > 6) { ok = false; break; }
                            s.len = (uint32_t)atoi(tok.substr(0, dot).c_str());
                            if (s.len == 0 || !wct_parse_hex(tok.c_str() + dot + 1, tok.size() - dot - 1, s.hash)) { ok = false; break; }
                        }
                        P.segs.push_back(s);
                        if (q == std::string::npos) break;
                        p = q + 1;
                    }
                    bool has_var = false;
                    for (auto& s : P.segs) if (s.is_var) has_var = true;
                    if (!ok || P.segs.empty() || !has_var) { note_bad("模板格式錯誤", line); }
                    else {
                        P.zh = zh; P.key = keyf;
                        P.prio = (int)g_patterns.size();
                        size_t m = meta.find("p=");
                        if (m != std::string::npos) P.prio = atoi(meta.c_str() + m + 2);
                        g_patterns.push_back(P); pat_count++; count++;
                    }
                } else {                                               // 一般條目
                    uint64_t eh = 0;
                    if (!wct_parse_hex(keyf.c_str(), keyf.size(), eh)) { note_bad("雜湊不是 16 位十六進位", line); }
                    else {
                        g_trans_map[eh] = zh;
                        count++;
                        uint64_t ch = 0; bool has_c = false; uint32_t tlen = 0;
                        size_t pos = 0;
                        while (pos <= meta.size() && !meta.empty()) {
                            size_t cm = meta.find(',', pos);
                            std::string tk = meta.substr(pos, cm == std::string::npos ? std::string::npos : cm - pos);
                            if (tk == "c") { ch = eh; has_c = true; }
                            else if (tk.size() == 18 && tk[0] == 'c' && tk[1] == '=') { has_c = wct_parse_hex(tk.c_str() + 2, 16, ch); }
                            else if (tk.size() > 2 && tk[0] == 't' && tk[1] == '=') tlen = (uint32_t)atoi(tk.c_str() + 2);
                            if (cm == std::string::npos) break;
                            pos = cm + 1;
                        }
                        if (has_c && g_trans_ci.find(ch) == g_trans_ci.end()) g_trans_ci[ch] = zh;
                        if (has_c && tlen > 0 && g_terms.find(ch) == g_terms.end()) g_terms[ch] = TermInfo{ tlen, zh };
                    }
                }
            }
        }
        line = nl ? nl + 1 : line + strlen(line);
    }
}

// 除錯用：讀含英文的純文字翻譯檔，建「雜湊 → 英文」與「模板鍵 → 英文模板」（只寫 log 用，不影響查表）
static void wct_load_source(const char* path) {
    g_src_text.clear(); g_src_pat.clear();
    FILE* fp = fopen(path, "rb");
    if (!fp) { wclog_err("[原文對照] 無法開啟 %s（ShowSource=1 需要含英文的純文字翻譯檔）", path); return; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    std::vector<char> buf(sz + 1, 0);
    fread(buf.data(), 1, sz, fp);
    fclose(fp);
    char* line = buf.data();
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line += 3;
    if (line[0] == '#' && strncmp(line, "#wc1cht:hash", 12) == 0) {
        wclog_err("[原文對照] %s 本身是雜湊格式，沒有英文可對照；請改指向含英文的純文字翻譯檔", path);
        return;
    }
    while (*line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* cr = strchr(line, '\r');
        if (cr) *cr = 0;
        if (line[0] && line[0] != '#') {
            char* tab = strchr(line, '\t');
            if (tab) {
                std::string eng = unescape_field(std::string(line, tab));
                std::string zh_raw(tab + 1);
                size_t tab2 = zh_raw.find('\t');
                if (tab2 != std::string::npos) zh_raw.erase(tab2);
                if (!unescape_field(zh_raw).empty()) {
                    TransPattern P;
                    if (build_pattern(eng, std::string("x"), P)) g_src_pat[wct_pattern_key(P.segs)] = eng;
                    else {
                        g_src_text[wct_hash(eng)] = eng;
                        std::string trimmed = eng;
                        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
                        if (!trimmed.empty() && trimmed != eng && g_src_text.find(wct_hash(trimmed)) == g_src_text.end())
                            g_src_text[wct_hash(trimmed)] = eng;
                    }
                }
            }
        }
        line = nl ? nl + 1 : line + strlen(line);
    }
    wclog("[原文對照] 從 %s 讀入 %d 條原文、%d 個模板（只用來寫 log）", path, (int)g_src_text.size(), (int)g_src_pat.size());
}

static bool load_translation_table(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        wclog_err("load_translation_table: 無法開啟 %s (errno=%d)", path, errno);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    std::vector<char> buf(sz + 1, 0);
    fread(buf.data(), 1, sz, fp);
    fclose(fp);

    g_trans_map.clear();
    g_trans_ci.clear();
    g_terms.clear();
    g_term_lens.clear();
    g_patterns.clear();
    g_pat_cache.clear();

    char* line = buf.data();
    int count = 0, pat_count = 0, empty_zh = 0, bad = 0;
    // UTF-8 BOM
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line += 3;

    // 檔頭註解有 "#wc1cht:hash=…" → 雜湊格式
    bool hashed = false;
    {
        const char* p = line;
        while (*p == '#') {
            const char* e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n >= 12 && strncmp(p, "#wc1cht:hash", 12) == 0) {
                std::string h(p, n);
                if (h.find("hash=fnv1a64") == std::string::npos) {
                    wclog_err("load_translation_table: 不支援的雜湊格式 \"%.40s\"（這個版本只支援 fnv1a64）", h.c_str());
                    return false;
                }
                hashed = true;
                break;
            }
            if (!e) break;
            p = e + 1;
        }
    }

    if (hashed) wct_load_hashed(line, count, pat_count, empty_zh, bad);
    else        wct_load_text(line, count, pat_count, empty_zh);

    std::stable_sort(g_patterns.begin(), g_patterns.end(),
                     [](const TransPattern& a, const TransPattern& b) { return a.prio < b.prio; });
    wct_finish_terms();
    g_src_text.clear(); g_src_pat.clear();
    if (hashed && g_wct_show_source && !g_wct_source_path.empty()) {
        wct_load_source(g_wct_source_path.c_str());
        for (auto& P : g_patterns) {                           // 模板名稱改顯示英文模板（log 比較好讀）
            auto it = g_src_pat.find(P.key);
            if (it != g_src_pat.end()) P.key = it->second;
        }
    }
    if (bad > 0) wclog_err("load_translation_table: 雜湊格式共有 %d 行無法解讀，已略過", bad);
    wclog("load_translation_table: 從 %s 載入 %d 條翻譯（%s格式；其中 %d 條含變數 $T/$D... 以模板比對；略過 %d 條空翻譯；可當詞彙的 %d 條）(檔案大小 %ld bytes)",
          path, count, hashed ? "雜湊" : "純文字", pat_count, empty_zh, (int)g_terms.size(), sz);
    for (size_t i = 0; i < g_patterns.size() && i < 20; i++)
        wclog("  模板 #%d: \"%s\"", (int)i, escape_field(g_patterns[i].key).c_str());
    return count > 0;
}

// ── 詞彙替換 ──
// 整句查不到時，把句中「在詞彙表裡有翻譯」的名稱/詞彙單獨換成中文，其餘保持英文。
// 例："Talk to HUNTER." 整句沒翻譯，但 HUNTER（不分大小寫）有 → "Talk to 獵人."
// 規則：整個單字比對（前後不能是英數字）、不分大小寫、長的詞彙優先；
//       數字/標點原樣保留。回傳是否有任何替換。
// 做法：由長到短，對文字裡每個「前後不是英數字」的區段算雜湊，查詞彙表。
static bool apply_terms_uncached(const std::string& text, std::string& out);
static bool apply_terms(const std::string& text, std::string& out) {
    static std::unordered_map<std::string, std::pair<bool, std::string> > cache;
    auto it = cache.find(text);
    if (it != cache.end()) { out = it->second.second; return it->second.first; }
    bool r = apply_terms_uncached(text, out);
    if (cache.size() > 4000) cache.clear();
    cache[text] = std::make_pair(r, r ? out : std::string());
    return r;
}
static bool apply_terms_uncached(const std::string& text, std::string& out) {
    if (g_terms.empty() || text.empty()) return false;
    const std::string low = wct_lower(text);
    const size_t n = low.size();
    std::vector<int> owner(n, -1);                                  // 每個位置被哪個詞彙佔用（詞彙編號）
    std::vector<const TermInfo*> used;                              // 編號 → 詞彙
    std::unordered_map<uint64_t, int> used_id;
    bool any = false;
    auto is_word = [](char c) { return isalnum((unsigned char)c) != 0; };
    for (uint32_t L : g_term_lens) {
        if (L > n) continue;
        for (size_t pos = 0; pos + L <= n; ) {
            size_t end = pos + L;
            bool left_ok  = (pos == 0) || !is_word(low[pos - 1]);
            bool right_ok = (end >= n) || !is_word(low[end]);
            if (!(left_ok && right_ok)) { pos++; continue; }
            bool free = true;
            for (size_t i = pos; i < end; i++) if (owner[i] >= 0) { free = false; break; }
            if (!free) { pos++; continue; }
            uint64_t h = wct_hash(low.data() + pos, L);
            auto ti = g_terms.find(h);
            if (ti == g_terms.end() || ti->second.len != L) { pos++; continue; }
            int id;
            auto ui = used_id.find(h);
            if (ui == used_id.end()) { id = (int)used.size(); used.push_back(&ti->second); used_id[h] = id; }
            else id = ui->second;
            for (size_t i = pos; i < end; i++) owner[i] = id;
            any = true;
            pos = end;
        }
    }
    if (!any) return false;
    out.clear();
    for (size_t i = 0; i < n; ) {
        if (owner[i] >= 0) {
            int id = owner[i];
            out += used[id]->zh;
            size_t j = i; while (j < n && owner[j] == id) j++;
            i = j;
        } else out += text[i++];
    }
    return true;
}

#endif // WC1CHT_TRANS_INC
