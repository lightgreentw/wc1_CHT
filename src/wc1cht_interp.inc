/*
 * wc1cht_interp.inc — 實驗支線 v3：「慢速但順暢」原型（子畫面內插）
 *
 * 原理（依據 v2 的觀測資料與戰鬥主迴圈 0x446ADB 的分析）：
 *   主迴圈每圈：輸入 → 更新(0x44686D) → 繪圖(0x4247AB，經 thunk 0x4027A2) → 換頁+限速(0x46EE89 + 0x46EEA3)。
 *   模擬每圈只走固定一步，所以速度 ∝ 圈數/秒。要「慢但順」，就讓模擬維持低頻（例如 8 Hz），
 *   但每個模擬步多畫 k-1 個「介於上一步與這一步之間」的子畫面，再畫真正的這一步（共 k 格）。
 *
 *   做法：把 0x446D72 那一行 `call 0x4027A2`（繪圖）改成跳到本模組的 trampoline。每次更新完成（update 回傳非 0）：
 *     1. 讀取遊戲狀態區（.data 的 [RegionLo,RegionHi)）目前值 cur，與前一步 p1、前前一步 p2。
 *     2. 挑出「平滑變化」的欄位（見 select_tracked）。
 *     3. 對 i = 1..k-1：把這些欄位寫成 p1 + (cur-p1)*i/k → 呼叫繪圖 → 設 dirty、換頁（內含限速）→ 還原成 cur。
 *     4. 回到遊戲原本的繪圖（畫 cur），迴圈尾端照常換頁。
 *   因為每個子畫面之後都把欄位還原成 cur，遊戲邏輯永遠只看到真實狀態；最壞情況只是子畫面出現視覺瑕疵。
 *   代價：畫面比真實狀態延遲一個模擬步（8 Hz 時約 125ms）——386 實機本來就有這種延遲。
 *
 * 安全機制：
 *   - Ctrl+熱鍵（預設 Ctrl+F11）隨時開關；預設不自動開啟。
 *   - 只在太空戰迴圈（mode==1）且 update 回傳非 0 時動作；補丁前驗證位元組，不符就不裝。
 *   - 補丁只在「不在戰鬥迴圈」時寫入（避免寫到正在執行的指令）。
 *   - SideEffectLog=1：前 3 個模擬步記錄「繪圖+換頁到底改了哪些全域變數」，供下一輪分析副作用。
 *
 * 整合（實驗版 wc1cht.cpp 已加好）：
 *   #include "wc1cht_interp.inc"    （放在 #include "wc1cht_ext.inc" 之後）
 *   wccinterp_attach(hInst);        （放在 wccext_attach(hInst); 之後）
 * 設定見 wc1cht.ini 的 [Interp]。
 */
#ifndef WC1CHT_INTERP_INC
#define WC1CHT_INTERP_INC

#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace wcinterp {

// ───────────── 純邏輯（無 Windows 相依；可在 Linux 自測）─────────────
// 從三個連續的狀態快照挑出「值得內插」的欄位：
//   - 這一步有變化，且 |Δ| 在 [minDelta, maxDelta]（排除旗標/計數器/跳變）
//   - 與上一步的變化量同方向，且變化量差異 ≤ tol×|Δ|（等速、等加速、衰減都過；急轉彎的瞬間不過）
static void select_tracked(const int32_t* cur, const int32_t* p1, const int32_t* p2, size_t K,
                           int minDelta, long long maxDelta, float tol, std::vector<uint32_t>& out) {
    out.clear();
    for (size_t k = 0; k < K; k++) {
        long long d1 = (long long)cur[k] - p1[k];
        long long d0 = (long long)p1[k] - p2[k];
        long long a1 = d1 < 0 ? -d1 : d1;
        if (a1 < minDelta || a1 > maxDelta) continue;
        if ((d0 < 0) != (d1 < 0) || d0 == 0) continue;
        long long diff = d1 - d0; if (diff < 0) diff = -diff;
        if ((double)diff > (double)tol * (double)a1 + 1.0) continue;
        out.push_back((uint32_t)k);
    }
}
// 依「模擬頻率 simHz」與「目標顯示頻率 target」挑倍率 k：顯示頻率 = simHz×k ≥ target（k 限制在 1..maxK）
static int pick_k(float target, float simHz, int maxK) {
    if (simHz <= 0.f || target <= 0.f) return 1;
    double r = (double)target / (double)simHz;
    int k = (int)(r - 1e-4); if ((double)k < r - 1e-4) k++;     // ceil，容許浮點誤差
    if (k < 1) k = 1;
    if (k > maxK) k = maxK;
    return k;
}
static inline int32_t lerp_i32(int32_t a, int32_t b, int i, int k) {
    return (int32_t)((long long)a + (long long)((long long)b - a) * i / k);
}

} // namespace wcinterp

