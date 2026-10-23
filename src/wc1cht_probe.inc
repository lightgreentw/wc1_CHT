/*
 * wc1cht_probe.inc — 實驗支線 v2：逐格記憶體觀測器（唯讀，不修改遊戲任何記憶體）
 *
 * 目的：為「慢速但順暢（渲染內插）」實驗蒐集資料。靜態分析找不到 3D 物件狀態放在哪，
 *       所以請你在太空戰中錄幾秒，本模組會比對每一格遊戲全域資料（exe 的 .data 區段）的變化，
 *       把「每格都在變」的變數分類（計數器 / 平滑位移 / 其他）寫進 wc1cht_probe.txt，供下一輪分析。
 *
 * 用法：
 *   1. 太空戰中，建議先用 F10 把速度降到 5～8 FPS（間隔越長，取樣越準；本模組在每格等待期的中點取樣）。
 *   2. 進行「固定動作」：例如維持同一個方向持續轉彎（或持續加速直飛）約 5 秒。
 *   3. 按 Ctrl+F10 開始錄製（嗶一聲），錄滿 Frames 格後自動結束（嗶兩聲）並寫出 wc1cht_probe.txt。
 *   4. 換一個動作再錄一次（例如：直飛不轉彎；鎖定敵機後持續追蹤），把 txt 傳給我。
 * 完全唯讀；只有 Enable=1 才會啟動。與基準版 wc1cht_ext.inc 互相獨立。
 *
 * 整合（實驗版 wc1cht.cpp 已加好）：
 *   #include "wc1cht_probe.inc"   （放在 #include "wc1cht_ext.inc" 之後）
 *   wccprobe_attach(hInst);       （放在 wccext_attach(hInst); 之後）
 */
#ifndef WC1CHT_PROBE_INC
#define WC1CHT_PROBE_INC

#include <vector>
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

namespace wcprobe {

// ───────────── 純分析（無 Windows 相依）─────────────
struct Row {
    uint32_t addr;
    int      changed;      // 變化的格數（相鄰兩格不同）
    int      kind;         // 0 其他 1 計數器 2 平滑位移 3 浮點平滑
    long long step;        // 計數器的固定步進
    long long totalDelta;  // 首尾差
};

static bool plausible_float(uint32_t b) {
    float f; memcpy(&f, &b, 4);
    if (!std::isfinite(f)) return false;
    float a = fabsf(f);
    return a == 0.f || (a > 1e-4f && a < 1e9f);
}

// frames[t][k] = 第 t 格、第 k 個 DWORD（位址 = base + 4*k）
static std::vector<Row> analyze(const std::vector<std::vector<uint32_t>>& frames, uint32_t base,
                                FILE* out, int maxRows = 6000, int showVals = 10) {
    std::vector<Row> rows;
    int N = (int)frames.size();
    if (N < 3) return rows;
    size_t K = frames[0].size();
    for (size_t k = 0; k < K; k++) {
        int changed = 0; bool allsame = true;
        long long d0 = 0; int signs[3] = {0,0,0};           // 負/零/正
        std::vector<long long> diffs; diffs.reserve(N);
        for (int t = 1; t < N; t++) {
            uint32_t a = frames[t-1][k], b = frames[t][k];
            long long d = (long long)(int32_t)b - (long long)(int32_t)a;
            if (a != b) changed++;
            diffs.push_back(d);
            signs[d < 0 ? 0 : (d == 0 ? 1 : 2)]++;
        }
        if (changed == 0) continue;
        d0 = diffs[0];
        for (long long d : diffs) if (d != d0) { allsame = false; break; }
        Row r; r.addr = base + (uint32_t)(4 * k); r.changed = changed; r.step = 0;
        r.totalDelta = (long long)(int32_t)frames[N-1][k] - (long long)(int32_t)frames[0][k];
        int nz = (int)diffs.size();
        bool mostlyOneSign = (signs[0] == 0 || signs[2] == 0) && changed >= (nz * 8) / 10;
        if (allsame && d0 != 0 && (d0 == 1 || d0 == -1 || d0 == 2 || d0 == 4)) { r.kind = 1; r.step = d0; }
        else if (mostlyOneSign) {
            bool fl = true; for (int t = 0; t < N; t++) if (!plausible_float(frames[t][k])) { fl = false; break; }
            r.kind = fl ? 3 : 2;
        } else r.kind = 0;
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.kind != b.kind) { // 平滑位移最前，其次浮點，再來計數器，最後其他
            auto rank = [](int k) { return k == 2 ? 0 : (k == 3 ? 1 : (k == 1 ? 2 : 3)); };
            return rank(a.kind) < rank(b.kind);
        }
        if (a.changed != b.changed) return a.changed > b.changed;
        return a.addr < b.addr;
    });
    if (out) {
        int c[4] = {0,0,0,0}; for (auto& r : rows) c[r.kind]++;
        fprintf(out, "# 共 %d 格；變化的 DWORD：平滑整數位移=%d 平滑浮點=%d 計數器=%d 其他=%d\n", N, c[2], c[3], c[1], c[0]);
        fprintf(out, "# 欄位：位址  類型  變化格數  首尾差  前%d格的值（整數 | 若合理則附浮點）\n", showVals);
        int shown = 0;
        for (auto& r : rows) {
            if (shown++ >= maxRows) break;
            static const char* kn[4] = {"其他", "計數器", "平滑整數", "平滑浮點"};
            if (r.kind == 3) {
                size_t kk = (r.addr - base) / 4; float fa, fb; memcpy(&fa, &frames[0][kk], 4); memcpy(&fb, &frames[N-1][kk], 4);
                fprintf(out, "%08X  %-8s  %2d/%d  d=%.4g", r.addr, kn[r.kind], r.changed, N - 1, fb - fa);
            } else
            fprintf(out, "%08X  %-8s  %2d/%d  d=%lld", r.addr, kn[r.kind], r.changed, N - 1, r.totalDelta);
            if (r.kind == 1) fprintf(out, "  步進=%lld", r.step);
            fprintf(out, "  :");
            size_t k = (r.addr - base) / 4;
            for (int t = 0; t < N && t < showVals; t++) {
                uint32_t b = frames[t][k];
                if (r.kind == 3) { float f; memcpy(&f, &b, 4); fprintf(out, " %.4g", f); }
                else fprintf(out, " %d", (int32_t)b);
            }
            fprintf(out, "\n");
        }
    }
    return rows;
}

} // namespace wcprobe

