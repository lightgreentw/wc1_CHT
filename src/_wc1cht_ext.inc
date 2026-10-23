/*
 * wc1cht_ext.inc — Wing Commander 1 (WCDX / Kilrathi Saga 版) 支線模組
 *   1. F12 開關 debug 模式（等同原版 "Origin -k -b" / 登錄檔 Cheater）
 *   2. 降速：把遊戲內建的 FPS 限速器壓低，模擬 386 時代的速度（F10 慢 / F11 快）
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
 * 全部經分析並驗證自 wc1_wcdx_cht.exe（ImageBase 0x400000，固定位址），詳見 WC1_debug_speed_branch.md。
 * 啟動時會先比對關鍵位址的機器碼特徵；不符（版本不同）就整個功能停用並寫 log，絕不亂寫記憶體。
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

// ───────────── 遊戲全域變數位址（見文件的位址表）─────────────
// Debug 旗標群（command line "Origin -k -b" 與登錄檔 Cheater 設定的就是這幾個）
static const DWORD A_CHEAT      = 0x004AE5F4;  // "Origin" 參數 / Cheater → 1：作弊總開關
static const DWORD A_RELAX_K    = 0x004AE5FC;  // "-k" → 0（預設 1）
static const DWORD A_RELAX_B    = 0x004AE600;  // "-b" → 0（預設 1）
static const DWORD A_CTRLDEL    = 0x004ADF24;  // 遊戲目錄有 ctrl.del 檔時 =1；Ctrl+Del/Ins 作弊鍵也要求它
// FPS 限速器
static const DWORD A_FRAME_MS   = 0x004B0EA4;  // 每影格毫秒數（int）
static const DWORD A_FRAME_DL   = 0x004B0EA8;  // 下一影格的 timeGetTime 期限（DWORD）；寫 0 = 立刻放行
static const DWORD A_FPS_MODE   = 0x004B0EB4;  // 1 = 太空戰模式，0 = 其他畫面
static const DWORD A_FPS_FLIGHT = 0x004B0EB8;  // float，太空戰 FPS 上限（預設 20.0）
static const DWORD A_FPS_OTHER  = 0x004B0EBC;  // float，其他畫面 FPS 上限（預設 16.0）
// 診斷用
static const DWORD A_TARGET     = 0x00625C70;  // byte，目前鎖定目標（-1 = 無）
static const DWORD A_SHIP_WING  = 0x00626460;  // DWORD[10]
static const DWORD A_SHIP_KIND  = 0x00625F10;  // DWORD[10]

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
static void verify_addresses() {
    // 限速器函式 0x46E0B0：push ebp; mov ebp,esp; sub esp,0Ch
    static const BYTE sig_limiter[] = { 0x55,0x8B,0xEC,0x83,0xEC,0x0C };
    // FPS 調整函式 0x46E20C：push ebp; mov ebp,esp; push ebx; push esi; push edi
    static const BYTE sig_adapt[]   = { 0x55,0x8B,0xEC,0x53,0x56,0x57 };
    // demo 函式 0x4482A0 開頭，以及它讀取作弊旗標的指令 mov eax,[4AE5F4]
    static const BYTE sig_demo[]    = { 0x55,0x8B,0xEC,0x83,0xEC,0x10 };
    static const BYTE sig_demo2[]   = { 0xA1,0xF4,0xE5,0x4A,0x00 };

    bool base_ok = ((DWORD)(uintptr_t)GetModuleHandleA(NULL) == 0x00400000);
    g_addr_ok_speed = base_ok && code_is(0x0046E0B0, sig_limiter, 6) && code_is(0x0046E20C, sig_adapt, 6);
    g_addr_ok_dbg   = base_ok && code_is(0x004482A0, sig_demo, 6)    && code_is(0x004482A9, sig_demo2, 5);

    float f = 0; DWORD ms = 0;
    if (g_addr_ok_speed) {
        if (!rdf(A_FPS_FLIGHT, f) || f < 1.f || f > 200.f) g_addr_ok_speed = false;
        if (!rd32(A_FRAME_MS, ms) || ms > 5000)            g_addr_ok_speed = false;
    }
    WCCEXT_LOG("[支線] 位址驗證：ImageBase=%s  速度模組=%s  Debug 模組=%s",
               base_ok ? "0x400000" : "不符", g_addr_ok_speed ? "可用" : "停用", g_addr_ok_dbg ? "可用" : "停用");
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
static SHORT WINAPI my_gaks(int vk) {
    SHORT r = g_real_gaks(vk);
    if (vk == VK_CONTROL && g_dbg_on && g_cfg.alt_as_ctrl && !(r & 0x8000)) {
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
static volatile int g_render_mult = 1;  // 實驗 v3：每個模擬步畫幾格（由 wc1cht_interp.inc 設定；1 = 不內插）
static float g_fps          = 0.f;     // 我們想要的太空戰 FPS（0 = 還沒決定，不干涉）
static float g_last_written = 0.f;
static float g_game_default = 20.f;

static float g_cpu_mhz = 0.f;      // 目前模擬的 CPU 時脈（MHz）；0 = 尚未指定
static float mhz_to_hz(float mhz) { float hz = mhz * g_cfg.hz_per_mhz; if (hz < 1.f) hz = 1.f; if (hz > 32.f) hz = 32.f; return hz; }
static void beep_for_fps(float fps) { Beep((DWORD)(300 + fps * 40), 60); }

static void speed_step(int dir /* -1 慢, +1 快 */) {
    if (!g_addr_ok_speed) return;
    float cur = g_cpu_mhz > 0 ? g_cpu_mhz : (g_fps > 0 ? g_fps : g_game_default) / g_cfg.hz_per_mhz;
    float pick = cur; const auto& P = g_cfg.mhz_steps;
    if (dir < 0) { for (auto it = P.rbegin(); it != P.rend(); ++it) if (*it < cur - 0.01f) { pick = *it; break; } }
    else         { for (auto it = P.begin();  it != P.end();  ++it) if (*it > cur + 0.01f) { pick = *it; break; } }
    if (pick == cur) { Beep(200, 40); WCCEXT_LOG("[支線] 模擬 CPU 時脈已在端點：%.0f MHz", cur); return; }
    g_cpu_mhz = pick; g_fps = mhz_to_hz(pick);
    WCCEXT_LOG("[支線] 模擬 CPU 時脈 %s → %.0f MHz（遊戲速度 %.2f Hz，每秒模擬步數）", dir < 0 ? "調慢" : "調快", pick, g_fps);
    beep_for_fps(g_fps);
}

