/*
 * wc1cht_ext.inc — Wing Commander 1 (WCDX / Kilrathi Saga 版) 支線模組
 *   1. F12 開關 debug 模式（等同原版 "Origin -k -b" / 登錄檔 Cheater）
 *   2. 降速：把遊戲內建的 FPS 限速器壓低，模擬 386 時代的速度。單位是「模擬 CPU 時脈 (MHz)」
 *      F10 慢 / F11 快；[Speed] Enable=1 時，遊戲原生的 Alt+N / Alt+M 也等同 F10 / F11（AltNM=1）
 *      每次切換都會用遊戲自己的「提示訊息」顯示目前檔位（例如 CPU 33 MHz [4/8] 8.25 Hz）
 *   3. Ctrl+F12 把診斷資訊寫進 wc1cht_debug.log（供下一輪逆向用）
 *
 * ── 整合方式（主線 wc1cht.cpp 只需要兩行）──────────────────────────
 *   (a) 在 DllMain 之前加：   #include "wc1cht_ext.inc"
 *   (b) 在 DllMain 的 DLL_PROCESS_ATTACH 區塊最後加：   wccext_attach(hInst);
 * 本檔只依賴主線的 wclog_force()（記錄用）；若單獨編譯，會自動退回自己的簡單 log。
 * 不需要改 wc1cht_patch.cpp、不需要重新 patch exe：所有動作都是 DLL 執行緒直接讀寫遊戲全域變數，
 * 以及（選用）把 exe 匯入表裡的 GetAsyncKeyState 換成我們的版本。
 *
 * ── 位址來源 ──────────────────────────────────────────────────
 * 位址一律由 wc1cht_profile.inc 的 wcprof::sym(S_xxx) 依目前的 exe（WC1 / SM1 / SM2）換算，本檔不再寫死位址。
 * 啟動時會先比對關鍵位址的機器碼特徵；不符（版本不同）就停用對應功能並寫 log，絕不亂寫記憶體。
 * 「提示訊息緩衝區」的位址不寫死：從遊戲處理 Alt+N/M 的函式（ADAPT）機器碼裡取出，並驗證格式字串內容。
 */
#ifndef WC1CHT_EXT_INC
#define WC1CHT_EXT_INC

#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include "wc1cht_speedcfg.inc"   // [Speed] 設定解析（純邏輯）

#ifndef WCCEXT_LOG
  #define WCCEXT_LOG wclog_force
#endif
#ifndef WCCEXT_ERR
  #define WCCEXT_ERR wclog_err      // 載入階段的錯誤：不受 ini [Log] 開關影響，一律記錄
#endif