// ───────────── 以下才有 Windows 相依 ─────────────
#ifdef _WIN32
#ifndef WCCPROBE_LOG
  #define WCCPROBE_LOG wclog_force
#endif
#ifndef WCCPROBE_ERR
  #define WCCPROBE_ERR wclog_err
#endif

namespace wcprobe {

static HINSTANCE g_hinst = NULL;
static DWORD P_FRAME_MS = 0x004B0EA4, P_FRAME_DL = 0x004B0EA8, P_FPS_MODE = 0x004B0EB4;   // WC1 值；probe_thread 啟動時依 exe 版本換算

static std::string ini_path() {
    char p[MAX_PATH]; DWORD n = GetModuleFileNameA(g_hinst, p, MAX_PATH);
    if (!n || n >= MAX_PATH) return "wc1cht.ini";
    char* sl = strrchr(p, '\\'); if (sl) sl[1] = 0; return std::string(p);
}

static bool data_range(DWORD& lo, DWORD& hi) {
    BYTE* base = (BYTE*)GetModuleHandleA(NULL); if (!base) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sh++) {
        if (strncmp((const char*)sh->Name, ".data", 5) == 0) {
            lo = (DWORD)(uintptr_t)(base + sh->VirtualAddress);
            hi = lo + (sh->Misc.VirtualSize + 3) / 4 * 4;
            return true;
        }
    }
    return false;
}

typedef DWORD (WINAPI *PFN_TGT)(void);