#ifdef _WIN32
#ifndef WCCINTERP_LOG
  #define WCCINTERP_LOG wclog_force
#endif
#ifndef WCCINTERP_ERR
  #define WCCINTERP_ERR wclog_err
#endif

namespace wcinterp {

// 以下是 WC1 的位址；interp_thread 啟動時依 exe 版本（WC1 / SM1 / SM2）換算。
static DWORD A_CALL_SITE   = 0x00446D72;  // call 0x4027A2（繪圖）：WC1 為 E8 2B BA FB FF（位移依版本不同，驗證時動態計算）
static DWORD A_RENDER      = 0x004027A2;  // 繪圖 thunk（三個 exe 剛好都在 0x4027A2）
static DWORD A_MARK_DIRTY  = 0x0046EE89;  // 設「畫面已變更」旗標
static DWORD A_PRESENT     = 0x0046EEA3;  // 換頁 + 限速
static DWORD A_MODE        = 0x004B0EB4;  // 1 = 太空戰迴圈

struct Cfg {
    bool  enable = false, autoon = false, sidelog = true;
    int   mult = 3, min_delta = 3, vk = VK_F11;
    float target_fps = 0.f;          // >0：自動挑倍率，讓顯示頻率 ≥ 此值；0：固定用 mult
    long long max_delta = 20000000;
    float tol = 0.35f;
    DWORD lo = 0x00621E00, hi = 0x00626900;     // WC1 的預設掃描區；其他版本見 interp_thread 的換算
};
static Cfg  c;
static HINSTANCE g_hinst = NULL;
static volatile LONG g_on = 0;

// 狀態快照（以 DWORD 為單位）
static std::vector<int32_t> s_cur, s_p1, s_p2;
static int   s_hist = 0;
static std::vector<uint32_t> s_track;
static size_t s_K = 0;
static DWORD  s_dlo = 0, s_dhi = 0;          // .data 範圍（副作用分析用）
static int   s_sidelog_frames = 0;
static std::vector<DWORD> s_seen_side;       // 已記錄過的副作用位址
static long  s_frames = 0;

typedef void (__cdecl *fn_t)(void);

static bool data_range() {
    BYTE* base = (BYTE*)GetModuleHandleA(NULL); if (!base) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sh++)
        if (strncmp((const char*)sh->Name, ".data", 5) == 0) {
            s_dlo = (DWORD)(uintptr_t)(base + sh->VirtualAddress);
            s_dhi = s_dlo + (sh->Misc.VirtualSize + 3) / 4 * 4; return true;
        }
    return false;
}

static void side_effect_report(const std::vector<int32_t>& before) {
    size_t n = before.size(); const int32_t* now = (const int32_t*)(uintptr_t)s_dlo;
    int logged = 0, total = 0;
    for (size_t k = 0; k < n; k++) {
        if (now[k] == before[k]) continue;
        total++;
        DWORD a = s_dlo + (DWORD)(4 * k);
        bool seen = false; for (DWORD x : s_seen_side) if (x == a) { seen = true; break; }
        if (seen || s_seen_side.size() >= 150) continue;
        s_seen_side.push_back(a);
        WCCINTERP_LOG("[interp] 副作用：0x%08lX  %ld → %ld", (unsigned long)a, (long)before[k], (long)now[k]);
        logged++;
    }
    WCCINTERP_LOG("[interp] 副作用傾印：這一輪子畫面後共 %d 個 DWORD 與之前不同（新記錄 %d 個）", total, logged);
}

} // namespace wcinterp

// ───────────── trampoline：接在 0x446D72 的 call 上 ─────────────
extern "C" {
    __attribute__((used)) DWORD wcinterp_orig = 0x004027A2;
    __attribute__((used)) void __cdecl wcinterp_cb(DWORD caller_ebp);
}
asm(
    ".text\n"
    ".globl _wcinterp_tramp\n"
    "_wcinterp_tramp:\n"
    "    pushal\n"
    "    pushfl\n"
    "    pushl %ebp\n"            // 呼叫端的 ebp（主迴圈的 frame），[ebp-8] = update 的回傳值
    "    call _wcinterp_cb\n"
    "    addl $4, %esp\n"
    "    popfl\n"
    "    popal\n"
    "    jmp *_wcinterp_orig\n"   // 回到原本的繪圖 thunk，之後 ret 回主迴圈
);
extern "C" void wcinterp_tramp(void);