namespace wcext {

// ───────────── 遊戲全域變數／函式位址（由 wcprof 依目前的 exe 換算；WC1 / SM1 / SM2 共用）─────────────
static DWORD A_CHEAT = 0, A_RELAX_K = 0, A_RELAX_B = 0, A_CTRLDEL = 0;                  // Debug 旗標群（Origin -k -b / 登錄檔 Cheater 設定的就是這幾個）
static DWORD A_FRAME_MS = 0, A_FRAME_DL = 0, A_FPS_MODE = 0, A_FPS_FLIGHT = 0, A_FPS_OTHER = 0;   // FPS 限速器
static DWORD A_TARGET = 0, A_SHIP_WING = 0, A_SHIP_KIND = 0;                            // 診斷用
static DWORD F_LIMITER = 0, F_ADAPT = 0, F_DEMO = 0;                                    // 驗證機器碼特徵用的函式
static DWORD A_HINT_FMT = 0, A_HINT_BUF = 0;                                            // 遊戲「Space Flight Max FPS」提示的格式字串與訊息緩衝區

static void init_addresses() {
    using namespace wcprof::tbl;
    A_CHEAT = wcprof::sym(S_CHEAT);   A_RELAX_K = wcprof::sym(S_RELAX_K);   A_RELAX_B = wcprof::sym(S_RELAX_B);   A_CTRLDEL = wcprof::sym(S_CTRLDEL);
    A_FRAME_MS = wcprof::sym(S_FRAME_MS);   A_FRAME_DL = wcprof::sym(S_FRAME_DL);   A_FPS_MODE = wcprof::sym(S_FPS_MODE);
    A_FPS_FLIGHT = wcprof::sym(S_FPS_FLIGHT);   A_FPS_OTHER = wcprof::sym(S_FPS_OTHER);
    A_TARGET = wcprof::sym(S_TARGET);   A_SHIP_WING = wcprof::sym(S_SHIP_WING);   A_SHIP_KIND = wcprof::sym(S_SHIP_KIND);
    F_LIMITER = wcprof::sym(S_LIMITER);   F_ADAPT = wcprof::sym(S_ADAPT);   F_DEMO = wcprof::sym(S_DEMO);
}

// ───────────── 設定 ─────────────
struct Config {
    bool  debug_enable = true;
    bool  debug_auto   = false;     // 遊戲啟動就開 debug
    bool  relax_checks = true;      // 同時套用 -k -b（解除遊戲內部的限制檢查）
    bool  set_ctrldel  = true;      // 同時把 ctrl.del 旗標設 1（Ctrl+Del/Ins 需要）
    bool  alt_as_ctrl  = true;      // Debug 開啟時，Alt+Del / Alt+Ins 視同 Ctrl+Del / Ctrl+Ins
    int   vk_debug     = VK_F12;
    bool  speed_enable = true;
    // 速度：唯一單位是「模擬 CPU 時脈 (MHz)」；遊戲速度(每秒模擬步數 Hz) = MHz × HzPerMHz
    std::vector<float> mhz_steps { 16.f, 20.f, 25.f, 33.f, 40.f, 50.f, 66.f, 80.f };   // F10/F11 的檔位
    float hz_per_mhz   = 0.25f;     // 校準值（估計）：33MHz ≈ 8.25Hz
    float start_hz     = 0.f;       // 啟動時套用的遊戲速度（Hz）；0 = 不干涉
    float start_mhz    = 0.f;
    std::string start_src;          // 啟動速度的來源（log 用）
    std::string legacy_note;        // 偵測到的舊鍵（log 提醒用）
    bool  alt_nm       = true;      // Speed 啟用時，遊戲原生的 Alt+N / Alt+M 等同 F10 / F11（換檔位），不再是 ±0.5 Hz
    bool  hint         = true;      // 切換檔位時，用遊戲自己的提示訊息顯示目前的 MHz 檔位
    float other_fps    = 0.f;       // >0 才限制非太空戰畫面
    int   vk_slower    = VK_F10;
    int   vk_faster    = VK_F11;
    int   timer_res_ms = 1;         // 0 = 不呼叫 timeBeginPeriod
    int   poll_ms      = 20;
};

static Config   g_cfg;
static HINSTANCE g_hinst = NULL;
static volatile LONG g_dbg_on = 0;
static bool     g_addr_ok_dbg = false, g_addr_ok_speed = false;

// ───────────── 小工具 ─────────────
static bool  safe_rd(DWORD a, void* out, int n) {
    if (IsBadReadPtr((const void*)(uintptr_t)a, n)) return false;
    memcpy(out, (const void*)(uintptr_t)a, n); return true;
}
static bool  rd32(DWORD a, DWORD& v) { return safe_rd(a, &v, 4); }
static bool  rdf (DWORD a, float& v) { return safe_rd(a, &v, 4); }
static bool  wr32(DWORD a, DWORD v)  {
    if (IsBadWritePtr((void*)(uintptr_t)a, 4)) return false;
    *(volatile DWORD*)(uintptr_t)a = v; return true;
}
static bool  wrf (DWORD a, float v)  { DWORD d; memcpy(&d, &v, 4); return wr32(a, d); }

static bool code_is(DWORD a, const BYTE* sig, int n) {
    BYTE b[16]; if (n > 16 || !safe_rd(a, b, n)) return false;
    return memcmp(b, sig, n) == 0;
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
static int vk_from_name(const std::string& s0, int def) {
    std::string s = trim(s0); for (auto& c : s) c = (char)tolower((unsigned char)c);
    if (s.empty()) return def;
    if (s == "none" || s == "off" || s == "0") return 0;
    if (s.size() >= 2 && s[0] == 'f' && isdigit((unsigned char)s[1])) {
        int n = atoi(s.c_str() + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (s == "pgup" || s == "pageup")   return VK_PRIOR;
    if (s == "pgdn" || s == "pagedown") return VK_NEXT;
    if (s == "home") return VK_HOME;
    if (s == "end")  return VK_END;
    if (s == "pause") return VK_PAUSE;
    if (s.size() == 1 && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    return (int)strtol(s.c_str(), nullptr, 0);
}

static std::string ini_path() {
    char p[MAX_PATH]; DWORD n = GetModuleFileNameA(g_hinst, p, MAX_PATH);
    if (!n || n >= MAX_PATH) return "wc1cht.ini";
    char* sl = strrchr(p, '\\'); if (sl) sl[1] = 0;
    return std::string(p) + "wc1cht.ini";
}
static std::string ini_s(const std::string& ini, const char* sec, const char* key, const char* def) {
    char buf[512]; GetPrivateProfileStringA(sec, key, def, buf, sizeof(buf), ini.c_str());
    // 去掉行尾註解（; 之後）
    char* sc = strchr(buf, ';'); if (sc) *sc = 0;
    return trim(buf);
}
static int   ini_i(const std::string& ini, const char* sec, const char* key, int def) {
    std::string s = ini_s(ini, sec, key, ""); return s.empty() ? def : (int)strtol(s.c_str(), nullptr, 0);
}
static float ini_f(const std::string& ini, const char* sec, const char* key, float def) {
    std::string s = ini_s(ini, sec, key, ""); return s.empty() ? def : (float)atof(s.c_str());
}

static std::vector<float> parse_list(const std::string& pl, float lo, float hi) {
    std::vector<float> ps; size_t pos = 0;
    while (pos <= pl.size()) {
        size_t q = pl.find(',', pos); if (q == std::string::npos) q = pl.size();
        float f = (float)atof(trim(pl.substr(pos, q - pos)).c_str());
        if (f >= lo && f <= hi) ps.push_back(f);
        pos = q + 1;
    }
    std::sort(ps.begin(), ps.end()); ps.erase(std::unique(ps.begin(), ps.end()), ps.end());
    return ps;
}

static void load_config() {
    std::string ini = ini_path();
    Config c;
    c.debug_enable = ini_i(ini, "Debug", "Enable", 1) != 0;
    c.debug_auto   = ini_i(ini, "Debug", "AutoEnable", 0) != 0;
    c.relax_checks = ini_i(ini, "Debug", "RelaxChecks", 1) != 0;
    c.set_ctrldel  = ini_i(ini, "Debug", "EnableCtrlDelFlag", 1) != 0;
    c.alt_as_ctrl  = ini_i(ini, "Debug", "AltAsCtrl", 1) != 0;
    c.vk_debug     = vk_from_name(ini_s(ini, "Debug", "ToggleKey", "F12"), VK_F12);

    c.speed_enable = ini_i(ini, "Speed", "Enable", 1) != 0;
    c.alt_nm       = ini_i(ini, "Speed", "AltNM", 1) != 0;
    c.hint         = ini_i(ini, "Speed", "Hint", 1) != 0;
    c.other_fps    = ini_f(ini, "Speed", "OtherFps", 0.f);
    c.vk_slower    = vk_from_name(ini_s(ini, "Speed", "SlowerKey", "F10"), VK_F10);
    c.vk_faster    = vk_from_name(ini_s(ini, "Speed", "FasterKey", "F11"), VK_F11);
    c.timer_res_ms = ini_i(ini, "Speed", "TimerResolution", 1);
    c.poll_ms      = std::max(5, std::min(100, ini_i(ini, "Speed", "PollMs", 20)));

    // ── 速度設定：新鍵（[Speed] CpuMHz / MHzSteps / HzPerMHz / GameHz）優先，舊鍵（[CpuClock]、Preset 等）只做相容 ──
    wcspeed::Input in;
    std::string t;
    t = ini_s(ini, "Speed", "CpuMHz", "");  if (!t.empty()) in.new_mhz = (float)atof(t.c_str());
    t = ini_s(ini, "Speed", "GameHz", "");  if (!t.empty()) in.new_game_hz = (float)atof(t.c_str());
    std::string legacy;
    auto note = [&](const char* k) { if (!legacy.empty()) legacy += "、"; legacy += k; };
    std::string lc_en  = ini_s(ini, "CpuClock", "Enable", "");
    std::string lc_mhz = ini_s(ini, "CpuClock", "MHz", "");
    std::string lc_hpm = ini_s(ini, "CpuClock", "HzPerMHz", "");
    std::string lc_pre = ini_s(ini, "CpuClock", "Presets", "");
    if (!lc_en.empty() || !lc_mhz.empty() || !lc_hpm.empty() || !lc_pre.empty()) note("[CpuClock] 區段");
    in.legacy_cpu_enable = lc_en.empty() ? 0 : ((int)strtol(lc_en.c_str(), nullptr, 0) != 0);
    if (!lc_mhz.empty()) in.legacy_mhz = (float)atof(lc_mhz.c_str());
    std::string ls_pre = ini_s(ini, "Speed", "Preset", "");
    std::string ls_pls = ini_s(ini, "Speed", "Presets", "");
    std::string ls_ffp = ini_s(ini, "Speed", "FlightFps", "");
    if (!ls_pre.empty()) { note("[Speed] Preset");    in.legacy_preset = (int)strtol(ls_pre.c_str(), nullptr, 0); }
    if (!ls_pls.empty()) { note("[Speed] Presets");   in.legacy_presets = parse_list(ls_pls, 1.f, 100.f); }
    if (!ls_ffp.empty()) { note("[Speed] FlightFps"); in.legacy_flight_fps = (float)atof(ls_ffp.c_str()); }
    // HzPerMHz：新鍵 > 舊 [CpuClock] > 預設 0.25
    t = ini_s(ini, "Speed", "HzPerMHz", "");
    if (!t.empty()) c.hz_per_mhz = (float)atof(t.c_str());
    else if (!lc_hpm.empty()) c.hz_per_mhz = (float)atof(lc_hpm.c_str());
    if (c.hz_per_mhz < 0.01f) c.hz_per_mhz = 0.25f;
    in.hz_per_mhz = c.hz_per_mhz;
    // 檔位：新鍵 MHzSteps > 舊 [CpuClock] Presets > 預設
    std::vector<float> steps = parse_list(ini_s(ini, "Speed", "MHzSteps", ""), 1.f, 1000.f);
    if (steps.empty() && !lc_pre.empty()) steps = parse_list(lc_pre, 1.f, 1000.f);
    if (!steps.empty()) c.mhz_steps = steps;

    wcspeed::Output out = wcspeed::resolve(in);
    c.start_hz = out.hz; c.start_mhz = out.mhz;
    c.start_src = out.source + (out.clamped ? "（已夾到 1～32 Hz）" : "");
    c.legacy_note = legacy;
    g_cfg = c;
}

// ───────────── 位址特徵檢查 ─────────────
static bool g_hint_ok = false;     // 提示訊息緩衝區是否取得並驗證成功

static void verify_addresses() {
    init_addresses();
    g_addr_ok_speed = g_addr_ok_dbg = g_hint_ok = false;
    const int pid = wcprof::id();
    if (!wcprof::has_game_modules()) {            // Transfer 是獨立的傳送工具，沒有太空戰迴圈
        WCCEXT_LOG("[支線] %s 沒有太空戰迴圈，debug／降速模組不啟用", wcprof::id_name(pid));
        return;
    }
    // 限速器函式：push ebp; mov ebp,esp; sub esp,0Ch
    static const BYTE sig_limiter[] = { 0x55,0x8B,0xEC,0x83,0xEC,0x0C };
    // FPS 調整函式（Alt+N/M 的處理）：push ebp; mov ebp,esp; push ebx; push esi; push edi
    static const BYTE sig_adapt[]   = { 0x55,0x8B,0xEC,0x53,0x56,0x57 };
    // demo 函式開頭，以及它讀取作弊旗標的指令 mov eax,[CHEAT]（+9 處；位址由 profile 導出，不寫死）
    static const BYTE sig_demo[]    = { 0x55,0x8B,0xEC,0x83,0xEC,0x10 };
    BYTE sig_demo2[5] = { 0xA1, (BYTE)(A_CHEAT & 0xFF), (BYTE)((A_CHEAT >> 8) & 0xFF), (BYTE)((A_CHEAT >> 16) & 0xFF), (BYTE)((A_CHEAT >> 24) & 0xFF) };

    bool base_ok = ((DWORD)(uintptr_t)GetModuleHandleA(NULL) == 0x00400000);
    g_addr_ok_speed = base_ok && F_LIMITER && F_ADAPT && A_FRAME_MS && A_FPS_FLIGHT && A_FPS_MODE &&
                      code_is(F_LIMITER, sig_limiter, 6) && code_is(F_ADAPT, sig_adapt, 6);
    g_addr_ok_dbg   = base_ok && F_DEMO && A_CHEAT && code_is(F_DEMO, sig_demo, 6) && code_is(F_DEMO + 9, sig_demo2, 5);

    float f = 0; DWORD ms = 0;
    if (g_addr_ok_speed) {
        if (!rdf(A_FPS_FLIGHT, f) || f < 1.f || f > 200.f) g_addr_ok_speed = false;
        if (!rd32(A_FRAME_MS, ms) || ms > 5000)            g_addr_ok_speed = false;
    }

    // 提示訊息緩衝區：從 ADAPT 的機器碼取出 push <格式字串> / push <緩衝區>，再驗證格式字串真的是遊戲的 FPS 提示
    if (g_addr_ok_speed && g_cfg.hint) {
        BYTE code[0x70]; unsigned fmt = 0, buf = 0;
        if (safe_rd(F_ADAPT, code, sizeof code) && wcspeed::parse_adapt_hint(code, sizeof code, fmt, buf)) {
            char txt[24] = {0};
            static const char kFmt[] = "Space Flight Max FPS";
            if (safe_rd(fmt, txt, 20) && memcmp(txt, kFmt, 20) == 0 && !IsBadWritePtr((void*)(uintptr_t)buf, 32)) {
                A_HINT_FMT = fmt; A_HINT_BUF = buf; g_hint_ok = true;
            }
        }
    }
    const bool bad = !g_addr_ok_speed || !g_addr_ok_dbg || (g_cfg.hint && g_addr_ok_speed && !g_hint_ok);
    (bad ? wclog_err : wclog_force)("[支線] 位址驗證（%s）：ImageBase=%s  速度模組=%s  Debug 模組=%s  提示訊息=%s%s",
               wcprof::id_name(pid), base_ok ? "0x400000" : "不符", g_addr_ok_speed ? "可用" : "停用", g_addr_ok_dbg ? "可用" : "停用",
               g_hint_ok ? "可用" : (g_cfg.hint ? "停用" : "已關閉"),
               g_hint_ok ? "" : (g_cfg.hint && g_addr_ok_speed ? "（找不到訊息緩衝區：切換檔位仍有效，只是不顯示提示；F10/F11 也不會模擬按鍵）" : ""));
    if (g_hint_ok) WCCEXT_LOG("[支線] 提示訊息：格式字串=0x%08lX 緩衝區=0x%08lX", (unsigned long)A_HINT_FMT, (unsigned long)A_HINT_BUF);
}

// ───────────── Debug 模式 ─────────────
static DWORD g_saved[4] = {0,0,0,0};
static bool  g_saved_valid = false;

static void debug_apply(bool on) {
    if (!g_addr_ok_dbg) return;
    if (on) {
        if (!g_saved_valid) {
            rd32(A_CHEAT, g_saved[0]); rd32(A_RELAX_K, g_saved[1]);
            rd32(A_RELAX_B, g_saved[2]); rd32(A_CTRLDEL, g_saved[3]);
            g_saved_valid = true;
        }
        wr32(A_CHEAT, 1);
        if (g_cfg.relax_checks) { wr32(A_RELAX_K, 0); wr32(A_RELAX_B, 0); }
        if (g_cfg.set_ctrldel)  wr32(A_CTRLDEL, 1);
    } else if (g_saved_valid) {
        wr32(A_CHEAT, g_saved[0]); wr32(A_RELAX_K, g_saved[1]);
        wr32(A_RELAX_B, g_saved[2]); wr32(A_CTRLDEL, g_saved[3]);
    }
}

// 每個輪詢週期重新確認一次（遊戲的 demo 回放函式會暫存/還原旗標，這裡確保最後仍是我們要的狀態）
static void debug_reassert() {
    if (!g_dbg_on || !g_addr_ok_dbg) return;
    DWORD a = 0, k = 1, b = 1, c = 0;
    rd32(A_CHEAT, a); rd32(A_RELAX_K, k); rd32(A_RELAX_B, b); rd32(A_CTRLDEL, c);
    bool bad = (a != 1) || (g_cfg.relax_checks && (k != 0 || b != 0)) || (g_cfg.set_ctrldel && c != 1);
    if (bad) debug_apply(true);
}

// ───────────── Alt→Ctrl 相容（IAT 掛鉤 GetAsyncKeyState）─────────────
// 遊戲的作弊鍵修飾鍵是用 GetAsyncKeyState(VK_CONTROL=0x11) 判斷的（見文件），
// 原版 DOS 攻略寫的是 Alt+Del / Alt+Ins。Debug 開啟時，把「Alt 按著」回報成「Ctrl 也按著」。
typedef SHORT (WINAPI *PFN_GAKS)(int);
static PFN_GAKS g_real_gaks = NULL;
static volatile DWORD g_inject_until = 0;   // 我們正在「幫忙按 Alt+N/M」到這個 tick 為止；這段時間不做 Alt→Ctrl 相容，避免誤觸 Ctrl+N/M
static SHORT WINAPI my_gaks(int vk) {
    SHORT r = g_real_gaks(vk);
    if (vk == VK_CONTROL && g_dbg_on && g_cfg.alt_as_ctrl && !(r & 0x8000) && (int)(g_inject_until - GetTickCount()) <= 0) {
        if (g_real_gaks(VK_MENU) & 0x8000) return (SHORT)0x8000;
    }
    return r;
}

static bool install_gaks_hook() {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    HMODULE exe = GetModuleHandleA(NULL);
    if (!u32 || !exe) return false;
    g_real_gaks = (PFN_GAKS)GetProcAddress(u32, "GetAsyncKeyState");
    if (!g_real_gaks) return false;

    BYTE* base = (BYTE*)exe;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return false;
    IMAGE_IMPORT_DESCRIPTOR* imd = (IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress);
    int patched = 0;
    for (; imd->Name; imd++) {
        const char* dn = (const char*)(base + imd->Name);
        if (lstrcmpiA(dn, "user32.dll") != 0) continue;
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imd->FirstThunk);
        for (; iat->u1.Function; iat++) {
            if ((PFN_GAKS)(uintptr_t)iat->u1.Function != g_real_gaks) continue;
            DWORD old; 
            if (!VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old)) continue;
            iat->u1.Function = (DWORD)(uintptr_t)&my_gaks;
            VirtualProtect(&iat->u1.Function, 4, old, &old);
            patched++;
        }
    }
    WCCEXT_LOG("[支線] GetAsyncKeyState IAT 掛鉤：改寫 %d 個匯入槽", patched);
    return patched > 0;
}

// ───────────── 速度（FPS 限速器）─────────────
static bool key_down(int vk);                 // 定義在後面的熱鍵區

static volatile int g_render_mult = 1;  // 實驗 v3：每個模擬步畫幾格（由 wc1cht_interp.inc 設定；1 = 不內插）
static float g_fps          = 0.f;     // 我們想要的太空戰 FPS = 遊戲速度 Hz（0 = 還沒決定，不干涉）
static float g_last_written = 0.f;
static float g_game_default = 20.f;

static float g_cpu_mhz = 0.f;      // 目前模擬的 CPU 時脈（MHz）；0 = 尚未指定
static float mhz_to_hz(float mhz) { float hz = mhz * g_cfg.hz_per_mhz; if (hz < 1.f) hz = 1.f; if (hz > 32.f) hz = 32.f; return hz; }

// 遊戲原生的 Alt+N / Alt+M 處理，以及我們模擬的那一下，都會「晚一個畫面」才被遊戲執行（遊戲每個畫面才處理一次訊息，
// 4 Hz 時一個畫面 250ms），所以切換檔位後的這段時間內：把原生處理造成的 FPS 變數變動還原、把它印出的提示改寫成 MHz。
static DWORD g_revert_until = 0;
static DWORD g_ignore_alt_until = 0;      // 這段時間內忽略 Alt+N/M 的按鍵邊緣（避免同一次按鍵被算兩次）
static char  g_hint_text[40] = "";
static inline bool tick_open(DWORD until) { return (int)(until - GetTickCount()) > 0; }

// Beep 會卡住 60ms；放在獨立執行緒，才不會延遲我們改寫提示文字
struct BeepArg { DWORD f, d; };
static DWORD WINAPI beep_thread(LPVOID p) { BeepArg a = *(BeepArg*)p; delete (BeepArg*)p; Beep(a.f, a.d); return 0; }
static void beep_async(DWORD f, DWORD d) {
    BeepArg* a = new BeepArg{f, d};
    HANDLE h = CreateThread(NULL, 0, beep_thread, a, 0, NULL);
    if (h) CloseHandle(h); else delete a;
}

// F10 / F11 / Alt+N / Alt+M 共用：dir = -1 調慢、+1 調快（換一個 MHz 檔位）
static void speed_step(int dir) {
    if (!g_addr_ok_speed) return;
    const auto& P = g_cfg.mhz_steps;
    float cur = g_cpu_mhz > 0 ? g_cpu_mhz : (g_fps > 0 ? g_fps : g_game_default) / g_cfg.hz_per_mhz;
    float pick = wcspeed::step_pick(P, cur, dir);
    int edge = (pick == cur) ? dir : 0;                       // 已在端點
    if (!edge || g_fps <= 0.f) { g_cpu_mhz = pick; g_fps = mhz_to_hz(pick); }   // 端點且尚未接管時，也要接管，才能還原遊戲原生的 ±0.5
    int idx = wcspeed::step_index(P, g_cpu_mhz);
    std::string h = wcspeed::format_hint(g_cpu_mhz, idx, (int)P.size(), g_fps, edge);
    snprintf(g_hint_text, sizeof g_hint_text, "%s", h.c_str());
    g_revert_until = GetTickCount() + 800;
    wrf(A_FPS_FLIGHT, g_fps); g_last_written = g_fps;         // 先寫入；遊戲原生處理若稍後才執行，會在 speed_enforce 被還原
    if (edge) {
        WCCEXT_LOG("[支線] 模擬 CPU 時脈已在%s端點：%.0f MHz（第 %d/%d 檔；遊戲速度 %.2f Hz）", dir < 0 ? "最慢" : "最快", g_cpu_mhz, idx, (int)P.size(), g_fps);
        beep_async(200, 40);
    } else {
        WCCEXT_LOG("[支線] 模擬 CPU 時脈 %s → %.0f MHz（第 %d/%d 檔；遊戲速度 %.2f Hz，每秒模擬步數）", dir < 0 ? "調慢" : "調快", g_cpu_mhz, idx, (int)P.size(), g_fps);
        beep_async((DWORD)(300 + g_fps * 40), 60);
    }
}

// 把遊戲「Space Flight Max FPS : x.x」提示改寫成我們的 MHz 提示。
// 遊戲的訊息顯示保存的是「指向 sprintf 緩衝區的指標」，所以改寫緩衝區內容，下一次繪製就會顯示新文字。
// 只有緩衝區內容確實是遊戲的 FPS 提示時才改寫，不會動到其他訊息。
static void hint_tick() {
    if (!g_hint_ok || !g_cfg.hint || !g_hint_text[0] || !tick_open(g_revert_until)) return;
    char cur[24] = {0};
    if (!safe_rd(A_HINT_BUF, cur, 20) || memcmp(cur, "Space Flight Max FPS", 20) != 0) return;
    char out[40]; size_t n = strlen(g_hint_text); if (n > 31) n = 31;
    memcpy(out, g_hint_text, n); out[n] = 0;
    if (!IsBadWritePtr((void*)(uintptr_t)A_HINT_BUF, n + 1)) memcpy((void*)(uintptr_t)A_HINT_BUF, out, n + 1);
}

// 讓遊戲「自己」顯示提示：幫你按一次 Alt+N（dir<0）或 Alt+M（dir>0），走遊戲自己的處理路徑，
// 這樣 F10/F11 與 Alt+N/M 是同一條路，提示由遊戲執行緒產生，我們不必在別的執行緒呼叫遊戲函式。
static void inject_alt_key(int vk) {
    if (!g_hint_ok || !g_cfg.hint) return;
    DWORD now = GetTickCount();
    g_inject_until = now + 400;               // 模擬期間不做 Alt→Ctrl 相容
    g_ignore_alt_until = now + 600;           // 模擬出來的 Alt+N/M 不要再被當成一次新的按鍵
    INPUT in[4]; memset(in, 0, sizeof in);
    WORD scAlt = (WORD)MapVirtualKeyA(VK_MENU, 0), scKey = (WORD)MapVirtualKeyA((UINT)vk, 0);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_MENU; in[0].ki.wScan = scAlt;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = (WORD)vk; in[1].ki.wScan = scKey;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = (WORD)vk; in[2].ki.wScan = scKey; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_MENU; in[3].ki.wScan = scAlt; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(4, in, sizeof(INPUT));
    if (sent != 4) WCCEXT_LOG("[支線] 警告：模擬 Alt+%c 只送出 %u/4 個事件，提示可能不會顯示", (char)vk, (unsigned)sent);
}

static void speed_enforce() {
    if (!g_addr_ok_speed || g_fps <= 0.f) return;
    float have = 0; if (!rdf(A_FPS_FLIGHT, have)) return;
    // 遊戲原生的 Alt+N / Alt+M 改了變數（±0.5，夾在 8～32）
    if (have != g_last_written && have >= 1.f && have <= 100.f) {
        if (tick_open(g_revert_until)) {
            // 剛切換過檔位：這是遊戲原生處理晚一步才執行的結果，不當成新的按鍵，下面會還原
        } else if (g_cfg.alt_nm) {
            // 沒被我們攔到按鍵邊緣（按得太快）：由「舊值 → 新值」反推方向；低於 8 Hz 時兩個方向都被夾成 8.0，改看按鍵狀態
            int d = wcspeed::infer_dir(g_last_written, have);
            if (d == 0) { bool n = key_down('N'), m = key_down('M'); if (m && !n) d = +1; else if (n && !m) d = -1; }
            g_ignore_alt_until = GetTickCount() + 300;
            if (d) { WCCEXT_LOG("[支線] 偵測到遊戲原生 Alt+%c（FPS 變數 %.2f → %.2f），改為切換 MHz 檔位", d > 0 ? 'M' : 'N', g_last_written, have); speed_step(d); }
            else {
                // 無法判斷方向：速度維持不變（變數會在下面還原），但遊戲剛印的「Space Flight Max FPS : x.x」是錯的，要改寫成目前的真實檔位
                WCCEXT_LOG("[支線] 偵測到 FPS 變數被改（%.2f → %.2f）但無法判斷方向，維持 %.0f MHz 並還原", g_last_written, have, g_cpu_mhz);
                std::string h = wcspeed::format_hint(g_cpu_mhz, wcspeed::step_index(g_cfg.mhz_steps, g_cpu_mhz), (int)g_cfg.mhz_steps.size(), g_fps, 0);
                snprintf(g_hint_text, sizeof g_hint_text, "%s", h.c_str());
                g_revert_until = GetTickCount() + 800;
            }
        } else if (have != g_fps) {
            WCCEXT_LOG("[支線] 偵測到遊戲內建微調：%.2f → %.2f Hz（接受）", g_fps, have);
            g_fps = have; g_cpu_mhz = g_fps / g_cfg.hz_per_mhz;
        }
    }
    float cur = 0; rdf(A_FPS_FLIGHT, cur);
    if (cur != g_fps) wrf(A_FPS_FLIGHT, g_fps);
    g_last_written = g_fps;

    DWORD mode = 0, ms = 0; rd32(A_FPS_MODE, mode); rd32(A_FRAME_MS, ms);
    float want_fps = 0;
    if (mode == 1) want_fps = g_fps;
    else if (g_cfg.other_fps > 0.f) { float o = 0; rdf(A_FPS_OTHER, o); if (o != g_cfg.other_fps) wrf(A_FPS_OTHER, g_cfg.other_fps); want_fps = g_cfg.other_fps; }
    if (want_fps > 0.f) {
        float disp_fps = want_fps * (mode == 1 && g_render_mult > 1 ? (float)g_render_mult : 1.0f);   // 內插時，每次換頁的間隔 = 模擬間隔 / 倍率
        DWORD want_ms = (DWORD)(1000.0f / disp_fps + 0.5f); if (want_ms < 1) want_ms = 1;
        if (ms != want_ms) { wr32(A_FRAME_MS, want_ms); wr32(A_FRAME_DL, 0); }
    }
}

static void set_timer_resolution() {
    if (g_cfg.timer_res_ms <= 0) return;
    typedef DWORD (WINAPI *PFN_TBP)(DWORD);
    HMODULE mm = GetModuleHandleA("winmm.dll"); if (!mm) mm = LoadLibraryA("winmm.dll");
    PFN_TBP f = mm ? (PFN_TBP)GetProcAddress(mm, "timeBeginPeriod") : NULL;
    if (f) { DWORD r = f((DWORD)g_cfg.timer_res_ms); WCCEXT_LOG("[支線] timeBeginPeriod(%d) → %lu（0 = 成功）", g_cfg.timer_res_ms, (unsigned long)r); }
}

// ───────────── 診斷傾印（Ctrl+F12）─────────────
static void dump_state() {
    DWORD a=0,k=0,b=0,c=0, mode=0, ms=0, dl=0; float f1=0,f2=0;
    rd32(A_CHEAT,a); rd32(A_RELAX_K,k); rd32(A_RELAX_B,b); rd32(A_CTRLDEL,c);
    rd32(A_FPS_MODE,mode); rd32(A_FRAME_MS,ms); rd32(A_FRAME_DL,dl); rdf(A_FPS_FLIGHT,f1); rdf(A_FPS_OTHER,f2);
    BYTE tgt = 0xEE; safe_rd(A_TARGET, &tgt, 1);
    WCCEXT_LOG("========== [支線] 診斷傾印 ==========");
    WCCEXT_LOG("Debug 旗標: cheat=%lu relaxK=%lu relaxB=%lu ctrl.del=%lu （我們的狀態 on=%ld）",
               (unsigned long)a,(unsigned long)k,(unsigned long)b,(unsigned long)c,(long)g_dbg_on);
    WCCEXT_LOG("FPS: mode=%lu  flightFps=%.2f otherFps=%.2f  frameMs=%lu  deadline=%lu  我們的目標=%.2f",
               (unsigned long)mode, f1, f2, (unsigned long)ms, (unsigned long)dl, g_fps);
    WCCEXT_LOG("鎖定目標 [0x%08lX]=%d", (unsigned long)A_TARGET, (int)(signed char)tgt);
    WCCEXT_LOG("速度：模擬 %.1f MHz → 遊戲速度 %.2f Hz  倍率 k=%d  AltNM=%d Hint=%d（提示緩衝區 %s）", g_cpu_mhz, g_fps, (int)g_render_mult,
               g_cfg.alt_nm ? 1 : 0, g_cfg.hint ? 1 : 0, g_hint_ok ? "可用" : "停用");
    for (int i = 0; i < 10; i++) {
        DWORD w = 0xFFFFFFFF, kd = 0xFFFFFFFF;
        rd32(A_SHIP_WING + 4 * i, w); rd32(A_SHIP_KIND + 4 * i, kd);
        WCCEXT_LOG("  艦 #%d  wing=%ld  kind=0x%lX", i, (long)w, (unsigned long)kd);
    }
}

// ───────────── 熱鍵輪詢執行緒 ─────────────
static bool game_has_focus() {
    HWND fg = GetForegroundWindow(); if (!fg) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
static bool key_down(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

static DWORD WINAPI ext_thread(LPVOID) {
    Sleep(500);                       // 讓遊戲先把初始化做完
    load_config();
    WCCEXT_LOG("[支線] wc1cht_ext 啟動  Debug 鍵=0x%02X  慢=0x%02X 快=0x%02X  MHz 檔位數=%d  AltNM=%d Hint=%d",
               g_cfg.vk_debug, g_cfg.vk_slower, g_cfg.vk_faster, (int)g_cfg.mhz_steps.size(), g_cfg.alt_nm ? 1 : 0, g_cfg.hint ? 1 : 0);
    verify_addresses();
    set_timer_resolution();

    if (g_addr_ok_speed && g_cfg.speed_enable) {
        rdf(A_FPS_FLIGHT, g_game_default);
        g_last_written = g_game_default;
        if (!g_cfg.legacy_note.empty())
            WCCEXT_LOG("[支線] 偵測到舊速度設定：%s。已依相容規則換算；新寫法請用 [Speed] 的 CpuMHz / MHzSteps / HzPerMHz / GameHz（見 ini 註解）", g_cfg.legacy_note.c_str());
        if (g_cfg.start_hz > 0.f) {
            g_fps = g_cfg.start_hz; g_cpu_mhz = g_cfg.start_mhz;
            WCCEXT_LOG("[支線] 速度：%s → 模擬 %.1f MHz = 遊戲速度 %.2f Hz（每秒模擬步數；HzPerMHz=%.3f；遊戲原預設 %.1f Hz ≈ %.0f MHz）%s",
                       g_cfg.start_src.c_str(), g_cfg.start_mhz, g_cfg.start_hz, g_cfg.hz_per_mhz, g_game_default, g_game_default / g_cfg.hz_per_mhz,
                       (g_cfg.start_hz >= g_game_default - 0.01f) ? "  ← 不會比遊戲預設慢" : "");
        } else {
            WCCEXT_LOG("[支線] 速度：%s，維持遊戲預設 %.1f Hz（≈ %.0f MHz）；F10/F11 可逐檔調整", g_cfg.start_src.c_str(), g_game_default, g_game_default / g_cfg.hz_per_mhz);
        }
    }
    if (g_addr_ok_dbg && g_cfg.debug_enable) {
        if (install_gaks_hook() == false && g_cfg.alt_as_ctrl) WCCEXT_ERR("[支線] 警告：Alt→Ctrl 相容掛鉤失敗，請直接用 Ctrl+Del / Ctrl+Ins");
        if (g_cfg.debug_auto) { g_dbg_on = 1; debug_apply(true); WCCEXT_LOG("[支線] Debug 模式：啟動時自動開啟"); }
    }

    bool p_dbg = false, p_slow = false, p_fast = false, p_n = false, p_m = false;
    for (;;) {
        Sleep(tick_open(g_revert_until) ? 2 : g_cfg.poll_ms);     // 剛切換檔位的短時間內用快速輪詢，讓提示改寫得及時
        bool focus = game_has_focus();
        bool d = focus && key_down(g_cfg.vk_debug);
        bool ctrl = key_down(VK_CONTROL);                // 實驗版：Ctrl+F10 留給觀測器，不當作降速鍵
        bool alt  = key_down(VK_MENU);
        bool s = focus && !ctrl && key_down(g_cfg.vk_slower);
        bool f = focus && !ctrl && key_down(g_cfg.vk_faster);
        bool en = focus && alt && !ctrl && key_down('N');   // 遊戲原生的 Alt+N（-0.5 Hz）
        bool em = focus && alt && !ctrl && key_down('M');   // 遊戲原生的 Alt+M（+0.5 Hz）

        if (d && !p_dbg && g_cfg.debug_enable) {
            if (key_down(VK_CONTROL)) dump_state();
            else if (!g_addr_ok_dbg) { WCCEXT_ERR("[支線] Debug 模組停用（位址驗證失敗）"); Beep(200, 120); }
            else {
                g_dbg_on = !g_dbg_on; debug_apply(g_dbg_on != 0);
                WCCEXT_LOG("[支線] Debug 模式 %s（F12）", g_dbg_on ? "開啟" : "關閉");
                Beep(g_dbg_on ? 1500 : 700, 90);
            }
        }
        if (g_cfg.speed_enable && g_addr_ok_speed) {
            // F10 / F11：換檔位，並「幫你按一次 Alt+N / Alt+M」讓遊戲自己顯示提示（見 inject_alt_key）
            if (s && !p_slow) { speed_step(-1); inject_alt_key('N'); }
            if (f && !p_fast) { speed_step(+1); inject_alt_key('M'); }
            // 真的按下 Alt+N / Alt+M：等同 F10 / F11（遊戲原生的 ±0.5 Hz 會被還原）
            if (g_cfg.alt_nm) {
                if (en && !p_n && !tick_open(g_ignore_alt_until)) { g_ignore_alt_until = GetTickCount() + 300; speed_step(-1); }
                if (em && !p_m && !tick_open(g_ignore_alt_until)) { g_ignore_alt_until = GetTickCount() + 300; speed_step(+1); }
            }
        }
        p_dbg = d; p_slow = s; p_fast = f; p_n = en; p_m = em;

        if (g_cfg.speed_enable) { speed_enforce(); hint_tick(); }
        debug_reassert();
    }
    return 0;
}

} // namespace wcext

// DllMain 呼叫這個；只建立執行緒，不做任何重事（loader lock 下安全）
static void wccext_attach(HINSTANCE hInst) {
    wcext::g_hinst = hInst;
    HANDLE h = CreateThread(NULL, 0, wcext::ext_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

#endif // WC1CHT_EXT_INC
