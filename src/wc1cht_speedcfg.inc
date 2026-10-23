/*
 * wc1cht_speedcfg.inc — [Speed] 設定解析（純邏輯，無 Windows 相依；可在 Linux 單獨測試）
 *
 * 速度的唯一單位是「模擬 CPU 時脈 (MHz)」：遊戲速度 Hz（每秒模擬步數）= MHz × HzPerMHz。
 * 優先序（由高到低）：
 *   1. [Speed] GameHz   >0  直接指定遊戲速度（測試/微調用）
 *   2. [Speed] CpuMHz   有寫：>0 套用；=0 表示「不干涉，維持遊戲預設」
 *   --- 以下是舊 ini 的相容規則（新鍵都沒寫才會用；會在 log 提醒改用新鍵）---
 *   3. 舊 [CpuClock] Enable=1 且 MHz>0
 *   4. 舊 [Speed] FlightFps>0
 *   5. 舊 [Speed] Preset（檔位序號，對應舊 [Speed] Presets 的 FPS 清單）
 *   6. 都沒有 → 不干涉
 */
#ifndef WC1CHT_SPEEDCFG_INC
#define WC1CHT_SPEEDCFG_INC
#include <vector>
#include <string>
#include <cstdio>

namespace wcspeed {

static const float kMinHz = 1.f, kMaxHz = 32.f;      // 32 = 遊戲內建 Alt+M 的上限

struct Input {
    float new_mhz = -1.f;                 // [Speed] CpuMHz；<0 = 沒寫
    float new_game_hz = -1.f;             // [Speed] GameHz；<0 = 沒寫
    int   legacy_cpu_enable = 0;          // 舊 [CpuClock] Enable
    float legacy_mhz = -1.f;              // 舊 [CpuClock] MHz
    float legacy_flight_fps = 0.f;        // 舊 [Speed] FlightFps
    int   legacy_preset = -1;             // 舊 [Speed] Preset（1 起算）
    std::vector<float> legacy_presets;    // 舊 [Speed] Presets（FPS 清單，已排序）
    float hz_per_mhz = 0.25f;
};
struct Output {
    float hz = 0.f;                       // 0 = 不干涉
    float mhz = 0.f;
    bool  clamped = false;
    std::string source;                   // 說明來源，寫進 log
};

static float clamp_hz(float hz, bool* clamped) {
    if (hz < kMinHz) { if (clamped) *clamped = true; return kMinHz; }
    if (hz > kMaxHz) { if (clamped) *clamped = true; return kMaxHz; }
    return hz;
}

static Output resolve(const Input& in) {
    Output o; float hpm = in.hz_per_mhz > 0.01f ? in.hz_per_mhz : 0.25f;
    auto fromHz = [&](float hz, const char* src) { o.hz = clamp_hz(hz, &o.clamped); o.mhz = o.hz / hpm; o.source = src; };
    if (in.new_game_hz > 0.f)                          { fromHz(in.new_game_hz, "GameHz"); return o; }
    if (in.new_mhz > 0.f)                              { fromHz(in.new_mhz * hpm, "CpuMHz"); return o; }
    if (in.new_mhz == 0.f)                             { o.source = "CpuMHz=0（不干涉）"; return o; }
    if (in.legacy_cpu_enable && in.legacy_mhz > 0.f)   { fromHz(in.legacy_mhz * hpm, "舊 [CpuClock] MHz"); return o; }
    if (in.legacy_flight_fps > 0.f)                    { fromHz(in.legacy_flight_fps, "舊 FlightFps"); return o; }
    if (in.legacy_preset >= 1 && in.legacy_preset <= (int)in.legacy_presets.size())
                                                       { fromHz(in.legacy_presets[in.legacy_preset - 1], "舊 Preset"); return o; }
    o.source = "未指定"; return o;
}

} // namespace wcspeed

