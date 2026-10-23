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
#endif
