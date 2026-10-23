/*
 * wc1cht_profile.inc — 多版本位址對照（WC1 / SM1 / SM2 共用同一個 wc1cht.dll）
 *
 * 背景：Wing1_wcdx.exe、SM1_wcdx.exe、SM2_wcdx.exe 同一個引擎，但各自重新編譯，
 *   函式與全域變數的位址都不同（SM1 約 +0x20～+0x30；SM2 的程式碼約 +0x6D0～+0x850，
 *   資料區還被重新排列）。本模組讓 DLL 的其餘部分「只認 WC1 的位址」：
 *     • ini（Caller= / [SceneDrivers] / Viewport= / [Interp] Region*）一律照 WC1 位址寫，
 *       讀進來時由這裡換成目前 exe 的位址 → 同一份 ini 三個 exe 通用。
 *     • DLL 內部的寫死位址（FontContext、PlayerRecord、原生字元繪製函式、debug 旗標、
 *       FPS 限速器、繪圖/換頁函式…）改用 wcprof::sym(S_xxx) 取得。
 *
 * 如何辨識 exe：PE TimeDateStamp（WC1 0x32821571 / SM1 0x3282101C / SM2 0x3294AA25 / Transfer 0x325D4FED）。
 *   Transfer_wcdx.exe 是更早的 build，函式順序與 WC1 完全不同，位址表是依「函式內容」比對產生的（只收逐指令相同的函式）。
 *   不認得的 exe → 視為 WC1（恆等對應）；各模組本來就會用機器碼特徵再驗證一次，不符就停用。
 *
 * ini 裡直接指定「某個 exe 專用的原始位址」（不轉換）：在位址前加 "SM1:" / "SM2:" / "WC1:" / "TRF:"（Transfer），
 *   例如  Caller=0x004225FB,SM2:0x004229C6   （後者只在 SM2 生效，且照字面使用）。
 *
 * 表格（wc1cht_profile_tables.inc）由 genprof.py 自動產生，請勿手改。
 * 本檔的查表函式沒有 Windows 相依，可在 Linux 單獨測試（定義 WCPROF_NO_WINDOWS）。
 */
#ifndef WC1CHT_PROFILE_INC
#define WC1CHT_PROFILE_INC

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

#ifndef WCPROF_LOG
  #define WCPROF_LOG(...) ((void)0)
#endif

#include "wc1cht_profile_tables.inc"

namespace wcprof {

enum { ID_UNKNOWN = -1, ID_WC1 = 0, ID_SM1 = 1, ID_SM2 = 2, ID_TRANSFER = 3 };
static int g_id = -2;                       // -2 = 尚未偵測

static const char* id_name(int id) {
    switch (id) {
        case ID_WC1: return "WC1 (Wing1_wcdx.exe)";
        case ID_SM1: return "SM1 (Secret Missions 1)";
        case ID_SM2: return "SM2 (Secret Missions 2)";
        case ID_TRANSFER: return "Transfer (人物傳送工具)";
        default:     return "未知版本（視為 WC1）";
    }
}
// ini 位址前綴（"WC1:" / "SM1:" / "SM2:" / "TRF:"）
static const char* id_prefix(int id) {
    switch (id) { case ID_SM1: return "SM1"; case ID_SM2: return "SM2"; case ID_TRANSFER: return "TRF"; default: return "WC1"; }
}
static int id_from_timestamp(uint32_t ts) {
    if (ts == 0x325D4FEDu) return ID_TRANSFER;
    if (ts == 0x32821571u) return ID_WC1;
    if (ts == 0x3282101Cu) return ID_SM1;
    if (ts == 0x3294AA25u) return ID_SM2;
    return ID_UNKNOWN;
}

#if defined(_WIN32) && !defined(WCPROF_NO_WINDOWS)
static int detect() {
    if (g_id != -2) return g_id;
    const BYTE* base = (const BYTE*)GetModuleHandleA(NULL);
    int id = ID_UNKNOWN;
    uint32_t ts = 0;
    if (base) {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        const IMAGE_NT_HEADERS* nt  = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        ts = nt->FileHeader.TimeDateStamp;
        id = id_from_timestamp(ts);
    }
    g_id = id;
    WCPROF_LOG("[位址表] 偵測到遊戲執行檔：%s（PE 時間戳記 0x%08lX）", id_name(id), (unsigned long)ts);
    return g_id;
}
#else
static int detect() { return g_id == -2 ? (g_id = ID_UNKNOWN) : g_id; }
#endif

static int id() { return g_id == -2 ? detect() : g_id; }
static int table_index() { int i = id(); return i < 0 ? 0 : i; }      // 未知 → WC1
// 太空戰相關模組（F12 debug、降速、probe、interp）是否適用：Transfer 是獨立的傳送工具，沒有太空戰迴圈
static bool has_game_modules() { return id() != ID_TRANSFER; }

// 某個「已知符號」在目前 exe 的位址
static uint32_t sym(tbl::Sym s) { return tbl::SYM[s][table_index()]; }
static uint32_t sym_wc1(tbl::Sym s) { return tbl::SYM[s][0]; }

namespace detail {
    static bool find_seg(const tbl::Seg* t, size_t n, uint32_t a, uint32_t& delta) {
        size_t lo = 0, hi = n;                       // 找最後一個 lo <= a 的區段（表格依 lo 遞增）
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (t[mid].lo <= a) lo = mid + 1; else hi = mid; }
        if (lo == 0) return false;
        const tbl::Seg& s = t[lo - 1];
        if (a >= s.lo && a < s.hi) { delta = s.delta; return true; }
        return false;
    }
    static void tables(int idx, const tbl::Seg*& code, size_t& ncode, const tbl::Seg*& data, size_t& ndata,
                       const tbl::Ret*& ret, size_t& nret) {
        if (idx == 3) { code = tbl::CODE3; ncode = sizeof(tbl::CODE3) / sizeof(tbl::Seg); data = tbl::DATA3; ndata = sizeof(tbl::DATA3) / sizeof(tbl::Seg); ret = tbl::RET3; nret = sizeof(tbl::RET3) / sizeof(tbl::Ret); }
        else if (idx == 1) { code = tbl::CODE1; ncode = sizeof(tbl::CODE1) / sizeof(tbl::Seg); data = tbl::DATA1; ndata = sizeof(tbl::DATA1) / sizeof(tbl::Seg); ret = tbl::RET1; nret = sizeof(tbl::RET1) / sizeof(tbl::Ret); }
        else          { code = tbl::CODE2; ncode = sizeof(tbl::CODE2) / sizeof(tbl::Seg); data = tbl::DATA2; ndata = sizeof(tbl::DATA2) / sizeof(tbl::Seg); ret = tbl::RET2; nret = sizeof(tbl::RET2) / sizeof(tbl::Ret); }
    }
}