// ═════════════ F10/F11 與 Alt+N/Alt+M 共用的純邏輯（無 Windows 相依）═════════════
namespace wcspeed {

// 在 MHz 檔位表（由小到大）裡，由目前的 cur 往 dir（-1 慢 / +1 快）挑下一檔；已在端點就回傳 cur
static float step_pick(const std::vector<float>& steps, float cur, int dir) {
    float pick = cur;
    if (dir < 0) { for (auto it = steps.rbegin(); it != steps.rend(); ++it) if (*it < cur - 0.01f) { pick = *it; break; } }
    else         { for (auto it = steps.begin();  it != steps.end();  ++it) if (*it > cur + 0.01f) { pick = *it; break; } }
    return pick;
}
// mhz 是第幾檔（1 起算）；不在檔位表上（例如用 GameHz 指定的值）回傳 0
static int step_index(const std::vector<float>& steps, float mhz) {
    for (size_t i = 0; i < steps.size(); i++) if (steps[i] > mhz - 0.01f && steps[i] < mhz + 0.01f) return (int)i + 1;
    return 0;
}

// 遊戲原生的 Alt+N / Alt+M：新值 = 夾制(舊值 ± 0.5, 8..32)。由「舊值 → 觀察到的新值」反推按的是哪個鍵：
//   +1 = Alt+M（加快）、-1 = Alt+N（減慢）、0 = 無法判斷（舊值 < 8 時兩個方向都被夾成 8.0，或兩邊都不符合）
//   前提：呼叫端只在「變數的值真的變了」才用它（值沒變就根本看不出有人按過鍵）。
static int infer_dir(float old_hz, float have_hz) {
    auto clampf = [](float x) { return x < 8.f ? 8.f : (x > 32.f ? 32.f : x); };
    float up = clampf(old_hz + 0.5f), dn = clampf(old_hz - 0.5f);
    bool mu = have_hz > up - 0.01f && have_hz < up + 0.01f;
    bool md = have_hz > dn - 0.01f && have_hz < dn + 0.01f;
    if (mu && !md) return +1;
    if (md && !mu) return -1;
    return 0;
}

// 提示文字（純 ASCII：遊戲的訊息字型沒有中文；最多 30 字元，緩衝區是遊戲自己的 sprintf 用的）
//   例：CPU 33 MHz [4/8] 8.25 Hz        到端點：CPU 80 MHz [8/8] 20.00 Hz MAX
static std::string format_hint(float mhz, int idx, int n, float hz, int edge /* -1 已在最慢 / +1 已在最快 / 0 */) {
    char b[64];
    bool whole = (mhz - (float)(long)(mhz + 0.5f)) > -0.01f && (mhz - (float)(long)(mhz + 0.5f)) < 0.01f;
    char m[24]; if (whole) snprintf(m, sizeof m, "%.0f", mhz); else snprintf(m, sizeof m, "%.1f", mhz);
    if (idx > 0) snprintf(b, sizeof b, "CPU %s MHz [%d/%d] %.2f Hz%s", m, idx, n, hz, edge > 0 ? " MAX" : (edge < 0 ? " MIN" : ""));
    else         snprintf(b, sizeof b, "CPU %s MHz [-/%d] %.2f Hz%s", m, n, hz, edge > 0 ? " MAX" : (edge < 0 ? " MIN" : ""));
    std::string r(b); if (r.size() > 30) r.resize(30);
    return r;
}

// 從 ADAPT 函式（遊戲處理 Alt+N/M 的函式）的機器碼取出「格式字串」與「訊息緩衝區」位址：
//   +0x62: 68 <fmt>      push 格式字串（"Space Flight Max FPS : %.1f"）
//   +0x67: 68 <buf>      push 訊息緩衝區（sprintf 的輸出，之後遊戲把「指向它的指標」交給訊息顯示）
//   +0x6C: E8 ...        call sprintf
// 位址不寫死：SM1/SM2 的資料位址不同，但函式內容相同，這樣各版本都適用；呼叫端還會再驗證字串內容。
static bool parse_adapt_hint(const unsigned char* code, size_t n, unsigned& fmt, unsigned& buf) {
    if (n < 0x6D || code[0x62] != 0x68 || code[0x67] != 0x68 || code[0x6C] != 0xE8) return false;
    fmt = (unsigned)code[0x63] | ((unsigned)code[0x64] << 8) | ((unsigned)code[0x65] << 16) | ((unsigned)code[0x66] << 24);
    buf = (unsigned)code[0x68] | ((unsigned)code[0x69] << 8) | ((unsigned)code[0x6A] << 16) | ((unsigned)code[0x6B] << 24);
    return fmt != 0 && buf != 0 && fmt != buf;
}

} // namespace wcspeed

#endif