static DWORD WINAPI probe_thread(LPVOID) {
    Sleep(800);
    P_FRAME_MS = wcprof::sym(wcprof::tbl::S_FRAME_MS); P_FRAME_DL = wcprof::sym(wcprof::tbl::S_FRAME_DL); P_FPS_MODE = wcprof::sym(wcprof::tbl::S_FPS_MODE);
    std::string dir = ini_path();
    std::string ini = dir + "wc1cht.ini";
    int enable = GetPrivateProfileIntA("Probe", "Enable", 0, ini.c_str());
    int nframes = GetPrivateProfileIntA("Probe", "Frames", 40, ini.c_str());
    if (nframes < 8) nframes = 8;
    if (nframes > 120) nframes = 120;
    if (!enable) return 0;
    char kb[64]; GetPrivateProfileStringA("Probe", "Key", "F10", kb, sizeof(kb), ini.c_str());
    int vk = VK_F10; if ((kb[0] == 'F' || kb[0] == 'f') && atoi(kb + 1) >= 1) vk = VK_F1 + atoi(kb + 1) - 1;

    DWORD lo = 0, hi = 0;
    if (!data_range(lo, hi)) { WCCPROBE_ERR("[probe] 找不到 .data 區段，停用"); return 0; }
    HMODULE mm = GetModuleHandleA("winmm.dll"); if (!mm) mm = LoadLibraryA("winmm.dll");
    PFN_TGT tgt = mm ? (PFN_TGT)GetProcAddress(mm, "timeGetTime") : NULL;
    if (!tgt) { WCCPROBE_ERR("[probe] 找不到 timeGetTime，停用"); return 0; }
    size_t K = (hi - lo) / 4;
    WCCPROBE_LOG("[probe] 啟動：.data = 0x%08lX..0x%08lX（%lu 個 DWORD），按 Ctrl+熱鍵 錄 %d 格", (unsigned long)lo, (unsigned long)hi, (unsigned long)K, nframes);

    bool prev = false;
    for (;;) {
        Sleep(15);
        HWND fg = GetForegroundWindow(); DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
        bool focus = pid == GetCurrentProcessId();
        bool d = focus && (GetAsyncKeyState(vk) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        bool start = d && !prev; prev = d;
        if (!start) continue;

        DWORD mode = 0; memcpy(&mode, (void*)(uintptr_t)P_FPS_MODE, 4);
        if (mode != 1) { WCCPROBE_LOG("[probe] 目前不在太空戰迴圈（mode=%lu），忽略", (unsigned long)mode); Beep(200, 120); continue; }
        WCCPROBE_LOG("[probe] 開始錄製 %d 格…", nframes);
        Beep(1200, 80);
        std::vector<std::vector<uint32_t>> frames; frames.reserve(nframes);
        std::vector<DWORD> stamps;
        DWORD lastDL = 0; memcpy(&lastDL, (void*)(uintptr_t)P_FRAME_DL, 4);
        DWORD tStart = tgt(); bool aborted = false;
        while ((int)frames.size() < nframes) {
            Sleep(1);
            if (tgt() - tStart > 120000) { aborted = true; break; }
            DWORD dl = 0, ms = 0; memcpy(&dl, (void*)(uintptr_t)P_FRAME_DL, 4); memcpy(&ms, (void*)(uintptr_t)P_FRAME_MS, 4);
            if (dl == lastDL || dl == 0) continue;               // 還在同一格
            lastDL = dl;                                          // 新的一格剛開始：期限 = 現在 + 間隔
            if (ms < 25 || ms > 1000) { continue; }              // 間隔太短，等待期不夠取樣
            DWORD target = dl - ms / 2;                           // 這格等待期的中點
            while ((int)(target - tgt()) > 0) Sleep(1);
            if ((int)(dl - tgt()) < 3) continue;                  // 來不及（已逼近下一格），放棄這格
            std::vector<uint32_t> snap(K);
            memcpy(snap.data(), (void*)(uintptr_t)lo, K * 4);
            frames.push_back(std::move(snap)); stamps.push_back(tgt());
        }
        if (aborted || frames.size() < 3) { WCCPROBE_LOG("[probe] 錄製失敗/逾時（只取得 %d 格）", (int)frames.size()); Beep(200, 200); continue; }
        SYSTEMTIME st; GetLocalTime(&st);                       // 每次錄製寫成不同檔名，避免覆寫前一份
        char fn[64]; snprintf(fn, sizeof(fn), "wc1cht_probe_%02d%02d%02d.txt", st.wHour, st.wMinute, st.wSecond);
        std::string path = dir + fn;
        FILE* fp = fopen(path.c_str(), "wb");
        if (fp) {
            fprintf(fp, "# wc1cht_probe  .data=0x%08lX..0x%08lX  frames=%d  相鄰格間隔(ms):", (unsigned long)lo, (unsigned long)hi, (int)frames.size());
            for (size_t i = 1; i < stamps.size(); i++) fprintf(fp, " %lu", (unsigned long)(stamps[i] - stamps[i-1]));
            fprintf(fp, "\n");
            analyze(frames, lo, fp);
            fclose(fp);
            WCCPROBE_LOG("[probe] 完成，已寫入 %s", path.c_str());
        } else WCCPROBE_LOG("[probe] 無法寫入 %s", path.c_str());
        Beep(1200, 60); Beep(1500, 60);
    }
    return 0;
}

} // namespace wcprobe

static void wccprobe_attach(HINSTANCE hInst) {
    if (!wcprof::has_game_modules()) { wclog_force("[支線] %s 沒有太空戰迴圈，probe 模組不啟用", wcprof::id_name(wcprof::id())); return; }
    wcprobe::g_hinst = hInst;
    HANDLE h = CreateThread(NULL, 0, wcprobe::probe_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
#endif // _WIN32

#endif // WC1CHT_PROBE_INC