// 程式碼位址（函式起點／指令起點）：WC1 位址 → 目前 exe。成功回傳 true。
static bool code(uint32_t a, uint32_t& out) {
    int idx = table_index();
    if (idx == 0) { out = a; return true; }
    for (int s = tbl::S_DRAW_CHAR; s <= tbl::S_PRESENT; s++)      // 已知函式符號優先（明確驗證過）
        if (tbl::SYM[s][0] == a) { out = tbl::SYM[s][idx]; return out != 0; }     // 0 = 該 exe 沒有這個函式
    const tbl::Seg *c, *d; const tbl::Ret* r; size_t nc, nd, nr; detail::tables(idx, c, nc, d, nd, r, nr);
    uint32_t delta;
    if (detail::find_seg(c, nc, a, delta)) { out = a + delta; return true; }
    return false;
}

// 返回位址（call 之後的下一個指令位址；Caller= / [SceneDrivers] 用的就是這種）。
// 一個 WC1 位址在 SM 版可能對應多個位址（例如 SM2 的空戰通訊字幕有三個分支），所以回傳清單。
static std::vector<uint32_t> code_ret_all(uint32_t ra) {
    std::vector<uint32_t> res;
    int idx = table_index();
    if (idx == 0) { res.push_back(ra); return res; }
    const tbl::Seg *c, *d; const tbl::Ret* r; size_t nc, nd, nr; detail::tables(idx, c, nc, d, nd, r, nr);
    for (size_t i = 0; i < nr; i++) if (r[i].key == ra) res.push_back(r[i].val);      // 明確對應優先
    if (!res.empty()) return res;
    uint32_t delta;
    if (detail::find_seg(c, nc, ra - 1, delta)) res.push_back(ra + delta);             // 以「call 指令最後一個 byte」所在區段換算
    return res;
}

// 資料位址（全域變數、Viewport 結構）：WC1 位址 → 目前 exe。
static bool data(uint32_t a, uint32_t& out) {
    int idx = table_index();
    if (idx == 0) { out = a; return true; }
    for (int s = 0; s <= tbl::S_VP_MFD_R; s++)                         // 已知資料符號優先
        if (tbl::SYM[s][0] == a) { out = tbl::SYM[s][idx]; return out != 0; }     // 0 = 該 exe 沒有這個變數
    const tbl::Seg *c, *d; const tbl::Ret* r; size_t nc, nd, nr; detail::tables(idx, c, nc, d, nd, r, nr);
    uint32_t delta;
    if (detail::find_seg(d, nd, a, delta)) { out = a + delta; return true; }
    return false;
}

// ini 位址 token 的前綴："SM1:" / "SM2:" / "WC1:"。
//   回傳 0 = 沒有前綴（要轉換）；1 = 前綴符合目前 exe（照字面使用，不轉換）；-1 = 前綴是別的 exe（略過）
static int strip_exe_prefix(std::string& tok) {
    size_t i = 0; while (i < tok.size() && (tok[i] == ' ' || tok[i] == '\t')) i++;
    if (tok.size() >= i + 4 && tok[i + 3] == ':') {
        std::string p = tok.substr(i, 3);
        for (auto& ch : p) ch = (char)toupper((unsigned char)ch);
        int pid = (p == "WC1") ? ID_WC1 : (p == "SM1") ? ID_SM1 : (p == "SM2") ? ID_SM2 : (p == "TRF") ? ID_TRANSFER : -9;
        if (pid != -9) {
            tok = tok.substr(i + 4);
            int cur = id(); if (cur < 0) cur = ID_WC1;
            return pid == cur ? 1 : -1;
        }
    }
    return 0;
}

} // namespace wcprof

#endif // WC1CHT_PROFILE_INC