extern "C" void __cdecl wcinterp_cb(DWORD caller_ebp) {
    using namespace wcinterp;
    if (!g_on || s_K == 0) { s_hist = 0; return; }
    DWORD mode = 0; if (IsBadReadPtr((void*)(uintptr_t)A_MODE, 4)) return;
    memcpy(&mode, (void*)(uintptr_t)A_MODE, 4);
    if (mode != 1) { s_hist = 0; return; }
    if (IsBadReadPtr((void*)(uintptr_t)(caller_ebp - 8), 4)) return;
    int ready = *(volatile int*)(uintptr_t)(caller_ebp - 8);       // update 回傳值：非 0 才會換頁
    if (!ready) return;

    int32_t* region = (int32_t*)(uintptr_t)c.lo;
    memcpy(s_cur.data(), region, s_K * 4);
    s_frames++;

    if (s_hist >= 2) select_tracked(s_cur.data(), s_p1.data(), s_p2.data(), s_K, c.min_delta, c.max_delta, c.tol, s_track);
    else s_track.clear();

    int kmul = wcext::g_render_mult; if (kmul < 1) kmul = 1; if (kmul > 8) kmul = 8;
    if (s_hist >= 2 && !s_track.empty() && kmul > 1) {
        std::vector<int32_t> side;
        bool doside = c.sidelog && s_sidelog_frames < 3 && s_dhi > s_dlo;
        if (doside) { side.resize((s_dhi - s_dlo) / 4); memcpy(side.data(), (void*)(uintptr_t)s_dlo, side.size() * 4); }

        fn_t render = (fn_t)(uintptr_t)A_RENDER, mark = (fn_t)(uintptr_t)A_MARK_DIRTY, pres = (fn_t)(uintptr_t)A_PRESENT;
        for (int i = 1; i < kmul; i++) {
            for (uint32_t k : s_track) region[k] = lerp_i32(s_p1[k], s_cur[k], i, kmul);
            render(); mark(); pres();
            for (uint32_t k : s_track) region[k] = s_cur[k];
        }
        if (doside) { s_sidelog_frames++; side_effect_report(side); }
        if (s_frames % 40 == 1 || s_frames < 6)
            WCCINTERP_LOG("[interp] 第 %ld 步：內插欄位 %d 個，倍率 k=%d", s_frames, (int)s_track.size(), kmul);
    }
    s_p2 = s_p1; s_p1 = s_cur; if (s_hist < 2) s_hist++;
}