static void speed_enforce() {
    if (!g_addr_ok_speed || g_fps <= 0.f) return;
    float have = 0; if (!rdf(A_FPS_FLIGHT, have)) return;
    // 遊戲內建的 Alt+N / Alt+M（±0.5 FPS）改了變數 → 視為使用者的微調，接受它
    if (have != g_last_written && have >= 1.f && have <= 100.f && have != g_fps) {
        WCCEXT_LOG("[支線] 偵測到遊戲內建微調：%.1f → %.1f FPS（接受）", g_fps, have);
        g_fps = have;
        g_cpu_mhz = g_fps / g_cfg.hz_per_mhz;
    }
    if (have != g_fps) wrf(A_FPS_FLIGHT, g_fps);
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
    WCCEXT_LOG("鎖定目標 [0x625C70]=%d", (int)(signed char)tgt);
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
    WCCEXT_LOG("[支線] wc1cht_ext 啟動  Debug 鍵=0x%02X  慢=0x%02X 快=0x%02X  MHz 檔位數=%d",
               g_cfg.vk_debug, g_cfg.vk_slower, g_cfg.vk_faster, (int)g_cfg.mhz_steps.size());
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

    bool p_dbg = false, p_slow = false, p_fast = false;
    for (;;) {
        Sleep(g_cfg.poll_ms);
        bool focus = game_has_focus();
        bool d = focus && key_down(g_cfg.vk_debug);
        bool ctrl = key_down(VK_CONTROL);                // 實驗版：Ctrl+F10 留給觀測器，不當作降速鍵
        bool s = focus && !ctrl && key_down(g_cfg.vk_slower);
        bool f = focus && !ctrl && key_down(g_cfg.vk_faster);

        if (d && !p_dbg && g_cfg.debug_enable) {
            if (key_down(VK_CONTROL)) dump_state();
            else if (!g_addr_ok_dbg) { WCCEXT_ERR("[支線] Debug 模組停用（位址驗證失敗）"); Beep(200, 120); }
            else {
                g_dbg_on = !g_dbg_on; debug_apply(g_dbg_on != 0);
                WCCEXT_LOG("[支線] Debug 模式 %s（F12）", g_dbg_on ? "開啟" : "關閉");
                Beep(g_dbg_on ? 1500 : 700, 90);
            }
        }
        if (s && !p_slow && g_cfg.speed_enable) speed_step(-1);
        if (f && !p_fast && g_cfg.speed_enable) speed_step(+1);
        p_dbg = d; p_slow = s; p_fast = f;

        if (g_cfg.speed_enable) speed_enforce();
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