namespace wcinterp {

static DWORD WINAPI interp_thread(LPVOID) {
    Sleep(1200);
    char dirp[MAX_PATH]; DWORD n = GetModuleFileNameA(g_hinst, dirp, MAX_PATH);
    std::string ini = "wc1cht.ini";
    if (n && n < MAX_PATH) { char* sl = strrchr(dirp, '\\'); if (sl) sl[1] = 0; ini = std::string(dirp) + "wc1cht.ini"; }
    const char* f = ini.c_str(); char b[64];
    c.enable  = GetPrivateProfileIntA("Interp", "Enable", 0, f) != 0;
    if (!c.enable) return 0;
    c.autoon  = GetPrivateProfileIntA("Interp", "AutoOn", 0, f) != 0;
    c.mult    = GetPrivateProfileIntA("Interp", "Multiplier", 3, f); if (c.mult < 2) c.mult = 2; if (c.mult > 8) c.mult = 8;
    GetPrivateProfileStringA("Interp", "TargetFps", "30", b, sizeof(b), f); c.target_fps = (float)atof(b);
    c.min_delta = GetPrivateProfileIntA("Interp", "MinDelta", 3, f);
    GetPrivateProfileStringA("Interp", "MaxDelta", "20000000", b, sizeof(b), f); c.max_delta = atoll(b);
    GetPrivateProfileStringA("Interp", "SmoothTol", "0.35", b, sizeof(b), f); c.tol = (float)atof(b);
    c.sidelog = GetPrivateProfileIntA("Interp", "SideEffectLog", 1, f) != 0;
    {   // 掃描區：ini 照 WC1 位址寫（預設 0x621E00..0x626900）。
        //   WC1：照字面。SM1/SM2：ini 值等於 WC1 預設 → 用位址表內建的對應區間；
        //   其他值 → 當作 WC1 位址換算；也可用 RegionLo_SM1 / RegionHi_SM1 / RegionLo_SM2 / RegionHi_SM2 直接指定該 exe 的原始位址。
        using namespace wcprof::tbl;
        int pid = wcprof::id();
        auto get = [&](const char* key, const char* def) { GetPrivateProfileStringA("Interp", key, def, b, sizeof(b), f); return (DWORD)strtoul(b, NULL, 0); };
        DWORD lo_wc1 = get("RegionLo", "0x621E00"), hi_wc1 = get("RegionHi", "0x626900");
        c.lo = lo_wc1; c.hi = hi_wc1;
        if (pid > 0) {
            char kl[32], kh[32]; wsprintfA(kl, "RegionLo_SM%d", pid); wsprintfA(kh, "RegionHi_SM%d", pid);
            DWORD ol = get(kl, "0"), oh = get(kh, "0");
            if (ol && oh) { c.lo = ol; c.hi = oh; WCCINTERP_LOG("[interp] 掃描區：使用 ini 指定的 SM%d 專用位址 0x%08lX..0x%08lX", pid, (unsigned long)c.lo, (unsigned long)c.hi); }
            else if (lo_wc1 == wcprof::sym_wc1(S_REGION_LO) && hi_wc1 == wcprof::sym_wc1(S_REGION_HI)) { c.lo = wcprof::sym(S_REGION_LO); c.hi = wcprof::sym(S_REGION_HI); WCCINTERP_LOG("[interp] 掃描區：WC1 預設 → %s 對應區間 0x%08lX..0x%08lX", wcprof::id_name(pid), (unsigned long)c.lo, (unsigned long)c.hi); }
            else { uint32_t ml = 0, mh = 0; if (wcprof::data(lo_wc1, ml) && wcprof::data(hi_wc1, mh) && mh > ml) { c.lo = ml; c.hi = mh; } else WCCINTERP_LOG("[interp] 掃描區換算失敗，維持字面值（請用 RegionLo_SM%d / RegionHi_SM%d 指定）", pid, pid); }
        }
    }
    GetPrivateProfileStringA("Interp", "ToggleKey", "F11", b, sizeof(b), f);
    if ((b[0] == 'F' || b[0] == 'f') && atoi(b + 1) >= 1) c.vk = VK_F1 + atoi(b + 1) - 1;

    // 依 exe 版本換算位址
    A_CALL_SITE = wcprof::sym(wcprof::tbl::S_CALLSITE); A_RENDER = wcprof::sym(wcprof::tbl::S_RENDER);
    A_MARK_DIRTY = wcprof::sym(wcprof::tbl::S_MARK_DIRTY); A_PRESENT = wcprof::sym(wcprof::tbl::S_PRESENT);
    A_MODE = wcprof::sym(wcprof::tbl::S_FPS_MODE);
    wcinterp_orig = A_RENDER;                                                   // trampoline 結尾 jmp 的目標

    // 驗證補丁位置與遊戲函式位址
    BYTE sig_call[5]; sig_call[0] = 0xE8;                                       // call A_RENDER（相對位移依版本不同，動態計算）
    { int32_t rel0 = (int32_t)(A_RENDER - (A_CALL_SITE + 5)); memcpy(sig_call + 1, &rel0, 4); }
    static const BYTE sig_mark[6] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 };      // 0x46EE89 開頭
    static const BYTE sig_pres[6] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xA0 };      // 0x46EEA3 開頭
    BYTE cb5[5], mk[6], pr[6]; bool ok = true;
    ok = ok && !IsBadReadPtr((void*)(uintptr_t)A_CALL_SITE, 5) && !IsBadReadPtr((void*)(uintptr_t)A_MARK_DIRTY, 6) && !IsBadReadPtr((void*)(uintptr_t)A_PRESENT, 6);
    if (ok) { memcpy(cb5, (void*)(uintptr_t)A_CALL_SITE, 5); memcpy(mk, (void*)(uintptr_t)A_MARK_DIRTY, 6); memcpy(pr, (void*)(uintptr_t)A_PRESENT, 6); }
    ok = ok && !memcmp(cb5, sig_call, 5) && !memcmp(mk, sig_mark, 6) && !memcmp(pr, sig_pres, 6);
    ok = ok && (DWORD)(uintptr_t)GetModuleHandleA(NULL) == 0x00400000 && c.hi > c.lo && c.hi - c.lo <= 0x40000 && !IsBadReadPtr((void*)(uintptr_t)c.lo, c.hi - c.lo) && !IsBadWritePtr((void*)(uintptr_t)c.lo, c.hi - c.lo);
    if (!ok) { WCCINTERP_ERR("[interp] 位址/補丁位置驗證失敗，停用（版本不同？）"); return 0; }
    if (!data_range()) { s_dlo = s_dhi = 0; }
    s_K = (c.hi - c.lo) / 4; s_cur.assign(s_K, 0); s_p1.assign(s_K, 0); s_p2.assign(s_K, 0);

    // 等到「不在戰鬥迴圈」再寫入補丁，避免寫到正在執行的指令
    for (;;) {
        DWORD mode = 1; if (!IsBadReadPtr((void*)(uintptr_t)A_MODE, 4)) memcpy(&mode, (void*)(uintptr_t)A_MODE, 4);
        if (mode == 0) break;
        Sleep(200);
    }
    DWORD old = 0;
    if (!VirtualProtect((void*)(uintptr_t)A_CALL_SITE, 5, PAGE_EXECUTE_READWRITE, &old)) { WCCINTERP_ERR("[interp] VirtualProtect 失敗，停用"); return 0; }
    BYTE patch[5]; patch[0] = 0xE8; int32_t rel = (int32_t)((DWORD)(uintptr_t)&wcinterp_tramp - (A_CALL_SITE + 5)); memcpy(patch + 1, &rel, 4);
    memcpy((void*)(uintptr_t)A_CALL_SITE, patch, 5);
    VirtualProtect((void*)(uintptr_t)A_CALL_SITE, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)A_CALL_SITE, 5);
    WCCINTERP_LOG("[interp] 已安裝 trampoline（0x%08lX → 0x%08lX）；倍率=%d 區域=0x%08lX..0x%08lX（%lu DWORD） Ctrl+F%d 開關",
                  (unsigned long)A_CALL_SITE, (unsigned long)(uintptr_t)&wcinterp_tramp, c.mult, (unsigned long)c.lo, (unsigned long)c.hi, (unsigned long)s_K, c.vk - VK_F1 + 1);

    auto apply = [&](bool on) {
        if (on) {
            if (wcext::g_fps <= 0.f) wcext::g_fps = wcext::g_game_default;   // 沒設降速時，用遊戲預設 20 Hz 當模擬頻率
            wcext::g_render_mult = c.target_fps > 0.f ? pick_k(c.target_fps, wcext::g_fps, 8) : c.mult; s_hist = 0; s_sidelog_frames = 0;
            InterlockedExchange(&g_on, 1);
        } else {
            InterlockedExchange(&g_on, 0); wcext::g_render_mult = 1;
        }
        WCCINTERP_LOG("[interp] 內插 %s（模擬 %.1f Hz → 顯示 %.1f FPS，倍率 k=%d%s）", on ? "開啟" : "關閉", wcext::g_fps,
                      wcext::g_fps * (on ? wcext::g_render_mult : 1), (int)wcext::g_render_mult, (on && c.target_fps > 0.f) ? "，自動：顯示 ≥ 目標" : "");
        Beep(on ? 1800 : 600, 90);
    };
    if (c.autoon) apply(true);

    bool prev = false;
    for (;;) {
        Sleep(20);
        HWND fg = GetForegroundWindow(); DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
        bool d = pid == GetCurrentProcessId() && (GetAsyncKeyState(c.vk) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        if (d && !prev) apply(!g_on);
        prev = d;
        if (g_on && c.target_fps > 0.f) {                       // 模擬頻率被 F10/F11 或 Alt+N/M 改掉時，自動重算倍率
            int k = pick_k(c.target_fps, wcext::g_fps, 8);
            if (k != wcext::g_render_mult) {
                wcext::g_render_mult = k;
                WCCINTERP_LOG("[interp] 自動倍率 k=%d（模擬 %.2f Hz → 顯示 %.1f FPS，目標 ≥ %.0f）", k, wcext::g_fps, wcext::g_fps * k, c.target_fps);
            }
        }
    }
    return 0;
}

} // namespace wcinterp

static void wccinterp_attach(HINSTANCE hInst) {
    if (!wcprof::has_game_modules()) { wclog_force("[支線] %s 沒有太空戰迴圈，interp 模組不啟用", wcprof::id_name(wcprof::id())); return; }
    wcinterp::g_hinst = hInst;
    HANDLE h = CreateThread(NULL, 0, wcinterp::interp_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
#endif // _WIN32
#endif // WC1CHT_INTERP_INC
