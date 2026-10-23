/*
 * wc1cht.cpp — Wing Commander 1 Chinese subtitle hook DLL
 *
 * 攔截點：FUN_0044e215 (0x0044E215) 函式本體
 * 由 wc1cht_patch.exe 用 Detour Hook 改寫其開頭 6 bytes 後，
 * 每次遊戲要畫字幕時都會先進入這裡的 hook_draw_string()。
 *
 * === 與 wc1cht_patch.exe 的介面約定 ===
 *   1. wc1cht_patch.exe 在 exe 裡建立一個 trampoline（保留原始 6 bytes
 *      prologue，接一個跳回原函式續接點的 JMP），首次呼叫時透過
 *      GetProcAddress 找到 wc1cht_set_trampoline，呼叫一次把
 *      trampoline 位址告訴這個 DLL
 *   2. 之後每次遊戲要畫字幕，都是直接 JMP 進 hook_draw_string，
 *      堆疊跟原函式收到的完全一樣（tail jump，零污染）
 *   3. hook_draw_string 是 naked 函式：若決定顯示英文，先把堆疊還原成
 *      進入時的樣子，再 JMP 到 trampoline（wc1cht_orig1_addr），
 *      原函式拿到的參數（含可變參數）一個都不少，效果等同沒被 hook
 *
 * 策略：模板比對
 *   1. 從 DAT_00625860 讀出玩家 callsign/name
 *   2. 把 eng_str 裡的動態名字還原成 $C/$N 占位符
 *   3. 查 wc1zh.txt 翻譯表
 *   4. 找到 → 畫中文；找不到 → 轉回原始函式（經 trampoline）
 *
 * 多版本（v-multi）：同一個 DLL 支援 Wing1_wcdx.exe / SM1_wcdx.exe / SM2_wcdx.exe。
 *   遊戲位址一律在 wc1cht_profile.inc（+ 自動產生的 wc1cht_profile_tables.inc）裡依 exe 版本換算；
 *   ini 照 WC1 位址寫即可，也可用 "SM2:0x..." 直接指定某個 exe 專用的原始位址。
 *
 * 編譯（Visual Studio 命令列）：
 *   cl /O2 /LD /Fe:wc1cht.dll wc1cht.cpp
 *
 * 編譯（MSYS2 MINGW32 shell）：
 *   g++ -O2 -shared -static -o wc1cht.dll wc1cht.cpp -Wl,--kill-at
 *   （--kill-at 讓匯出符號名稱不帶 @N 修飾，GetProcAddress 才能用純名字找到）
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cerrno>
#include <cmath>

// ─────────────────────────────────────────────
// 跨編譯器巨集
// ─────────────────────────────────────────────
//
// WC1CHT_ASMLABEL(name)：讓 C++ 符號的組合語言名稱「固定」為 name，
//   不受 MinGW 底線前綴 / @N 裝飾影響，naked 進入點的組合語言才能
//   用 "call wc1cht_hook1_impl" 這種寫法穩定找到它。
//   （MSVC 的 __asm 區塊直接認得 C 符號，不需要這個巨集）
// WC1CHT_USED：防止連結器把「只被組合語言引用」的函式當成沒人用而丟掉。
#if defined(_MSC_VER)
    #define WC1CHT_ASMLABEL(name)
    #define WC1CHT_USED
#elif defined(__GNUC__)
    #define WC1CHT_ASMLABEL(name) __asm__(#name)
    #define WC1CHT_USED           __attribute__((used))
#else
    #error "需要 MSVC 或 GCC/MinGW"
#endif
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <cctype>
#include <map>
#include <algorithm>
#include <string>
#include <vector>
#include <algorithm>

// ─────────────────────────────────────────────
// 0.  執行期 Log（診斷用，寫到跟 wc1cht.dll 同目錄的 wc1cht_debug.log）
// ─────────────────────────────────────────────
//
// 【開關】預設「不記錄」。wc1cht.ini 的 [Log] Enable=1 才會寫入一般診斷 log；
// 例外：載入階段的錯誤（位址表驗證失敗、翻譯表／字型／設定檔載入失敗、Hook3 初始化失敗等）
// 一律寫入 wc1cht_debug.log（用 wclog_err），不受開關影響，方便事後查原因。
//
// 目的：在遊戲實際執行時，能事後檢查以下三件事分別卡在哪：
//   1. DLL 有沒有被載入（DllMain 有沒有跑過）
//   2. dispatcher 有沒有成功呼叫 wc1cht_set_trampoline（hook 是否真的接上）
//   3. hook_draw_string 有沒有持續被呼叫、找不找得到對話字串、
//      翻譯表有沒有命中
//
// 注意：dispatcher 本身（wc1cht_patch.exe 寫進 exe 的組合語言 stub）
// 若在 LoadLibraryA/GetProcAddress 這一步就失敗，wc1cht.dll 根本沒被
// 載入，這裡的 log 自然也不會出現 —— 那種情況請確認 wc1cht.dll 與遊戲同為 32-bit、
// 放在遊戲目錄，以及 wc1cht_patch.exe 是否成功修補。

static char g_log_path[MAX_PATH] = "";

static void wclog_init_path() {
    char dll_dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(GetModuleHandleA("wc1cht.dll"), dll_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        strcpy(g_log_path, "wc1cht_debug.log");
        return;
    }
    char* slash = strrchr(dll_dir, '\\');
    if (slash) slash[1] = 0;
    _snprintf(g_log_path, MAX_PATH, "%swc1cht_debug.log", dll_dir);
}

// 靜音旗標：空戰駕駛艙的 HUD 文字「每一幀」都會重畫，如果每次都 fopen/寫檔，
// 遊戲會被 log 拖慢、log 檔也會暴增。hook 進入點會依呼叫次數決定是否靜音
// （見 wc1cht.ini 的 VerboseCalls）。靜音時 wclog 直接返回；
// 重要事件（新出現的文字、新的 viewport、錯誤）用 wclog_force 強制記錄。
static bool g_log_muted = false;
static bool g_log_enabled = false;   // wc1cht.ini [Log] Enable=1；DllMain 一載入就讀（預設關閉）

static void wclog_v(const char* fmt, va_list args) {
    if (!g_log_path[0]) wclog_init_path();

    FILE* fp = fopen(g_log_path, "a");
    if (!fp) return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(fp, "[%02d:%02d:%02d.%03d] ",
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    vfprintf(fp, fmt, args);
    fprintf(fp, "\n");
    fclose(fp);
}

static void wclog(const char* fmt, ...) {
    if (!g_log_enabled || g_log_muted) return;
    va_list args;
    va_start(args, fmt);
    wclog_v(fmt, args);
    va_end(args);
}

static void wclog_force(const char* fmt, ...) {
    if (!g_log_enabled) return;
    va_list args;
    va_start(args, fmt);
    wclog_v(fmt, args);
    va_end(args);
}

// 載入階段的錯誤：不管 ini 的 [Log] 開關，一律寫入（也不受靜音影響）
static void wclog_err(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    wclog_v(fmt, args);
    va_end(args);
}

// ── 目前處理到哪一步（崩潰記錄器會把它印出來，用來判斷崩潰發生在 hook 裡還是遊戲本體）──
static const char* volatile g_stage = "尚未進入任何 hook";
static char g_stage_text[64] = "";

struct LogMuteGuard {
    bool prev;
    explicit LogMuteGuard(bool mute) : prev(g_log_muted) { g_log_muted = mute; }
    ~LogMuteGuard() { g_log_muted = prev; g_stage = "已從 hook 返回遊戲（崩潰若發生在這之後，就在遊戲本體）"; }
};

// ── 安全的指標探測 ──
// IsBadReadPtr 內部會真的觸發存取違規再被 SEH 吞掉；下面的崩潰記錄器是「向量式」處理器，
// 會比 SEH 先看到這些例外，所以探測期間要告訴它「這是我們自己故意的，不用記」。
static volatile LONG g_in_probe = 0;
static bool wc_is_bad_read(const void* p, UINT_PTR n) {
    InterlockedIncrement(&g_in_probe);
    BOOL r = IsBadReadPtr(p, n);
    InterlockedDecrement(&g_in_probe);
    return r != 0;
}
#define IsBadReadPtr(p, n) wc_is_bad_read((p), (n))

// ── 崩潰記錄器（向量式例外處理器）──
// 遊戲崩潰時 log 只會突然中斷、沒有任何線索。這個處理器在例外發生的當下，
// 把「例外種類、出事的指令位址（模組+偏移）、暫存器、堆疊上疑似返回位址」寫進 log，
// 只用 Win32 API 直接寫檔（不 malloc、不用 stdio），堆積區壞掉時也寫得出來。
// 它只記錄、不處理，最後仍交還給系統（遊戲該怎麼崩潰還是怎麼崩潰）。
static LONG g_veh_count = 0;

static void veh_write(const char* s) {
    HANDLE h = CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, s, (DWORD)lstrlenA(s), &w, NULL);
    CloseHandle(h);
}

static void veh_describe_addr(DWORD addr, char* out, int cap) {
    HMODULE hm = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &hm) && hm) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(hm, full, MAX_PATH)) {
            const char* sl = strrchr(full, '\\');
            lstrcpynA(name, sl ? sl + 1 : full, MAX_PATH);
        }
        wsprintfA(out, "%08X (%s+0x%X)", addr, name, (unsigned)(addr - (DWORD)(uintptr_t)hm));
    } else {
        wsprintfA(out, "%08X (不屬於任何已載入模組)", addr);
    }
    (void)cap;
}

static LONG CALLBACK wc_veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                 code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                 code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_IN_PAGE_ERROR ||
                 code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
    if (!fatal || g_in_probe > 0) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_veh_count) > 30) return EXCEPTION_CONTINUE_SEARCH;

    const CONTEXT* c = ep->ContextRecord;
    char buf[1024], where[160], line[160];
    SYSTEMTIME t; GetLocalTime(&t);

    veh_describe_addr((DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    wsprintfA(buf, "[%02d:%02d:%02d.%03d] !!!!!! 例外 #%d  代碼=0x%08X  出事指令位址=%s\r\n",
              t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, (int)g_veh_count, (unsigned)code, where);
    veh_write(buf);

    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
        ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        wsprintfA(buf, "    存取種類=%s  目標位址=%08X\r\n",
                  kind == 0 ? "讀取" : kind == 1 ? "寫入" : "執行(DEP)",
                  (unsigned)ep->ExceptionRecord->ExceptionInformation[1]);
        veh_write(buf);
    }
    wsprintfA(buf, "    EAX=%08X EBX=%08X ECX=%08X EDX=%08X\r\n    ESI=%08X EDI=%08X EBP=%08X ESP=%08X\r\n",
              (unsigned)c->Eax, (unsigned)c->Ebx, (unsigned)c->Ecx, (unsigned)c->Edx,
              (unsigned)c->Esi, (unsigned)c->Edi, (unsigned)c->Ebp, (unsigned)c->Esp);
    veh_write(buf);

    wsprintfA(buf, "    hook 階段=%s  文字=\"%s\"\r\n", g_stage, g_stage_text);
    veh_write(buf);

    // 出事位址的前 16 個 byte
    if (!IsBadReadPtr((const void*)(uintptr_t)c->Eip, 16)) {
        const unsigned char* b = (const unsigned char*)(uintptr_t)c->Eip;
        wsprintfA(buf, "    EIP 處機器碼: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
        veh_write(buf);
    }
    // 堆疊上前 24 個 DWORD；落在 exe 程式碼範圍的標 * （通常是返回位址）
    DWORD exe_base = (DWORD)(uintptr_t)GetModuleHandleA(NULL);
    if (!IsBadReadPtr((const void*)(uintptr_t)c->Esp, 24 * 4)) {
        const DWORD* sp = (const DWORD*)(uintptr_t)c->Esp;
        veh_write("    堆疊（* = 疑似返回位址）:\r\n");
        for (int i = 0; i < 24; i += 4) {
            line[0] = 0;
            for (int j = 0; j < 4; j++) {
                DWORD v = sp[i + j];
                bool ret = (v >= exe_base + 0x1000 && v < exe_base + 0x260000);
                char cell[24];
                wsprintfA(cell, " %08X%c", v, ret ? '*' : ' ');
                lstrcatA(line, cell);
            }
            lstrcatA(line, "\r\n");
            veh_write(line);
        }
    }
    veh_write("    （這是第一手例外，之後遊戲可能自己處理或直接崩潰。若它是最後一筆，就是崩潰位置）\r\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

// ─────────────────────────────────────────────
// 0.5 多版本位址表（WC1 / SM1 / SM2）
// ─────────────────────────────────────────────
#define WCPROF_LOG wclog_force
#include "wc1cht_profile.inc"

// ─────────────────────────────────────────────
// 1.  遊戲內部記憶體位址（靜態分析結果）
// ─────────────────────────────────────────────

// FontContext 全域指標的位址
// PlayerRecord 指標的位址
//
// 預設是 WC1 的位址；DllMain 會呼叫 apply_profile() 依實際 exe 版本換成 SM1 / SM2 的位址。
static DWORD ADDR_FONT_CTX   = 0x0062264C;
static DWORD ADDR_PLAYER_REC = 0x00625860;

// 注意：原本這裡有 ADDR_ORIG_DRAW = 0x0044E215 直接指向原函式，
// 但那個位址現在已經被 wc1cht_patch.exe 改寫成跳到 dispatcher，
// 直接呼叫會變成呼叫自己（無窮遞迴）。
// 正確的「呼叫原始英文渲染邏輯」入口，改由 wc1cht_set_trampoline()
// 在執行期動態設定到 g_orig_draw，見下方全域變數區。

// ─────────────────────────────────────────────
// 2.  Viewport 結構（經靜態分析與動態驗證確認，
//     並非猜測 —— 這是修正「畫面拖影破圖」的關鍵）
// ─────────────────────────────────────────────
//
// 最初版本誤以為 pixels 是一塊單純的「row * 320 + col」線性排列
// buffer，但實際上每一列的記憶體位置是透過 rowOffsets 這張表
// 查出來的，不是固定間距！用錯誤假設寫像素會讓資料疊寫到
// 不相干的列，造成畫面上的橫向拖影/破圖。
//
// 正確定址方式：
//   BYTE* row_ptr = viewport->pixels + viewport->rowOffsets[row];
//   row_ptr[col] = color;

#pragma pack(push, 1)
struct Viewport {
    BYTE*   pixels;        // +0x00  像素資料基底
    WORD*   rowOffsets;    // +0x04  每一列在 pixels 裡的位移表（關鍵！）
    short   left;          // +0x08
    short   top;           // +0x0A
    short   right;         // +0x0C
    short   bottom;        // +0x0E
    BYTE*   allocation;    // +0x10
};

struct FontContext {
    Viewport* viewport;    // +0x00
    short  cursor_x;       // +0x04
    short  cursor_y;       // +0x06
    void*  font_data;      // +0x08  FONTS.FNT resource base
    BYTE   colour;         // +0x0C  前景色 palette index
    BYTE   backgroundColour; // +0x0D
    void*  out_buf_ptr;    // +0x0E
    void*  out_cur_ptr;    // +0x12
    BYTE   align_mode;     // +0x16  0=靠左, 2=置中
};

struct PlayerRecord {
    char   last_name[14]; // +0x00
    char   callsign[14];  // +0x0E
    short  rank_id;       // +0x1C  (approx)
    short  mission_count; // +0x1E
    short  kills;         // +0x20 / 0x22
};
#pragma pack(pop)

// ─────────────────────────────────────────────
// 3.  中文點陣字型格式 (wc1cht.fnt，CZHT 格式)
// ─────────────────────────────────────────────
//
// === 改用 bdf_to_czht_fnt.py 產生的 CZHT 格式，不再是自訂的 WLCK 格式 ===
//
// 檔頭（8 bytes）：
//   4 bytes  magic    "CZHT"
//   1 byte   0x00
//   1 byte   0x01     格式版本
//   2 bytes  count    字形筆數（little-endian uint16）
//
// 每筆記錄（2 + N*bpr bytes）：
//   2 bytes  code     Unicode 碼點（little-endian uint16）
//   N*bpr bytes  bitmap  1bpp，逐列儲存，MSB = 最左像素，
//                        bpr = ceil(N/8) bytes/列，共 N 列
//
// N（字格大小）不會直接寫在檔案裡，改用「檔案總大小」跟「count」
// 反推：record_size = (file_size - 8) / count，
//       record_size = 2 + N*ceil(N/8)，
// 對照幾個常見的 N 值（8/10/12/14/16/20/24/32...）找出唯一解。
// bdf_to_czht_fnt.py 預設 N=16，這是最常見的情況。

// 字形資料本體（可被多個 ChtFont 共用：同一個檔案、同樣的解讀方式只載入一份）
struct GlyphSet {
    std::vector<BYTE> pool;                   // 所有字形的逐像素資料（1=前景），每字 w*h bytes
    std::unordered_map<DWORD, size_t> index;  // Unicode → pool 內的位移
};

// 一套「使用設定」：字型資料 + 這個場景/hook 專屬的間距與修正
// 總共有 4 套：[場景: 對話/空戰][hook: Hook1/Hook2]
struct ChtFont {
    std::string path;                         // 實際載入的檔案路徑
    int  w = 16, h = 16;                      // 字格寬、高（像素）
    int  bpr = 2;                             // 檔案裡每列的 byte 數 = ceil(w/8)
    int  char_spacing = 0;                    // 字與字之間額外間距（像素）
    int  line_spacing = 0;                    // 行與行之間額外間距（像素）
    int  y_adjust = 0;                        // 單行標籤垂直修正（僅 Hook1 的格式化標籤用）
    int  y_offset = 0;                        // 這套字型的垂直位移（像素，正=往下）：字形在字格內偏上/偏下時用
    bool loaded = false;
    std::shared_ptr<GlyphSet> gs;

    const BYTE* find(DWORD cp) const {
        if (!gs) return nullptr;
        auto it = gs->index.find(cp);
        return it == gs->index.end() ? nullptr : gs->pool.data() + it->second;
    }
    int advance_x() const { return w + char_spacing; }
    int advance_y() const { return h + line_spacing; }
};

// 場景：對話（簡報室、機庫、酒吧、對話字幕...）vs 空戰（駕駛艙內）
enum { SCENE_DIALOGUE = 0, SCENE_COMBAT = 1 };
enum { HOOK_1 = 0, HOOK_2 = 1 };
static const char* const SCENE_NAME[2] = { "對話", "空戰" };

// ─────────────────────────────────────────────
// 4.  全域資料
// ─────────────────────────────────────────────

// 四套字型：g_font[場景][hook]
//   hook 0 = Hook1（DrawFormattedText：星圖標籤、HUD 標籤等「%X%Y%F%s」格式化文字）
//   hook 1 = Hook2（DrawTextString：對話字幕等整段文字）
static ChtFont g_font[2][2];

// 場景判定規則（wc1cht.ini 的 [Scene]）
static int g_combat_max_width = 100;          // viewport 寬度 <= 此值 → 空戰；0 = 停用
static std::vector<DWORD> g_combat_vps;       // 強制判為「空戰」的 viewport 位址
static std::vector<DWORD> g_dialogue_vps;     // 強制判為「對話」的 viewport 位址
static long g_verbose_limit = 500;            // 每個 hook 前 N 次呼叫完整記錄，之後靜音；0=不靜音
struct VpRect { short l, t, r, b; };

// ── 規則（Rule）：依「畫面上的位置/呼叫端」決定用哪一套字型 ──
// 一個 Rule = 一組比對條件（全部要符合）+ 要用的字型（Use）+ 放不下時依序改用的較小字型（Shrink）。
// 條件的來源（全都是 hook 被呼叫當下就能取得的）：
//   Hook      是 Hook1（格式化標籤）還是 Hook2（整段文字）
//   Viewport  文字畫進哪個 viewport 結構（遊戲全域的 viewport 位址固定，例如左/右 MFD、字幕區）
//   Rect/Inside/Width/Height  viewport 的範圍與大小
//   Cursor    文字起點（%X/%Y 或游標）落在哪個範圍（用來分開「同一個全螢幕 viewport 裡的不同區塊」）
//   Caller    誰呼叫的：hook 被呼叫時堆疊上的返回位址（往上追八層，任何一層符合即可）。不同畫面通常有各自的繪製程式碼
//   Text      英文原文（已把玩家名字還原成 $C/$N）符合的萬用字元樣式
struct Rule {
    std::string name;
    int  priority = 0;
    int  hook = 0;                                    // 0=不限 1=Hook1 2=Hook2
    bool has_rect = false, has_inside = false, has_cursor = false;
    VpRect rect = {0,0,0,0}, inside = {0,0,0,0}, cursor = {0,0,0,0};
    int  wmin = 0, wmax = 100000, hmin = 0, hmax = 100000;
    std::vector<DWORD> viewports;                     // Viewport=0x... 清單
    std::vector<std::pair<DWORD, DWORD> > callers;    // Caller=範圍清單
    std::vector<std::string> texts;                   // Text=樣式清單
    std::vector<std::string> scene_names;             // Scene=名稱（對應 [SceneDrivers]）
    std::vector<std::pair<DWORD, DWORD> > scene_addrs; // 載入後展開成「場景驅動函式」位址範圍
    bool skip = false;                                // Skip=1：這類文字不翻譯、不記錄，原樣交給遊戲
    int  align = -1;                                  // Align=：-1=沿用 DefaultAlign、0=靠左 1=置中 2=靠右 3=auto（依遊戲的 FontContext.align_mode）
    int  terms = -1;                                  // Terms=：-1=沿用全域 TermFallback、0=關、1=開
    int  yoffset = 0;                                 // YOffset=：這條規則的文字整體垂直位移（像素，正=往下）
    std::string use_name;
    std::vector<std::string> shrink_names;
    const ChtFont* use = nullptr;                     // 載入後解析
    std::vector<const ChtFont*> shrink;
    volatile long hits = 0;
};
static std::vector<VpRect> g_combat_rects;    // 強制判為「空戰」的 viewport 範圍（暫時性的 viewport 位址會變，只能比範圍）
static bool g_dump_missed = false;            // 把沒翻譯的文字寫進 wc1cht_missed.tsv
static bool g_clip_guard = true;              // 擋下「會畫到 viewport 外面太多」的英文原文（見 process_dialog_text）
// 預設對齊（沒有規則、或規則沒寫 Align 時）。0=靠左 3=auto。
// 由 log（392 筆情境）驗證：遊戲在畫每一段文字前都會設定 FontContext.align_mode，
// 置中的文字（字幕、選單提示、對話框、面板標題、MFD 標題）= 2，靠左的（Sector:/System:、Hook1 的標籤）= 0，
// 所以 auto 可以「每一段文字各自」還原原版的對齊方式，不用手動分。
static int g_default_align = 3;
static bool g_term_fallback = true;           // 整句查不到時，把句中已有翻譯的名稱/詞彙單獨換成中文（例如 "Talk to HUNTER." → "Talk to 獵人."）
static int  g_term_max_len = 40;              // 只對不超過這個長度的字串做詞彙替換（太長的句子換幾個詞只會變成中英夾雜）

// 字型集：名稱（小寫）→ 字型設定。來源有兩種：
//   1. 舊版四個區段 Dialogue_Hook1 / Dialogue_Hook2 / Combat_Hook1 / Combat_Hook2（自動登記）
//   2. 新的 [Font.名稱] 區段
// 規則（Rule）用名稱引用它們。用 map 是因為節點位址穩定，Rule 可以直接存指標。
static std::map<std::string, ChtFont> g_fontsets;
static std::vector<Rule> g_rules;

// 場景驅動函式表（[SceneDrivers]）：名稱 → 位址清單。
// 由 log 分析得出：每個場景（酒吧、簡報室、任務報告室…）最上層都有各自的主函式，
// 畫面上任何文字被畫出來時，它的呼叫鏈（八層內）都會經過那個函式，
// 所以「呼叫鏈裡有沒有 0x0045A99D」就等於「現在是不是在酒吧」。
static std::vector<std::pair<std::string, std::vector<std::pair<DWORD, DWORD> > > > g_scene_drivers;
static bool g_log_contexts = true;            // 記錄「新的呼叫情境」（hook/呼叫端/viewport/游標），用來設計規則

// 目前這次 hook 呼叫的呼叫端返回位址（往上追八層；追不到的是 0）
// 用 log 分析的結果：字幕/選單這類文字，前三層常常只是共用的繪字函式，
// 真正能分出「是哪個場景」的函式可能在更上層，所以追深一點。
static const int CALLER_DEPTH = 8;
static DWORD g_ret_chain[CALLER_DEPTH] = {0};

// 使用者按「標記鍵」(預設 F9) 的次數。每按一次就視為「進入新畫面」，
// 之後出現的呼叫情境都會重新記一次，並在 log 中標上這個編號。
static volatile LONG g_mark_id = 0;
static int  g_mark_vk = 0;
static HANDLE g_mark_thread = NULL;

// 玩家動態名稱（每次進入 hook 時更新）
static char g_callsign[32] = "";
static char g_lastname[32] = "";

// 是否已初始化
static bool g_initialized = false;

// 統計用（log 摘要，避免每次呼叫都寫檔洗爆硬碟）
static long g_call_count       = 0;
static long g_scan_miss_count  = 0;   // 堆疊掃描找不到字串
static long g_trans_hit_count  = 0;   // 翻譯表命中
static long g_trans_miss_count = 0;   // 翻譯表沒命中（回退英文）

// 原始函式的 trampoline 位址存在 wc1cht_orig1_addr / wc1cht_orig2_addr
// （定義在 10b 節）。退回英文一律由 naked 進入點 tail JMP 完成，
// C++ 這邊不再「呼叫」原始函式。

// ─────────────────────────────────────────────
// 5.  字串工具
// ─────────────────────────────────────────────

// 把字串裡的 old_sub 全部替換為 new_sub
static std::string str_replace(const std::string& src,
                               const std::string& old_sub,
                               const std::string& new_sub) {
    if (old_sub.empty()) return src;
    std::string result;
    result.reserve(src.size());
    size_t pos = 0, found;
    while ((found = src.find(old_sub, pos)) != std::string::npos) {
        result.append(src, pos, found - pos);
        result += new_sub;
        pos = found + old_sub.size();
    }
    result.append(src, pos, std::string::npos);
    return result;
}

// 把 eng_str 中的 callsign/lastname 還原成 $C/$N，得到模板字串
static std::string make_template_key(const char* eng_str) {
    std::string s(eng_str);
    if (g_callsign[0]) s = str_replace(s, g_callsign, "$C");
    if (g_lastname[0]) s = str_replace(s, g_lastname, "$N");
    return s;
}

// 把翻譯字串裡的 $C/$N 替換回玩家名稱
static std::string apply_variables(const std::string& trans) {
    std::string s = trans;
    if (g_callsign[0]) s = str_replace(s, "$C", g_callsign);
    if (g_lastname[0]) s = str_replace(s, "$N", g_lastname);
    return s;
}

// UTF-8 → Unicode codepoint，並前移指標；出錯回傳 0xFFFD
static DWORD utf8_next(const char*& p) {
    unsigned char c = (unsigned char)*p;
    if (c == 0)          return 0;
    if (c < 0x80)        { p++; return c; }
    if (c < 0xC0)        { p++; return 0xFFFD; }
    if (c < 0xE0) {
        if ((p[1] & 0xC0) != 0x80) { p++; return 0xFFFD; }
        DWORD cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        p += 2; return cp;
    }
    if (c < 0xF0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
            { p++; return 0xFFFD; }
        DWORD cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3; return cp;
    }
    p++; return 0xFFFD;
}

// ─────────────────────────────────────────────
// 6.  載入翻譯表 (wc1zh.txt)
// ─────────────────────────────────────────────
//
// 格式（UTF-8，每行一條，欄位用 TAB 分隔）：
//   英文模板\t中文翻譯
//
// 英文模板裡已用 $C/$N 代替動態名稱。
// 中文翻譯裡的 $C/$N 在顯示時會被替換回玩家名稱。
//
// 行首 # 為注釋。

// ── 跳脫字元 ──
// 翻譯表一行一條，所以多行文字要用 \n 表示（\t = Tab，\\ = 反斜線）。
// 英文 key 與中文翻譯兩邊都會還原。
static std::string unescape_field(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            if (c == 'n')  { o += '\n'; i++; continue; }
            if (c == 't')  { o += '\t'; i++; continue; }
            if (c == '\\') { o += '\\'; i++; continue; }
        }
        o += s[i];
    }
    return o;
}

static std::string escape_field(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\\') o += "\\\\";
        else if (c == '\r') {}
        else o += c;
    }
    return o;
}

// ── 翻譯表（載入、查表、模板比對、詞彙補強；支援純文字與雜湊兩種翻譯檔格式）──
#include "wc1cht_trans.inc"

// ─────────────────────────────────────────────
// 7.  載入中文點陣字型 (wc1cht.fnt，CZHT 格式)
// ─────────────────────────────────────────────
//
// 改用 bdf_to_czht_fnt.py 產生的 CZHT 格式（詳見上方「3. 中文點陣
// 字型格式」的完整規格說明），字碼欄位維持 Unicode（對應
// --target-charset unicode 產生的檔案）。

static const char CZHT_MAGIC[4] = { 'C', 'Z', 'H', 'T' };

// 從「檔案總大小」跟「count」反推字格尺寸。
//   record_size = 2(code) + h * ceil(w/8)
// 優先找「正方形」（8/10/12/14/16/...），因為中文點陣字幾乎都是正方形；
// 找不到才回傳 false，由呼叫端要求使用者在 wc1cht.ini 明確指定 GlyphWidth/GlyphHeight。
static bool detect_glyph_dims(long file_size, WORD count, int& w, int& h) {
    if (count == 0) return false;
    long payload = file_size - 8;
    if (payload <= 0 || payload % count != 0) return false;
    long record = payload / count;

    for (int n = 4; n <= 64; n++) {            // record 大小隨 n 嚴格遞增，所以每個正方形尺寸都不會重複
        if (2 + (long)n * ((n + 7) / 8) == record) { w = h = n; return true; }
    }
    return false;
}

// 一列的原始 bytes → 逐像素（1=前景）。
//   left_aligned=true ：MSB 是最左像素，多出來的 bit 在最右邊（標準做法）
//   left_aligned=false：資料「靠右對齊」，多出來的 bit 在最左邊
//                       （有些轉檔工具對非 8 的倍數寬度（例如 12）會這樣存）
static void unpack_row(const BYTE* row_bytes, int bpr, int width, bool left_aligned, BYTE* out) {
    unsigned long long v = 0;
    for (int i = 0; i < bpr; i++) v = (v << 8) | row_bytes[i];
    int total_bits = bpr * 8;
    for (int c = 0; c < width; c++) {
        int shift = left_aligned ? (total_bits - 1 - c) : (width - 1 - c);
        out[c] = (BYTE)((v >> shift) & 1);
    }
}

// 把一個字形用 ASCII art 印進 log，方便從 log 直接看字型檔內容對不對
static void log_glyph_art(const ChtFont& f, DWORD cp, const char* tag) {
    const BYTE* g = f.find(cp);
    if (!g) return;
    wclog("[%s] 字形預覽 U+%04X（%dx%d）:", tag, cp, f.w, f.h);
    std::string line;
    for (int y = 0; y < f.h; y++) {
        line.assign("    |");
        for (int x = 0; x < f.w; x++) line += g[y * f.w + x] ? '#' : '.';
        line += '|';
        wclog("%s", line.c_str());
    }
}

// align_opt: 0=自動判斷  1=強制左對齊  2=強制右對齊
// want_w/want_h: 0 = 自動偵測
static bool load_cht_font(ChtFont& font, const char* path, int want_w, int want_h,
                          int align_opt, const char* tag, int win_x = 0, int win_y = 0) {
    font.loaded = false;
    font.gs.reset();
    font.path = path;

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        wclog_err("[%s] load_cht_font: 無法開啟 %s (errno=%d)", tag, path, errno);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    rewind(fp);

    char magic[4];
    BYTE zero_byte, version_byte;
    WORD count;
    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, CZHT_MAGIC, 4) != 0) {
        wclog_err("[%s] load_cht_font: %s 不是有效的 CZHT 檔案（開頭 4 bytes 應為 'CZHT'）", tag, path);
        fclose(fp); return false;
    }
    if (fread(&zero_byte, 1, 1, fp) != 1 || fread(&version_byte, 1, 1, fp) != 1 ||
        fread(&count, 2, 1, fp) != 1) { fclose(fp); return false; }
    if (version_byte != 0x01)
        wclog_err("[%s] load_cht_font: 版本號是 0x%02X，預期 0x01，嘗試繼續讀取", tag, version_byte);

    // ── 決定字格尺寸 ──
    // 兩組尺寸要分清楚：
    //   sw × sh = 檔案裡每個字形「實際儲存」的尺寸（由檔案大小自動推測；推測不出來才採用 ini 的值）
    //   w  × h  = 「畫在畫面上」的尺寸（ini 的 GlyphWidth / GlyphHeight）。可以跟儲存尺寸不同：
    //             小於 → 只取字格的一個視窗（裁掉多餘的邊）；大於 → 多出來的部分補空白。
    //             視窗左上角位置用 GlyphOffsetX / GlyphOffsetY 指定（預設 0,0；可為負數）
    int det_w = 0, det_h = 0;
    bool detected = detect_glyph_dims(file_size, count, det_w, det_h);
    const long record = (count > 0 && (file_size - 8) % count == 0) ? (file_size - 8) / count : 0;
    int w = want_w, h = want_h, sw, sh;
    if (w == 0 && h == 0) {
        if (!detected) {
            wclog_err("[%s] load_cht_font: 無法從檔案大小(%ld)與字數(%u)推出字格尺寸，"
                  "請在 wc1cht.ini 指定 GlyphWidth / GlyphHeight", tag, file_size, count);
            fclose(fp); return false;
        }
        w = sw = det_w; h = sh = det_h;
    } else {
        if (w == 0) w = detected ? det_w : h;           // 只填一個：另一個沿用檔案的儲存尺寸
        if (h == 0) h = detected ? det_h : w;
        if (record == 2 + (long)h * ((w + 7) / 8))      { sw = w; sh = h; }          // ini 的值剛好就是檔案的儲存尺寸（含非正方形字格）
        else if (detected)                              { sw = det_w; sh = det_h; }  // 否則把 ini 的值當成「顯示視窗」
        else                                            { sw = w; sh = h; }          // 兩邊都對不上：沿用舊行為，下面會報錯
    }
    if (w < 1 || h < 1 || w > 64 || h > 64) {
        wclog_err("[%s] load_cht_font: 顯示字格 %dx%d 超出允許範圍（1~64），放棄載入", tag, w, h);
        fclose(fp); return false;
    }
    const bool windowed = (w != sw || h != sh || win_x != 0 || win_y != 0);
    int bpr = (sw + 7) / 8;
    long expected_payload = (long)count * (2 + (long)sh * bpr);
    wclog("[%s] load_cht_font: %s  count=%u  檔案大小=%ld  檔案儲存字格=%dx%d (每列 %d bytes)  畫面字格=%dx%d%s%s",
          tag, path, count, file_size, sw, sh, bpr, w, h,
          detected ? "" : "  （檔案大小無法自動推測，採用 ini 指定值）",
          windowed ? "  ← 取視窗（裁切／補白）" : "");
    if (windowed) wclog("[%s] 視窗左上角 = 字格內 (%d,%d)：畫面第 0 列 = 字格第 %d 列，畫面第 0 欄 = 字格第 %d 欄（超出字格的部分補空白）",
                        tag, win_x, win_y, win_y, win_x);
    if (detected && record != 2 + (long)sh * bpr)
        wclog("[%s] 注意：依檔案大小推測字格應為 %dx%d，但目前使用 %dx%d", tag, det_w, det_h, sw, sh);

    if (file_size - 8 < expected_payload) {
        wclog_err("[%s] load_cht_font: 檔案太小！以 %dx%d 計算需要 %ld bytes，實際只有 %ld bytes。"
              "字格尺寸設定錯誤（或檔案損毀），為避免畫出亂碼，放棄載入此字型",
              tag, sw, sh, expected_payload + 8, file_size);
        fclose(fp); return false;
    }
    if (file_size - 8 > expected_payload)
        wclog_err("[%s] 警告：檔案比預期多 %ld bytes，字格尺寸可能設定錯誤", tag, file_size - 8 - expected_payload);

    // ── 一次讀入原始資料（為了先統計對齊方式，再展開像素）──
    std::vector<BYTE> raw((size_t)expected_payload);
    if (fread(raw.data(), 1, raw.size(), fp) != raw.size()) {
        wclog_err("[%s] load_cht_font: 讀取點陣資料失敗，檔案可能截斷", tag);
        fclose(fp); return false;
    }
    fclose(fp);
    const size_t rec_size = 2 + (size_t)sh * bpr;

    // ── 決定對齊方式 ──
    // 寬度是 8 的倍數時沒有對齊問題。否則：
    //   左對齊資料：最右邊的 pad 個 bit 永遠是 0
    //   右對齊資料：最左邊的 pad 個 bit 永遠是 0
    bool left_aligned = true;
    int pad = bpr * 8 - sw;
    if (pad > 0) {
        bool low_used = false, high_used = false;
        const BYTE high_mask = (BYTE)(0xFF << (8 - pad));      // 第一個 byte 的最高 pad 個 bit
        const BYTE low_mask  = (BYTE)((1 << pad) - 1);          // 最後一個 byte 的最低 pad 個 bit
        for (int i = 0; i < count; i++) {
            const BYTE* bm = &raw[i * rec_size + 2];
            for (int r = 0; r < sh; r++) {
                const BYTE* row = bm + (size_t)r * bpr;
                if (row[0] & high_mask)       high_used = true;
                if (row[bpr - 1] & low_mask)  low_used = true;
            }
        }
        const char* why;
        if (align_opt == 1)      { left_aligned = true;  why = "ini 強制左對齊"; }
        else if (align_opt == 2) { left_aligned = false; why = "ini 強制右對齊"; }
        else if (!low_used && high_used)  { left_aligned = true;  why = "自動判斷：右側補齊 bit 全為 0 → 左對齊"; }
        else if (low_used && !high_used)  { left_aligned = false; why = "自動判斷：左側補齊 bit 全為 0 → 右對齊"; }
        else { left_aligned = true; why = "無法判斷（資料可能是空的或 bit 順序不同），預設左對齊"; }
        wclog("[%s] load_cht_font: 每列有 %d 個補齊 bit  左側有資料=%s 右側有資料=%s → %s",
              tag, pad, high_used ? "是" : "否", low_used ? "是" : "否", why);
    }

    // ── 展開成逐像素（並取出顯示視窗）──
    font.w = w; font.h = h; font.bpr = bpr;
    auto gs = std::make_shared<GlyphSet>();
    gs->pool.assign((size_t)count * w * h, 0);
    std::vector<BYTE> cell((size_t)sw * sh);
    for (int i = 0; i < count; i++) {
        const BYTE* rec = &raw[i * rec_size];
        DWORD code = rec[0] | (rec[1] << 8);
        BYTE* dst = &gs->pool[(size_t)i * w * h];
        for (int r = 0; r < sh; r++)
            unpack_row(rec + 2 + (size_t)r * bpr, bpr, sw, left_aligned, &cell[(size_t)r * sw]);
        for (int r = 0; r < h; r++) {
            int sr = r + win_y; if (sr < 0 || sr >= sh) continue;
            for (int c = 0; c < w; c++) {
                int sc = c + win_x; if (sc < 0 || sc >= sw) continue;
                dst[(size_t)r * w + c] = cell[(size_t)sr * sw + sc];
            }
        }
        gs->index[code] = (size_t)i * w * h;
    }
    font.gs = gs;
    font.loaded = !gs->index.empty();
    wclog("[%s] load_cht_font: 成功載入 %d 個字形（畫面字格 %dx%d）", tag, (int)gs->index.size(), w, h);

    // 印兩個樣本字形，直接從 log 就能確認字型內容有沒有讀對
    log_glyph_art(font, 0x9EDE, tag);   // 點
    log_glyph_art(font, 0x722A, tag);   // 爪
    return font.loaded;
}

// ─────────────────────────────────────────────
// 7b. 設定檔 wc1cht.ini
// ─────────────────────────────────────────────
//
// 放在 wc1cht.dll 同一個資料夾。不存在時會自動產生一份預設檔（內含說明）。
// 修改後需要重新啟動遊戲才會生效。

static std::string g_dll_dir;          // DLL 所在目錄（結尾含 '\')
static std::string g_trans_file = "wc1zh.txt";
static std::string g_source_file = "wc1zh.txt";   // [Log] SourceFile：含英文的純文字翻譯檔（ShowSource=1 時，只用來在 log 顯示原文）
static uint64_t g_dbg_hash = 0;                   // 最近一次精確查表命中的雜湊（除錯 log 用）

static std::string join_dir(const std::string& name) {
    if (name.size() > 1 && (name[1] == ':' || name[0] == '\\' || name[0] == '/')) return name;  // 已是絕對路徑
    return g_dll_dir + name;
}

static const char* DEFAULT_INI_TEXT =
    "; wc1cht.ini — Wing Commander 1 中文化設定檔（UTF-8，改完請重新啟動遊戲）\r\n"
    "; 放在 wc1cht.dll 同一個資料夾。不存在時 DLL 會自動產生這份預設檔。\r\n"
    ";\r\n"
    "; 【部署清單：要放進遊戲目錄的檔案】\r\n"
    ";   wc1cht.dll、本檔（wc1cht.ini），以及「本檔指定」的兩類檔案：\r\n"
    ";   1. 翻譯對照檔：[General] 的 Translation=\r\n"
    ";   2. 字型檔（CZHT 格式 .fnt）：各字型區段的 Font=，包含 [Dialogue_Hook1/2]、[Combat_Hook1/2]、[Font.名稱]\r\n"
    ";   這些檔名都可以自訂；寫相對路徑時以 wc1cht.dll 所在資料夾為基準，也可以寫絕對路徑。\r\n"
    ";\r\n"
    "; ── 場景 × Hook = 四套字型設定 ──\r\n"
    ";   [Dialogue_Hook1] 對話場景（簡報室/機庫/酒吧...）的格式化標籤，例如星圖的「巡航點1」\r\n"
    ";   [Dialogue_Hook2] 對話場景的對話字幕（畫面下方多行字幕）\r\n"
    ";   [Combat_Hook1]   空戰場景（駕駛艙內）的格式化標籤，例如 MFD 的「導航電腦」\r\n"
    ";   [Combat_Hook2]   空戰場景的整段文字\r\n"
    "; 沒寫的區段會依序借用：Combat_HookN → Dialogue_HookN → 舊版的 [HookN]。\r\n"
    ";\r\n"
    "; ── 每個區段可用的設定 ──\r\n"
    "; Font          字型檔（CZHT 格式）。相對路徑 = 相對於 wc1cht.dll 所在資料夾\r\n"
    "; GlyphWidth    「畫在畫面上」的字格寬（像素）。0 = 依檔案大小自動偵測（建議）\r\n"
    "; GlyphHeight   「畫在畫面上」的字格高（像素）。0 = 自動偵測。只填其中一個時，另一個沿用檔案的字格尺寸\r\n"
    ";               可以跟檔案裡實際儲存的字格不同，也可以不是正方形：\r\n"
    ";                 比檔案小 → 只取字格的一個視窗（例如 19x19 的檔案填 GlyphHeight=18 → 畫面上是 19x18，裁掉最下面一列）\r\n"
    ";                 比檔案大 → 多出來的部分補空白（例如填 GlyphWidth=20 → 右邊多一欄空白）\r\n"
    "; GlyphOffsetX/Y 視窗左上角在檔案字格內的位置（預設 0,0，可為負數）。例如 19x19 檔案想裁掉最上面一列：GlyphHeight=18、GlyphOffsetY=1\r\n"
    "; RowAlign      寬度不是 8 的倍數（如 12）時每列 bit 的對齊：auto（建議）/ left / right\r\n"
    ";               （Font/GlyphWidth/GlyphHeight/GlyphOffsetX/GlyphOffsetY/RowAlign 視為一組，跟著 Font 一起借用）\r\n"
    "; CharSpacing   字與字額外間距（像素，可為負）\r\n"
    "; LineSpacing   行與行額外間距（像素，可為負）\r\n"
    "; YAdjust       格式化標籤的垂直修正（像素）。auto = 讓字格垂直中心對齊原版 8px 英文\r\n"
    ";\r\n"
    "; ── 場景怎麼判斷？──\r\n"
    "; 依「文字被畫進的 viewport」：空戰駕駛艙的 MFD 很小（約 80px 寬），\r\n"
    "; 對話字幕是整個 320px 寬、星圖約 150px 寬。\r\n"
    "; 每遇到一個新的 viewport，wc1cht_debug.log 都會記一行「[場景] 新的 viewport ... → 空戰/對話」，\r\n"
    "; 如果判錯，可用下面的位址清單強制指定。\r\n"
    "\r\n"
    "[Log]\r\n"
    "; 1 = 寫診斷 log（wc1cht_debug.log）；0 = 不寫（預設）。載入階段的錯誤（位址驗證、翻譯表／字型／設定檔載入失敗）不受此開關影響，一律記錄。\r\n"
    "; 下面的 VerboseCalls、LogContexts、MarkKey（F9）、F12 的 Ctrl+F12 診斷傾印、[Speed]／[Interp]／[Probe] 的訊息，都要 Enable=1 才有作用。\r\n"
    "Enable=0\r\n"
    "; 雜湊格式的翻譯檔不含英文。ShowSource=1（需 Enable=1）時，會另外讀 SourceFile 指定的含英文純文字翻譯檔，\r\n"
    "; 讓 log 在每次命中時一併寫出雜湊與對應的英文原文／模板，並把未命中清單的雜湊也記下來。只用來除錯，不影響翻譯結果。\r\n"
    "ShowSource=0\r\n"
    "SourceFile=wc1zh.txt\r\n"
    "\r\n"
    "[General]\r\n"
    "; 翻譯對照檔。純文字格式（英文<TAB>中文）或雜湊格式（檔頭 #wc1cht:hash=fnv1a64，由 tools/wc1cht_hash_tool.py 產生）皆可，自動判斷。相對路徑以 wc1cht.dll 所在資料夾為基準\r\n"
    "Translation=wc1zh.txt\r\n"
    "; 每個 hook 前 N 次呼叫完整記錄，之後靜音（只記新出現的文字）。0 = 不靜音（會讓 log 很大、遊戲變慢）\r\n"
    "VerboseCalls=500\r\n"
    "; 1 = 把畫面上出現過、但 wc1zh.txt 沒有翻譯的文字記到 wc1cht_missed.tsv（翻完貼進 wc1zh.txt 即可）。預設 0 = 不記錄\r\n"
    "DumpMissed=0\r\n"
    "; 1 = 擋下「會畫到 viewport 下緣之外太多」的英文原文（防止第二關簡報星圖崩潰）。確定沒問題可改 0 關閉\r\n"
    "ClipGuard=1\r\n"
    "; 標記鍵（需 [Log] Enable=1）：進入每個畫面後按一下，log 會寫入分隔線並重新記錄該畫面的「呼叫情境」（見檔尾 Rule 說明）。none = 停用\r\n"
    "MarkKey=F9\r\n"
    "; 1 = 記錄每種新的呼叫情境（hook、呼叫端位址、viewport、文字起點、命中的規則）。設計規則時需要\r\n"
    "LogContexts=1\r\n"
    "; 整句沒翻譯時，把句中「詞彙表裡有翻譯」的名稱/詞彙單獨換成中文（例如 Talk to HUNTER. → Talk to 獵人.）。\r\n"
    "; 只處理不超過 TermMaxLen 個字元的字串（長句只換幾個詞會變成中英夾雜）。個別規則可用 Terms=on/off 覆寫\r\n"
    "TermFallback=1\r\n"
    "; 沒有規則、或規則沒寫 Align 時的對齊方式：auto（建議，還原原版）/ left / center / right\r\n"
    "DefaultAlign=auto\r\n"
    "TermMaxLen=40\r\n"
    "\r\n"
    "[Scene]\r\n"
    "; viewport 寬度 <= 此值 → 空戰場景。0 = 停用這條規則（全部視為對話）\r\n"
    "CombatMaxWidth=100\r\n"
    "; 強制判為空戰/對話的 viewport 位址（十六進位，逗號分隔，例如 0x006155e0,0x00615690）。優先於寬度規則\r\n"
    "CombatViewports=\r\n"
    "DialogueViewports=\r\n"
    "; 以「範圍」指定空戰（left,top,right,bottom；多組用 | 隔開）。\r\n"
    "; 空戰通訊字幕（\"SPIRIT: My honor is to obey.\"）畫在暫時建立的 viewport，位址每次不同，\r\n"
    "; 只能用範圍認：18,14,301,74\r\n"
    "; 簡報室星圖右側的文字面板（155,2,259,155）只有 105px 寬、154px 高，用大字會排不下，\r\n"
    "; 也歸到「小字組」（Combat_*）。名稱雖叫 Combat，凡是列在這裡的範圍都用 Combat_* 的字型設定。\r\n"
    "CombatRects=18,14,301,74 | 155,2,259,155\r\n"
    "\r\n"
    "[Dialogue_Hook1]\r\n"
    "Font=wc1cht.fnt\r\n"
    "GlyphWidth=0\r\n"
    "GlyphHeight=0\r\n"
    "RowAlign=auto\r\n"
    "CharSpacing=0\r\n"
    "LineSpacing=0\r\n"
    "YAdjust=auto\r\n"
    "\r\n"
    "[Dialogue_Hook2]\r\n"
    "Font=wc1cht.fnt\r\n"
    "GlyphWidth=0\r\n"
    "GlyphHeight=0\r\n"
    "RowAlign=auto\r\n"
    "CharSpacing=0\r\n"
    "LineSpacing=0\r\n"
    "\r\n"
    "[Combat_Hook1]\r\n"
    "Font=wc1cht.fnt\r\n"
    "GlyphWidth=0\r\n"
    "GlyphHeight=0\r\n"
    "RowAlign=auto\r\n"
    "CharSpacing=0\r\n"
    "LineSpacing=0\r\n"
    "YAdjust=auto\r\n"
    "\r\n"
    "[Combat_Hook2]\r\n"
    "Font=wc1cht.fnt\r\n"
    "GlyphWidth=0\r\n"
    "GlyphHeight=0\r\n"
    "RowAlign=auto\r\n"
    "CharSpacing=0\r\n"
    "LineSpacing=0\r\n"
    "\r\n"
    "; ═══════════ 進階：依「畫面位置」指定字型（字型集 + 規則）═══════════\r\n"
    "; 「對話/空戰 × Hook1/Hook2」只有 4 套，不夠分辨十幾種畫面時，用下面兩種區段：\r\n"
    ";\r\n"
    ";   [Font.名稱]   一套字型（寫法同上面的 Font/GlyphWidth/GlyphHeight/RowAlign/CharSpacing/LineSpacing/YAdjust）\r\n"
    ";   [Rule.名稱]   一條規則：符合條件時使用某套字型\r\n"
    ";\r\n"
    ";       Hook=1|2          只比對 Hook1（格式化標籤）或 Hook2（整段文字）；不寫 = 不限\r\n"
    ";       Viewport=0x...    文字畫進哪個 viewport 結構（遊戲全域的 viewport 位址固定，可寫多個，逗號分隔）\r\n"
    ";       Rect=l,t,r,b      viewport 範圍必須完全相同\r\n"
    ";       Inside=l,t,r,b    viewport 範圍必須在這個矩形內\r\n"
    ";       Width=a-b  Height=a-b   viewport 的寬/高範圍（只寫一個數字 = 至少）\r\n"
    ";       Cursor=l,t,r,b    文字起點（%X/%Y 或游標）落在這個矩形內（用來分開同一個全螢幕 viewport 裡的不同區塊）\r\n"
    ";       Caller=0xA-0xB    呼叫端：hook 被呼叫時的返回位址落在此範圍（往上追八層，任何一層落在範圍內就算；可寫多段，逗號分隔）\r\n"
    ";       Scene=名稱,名稱   場景：目前畫面屬於 [SceneDrivers] 裡該名稱的場景（用來分酒吧/簡報室/任務報告室…）\r\n"
    ";       Skip=1            符合時不翻譯、不記錄，原樣交給遊戲（例如玩家輸入的存檔名）；此時不需要 Use\r\n"
    ";       Align=auto|left|center|right  文字對齊（不寫 = 沿用 [General] DefaultAlign，預設 auto）。\r\n"
    ";                         auto = 每一段文字各自跟著遊戲原本的對齊方式（置中／靠左），\r\n"
    ";                                原版哪裡置中、哪裡靠左就照樣還原（字幕／選單／對話框／標題置中，Sector:／Range: 等靠左）。\r\n"
    ";                         left = 一律從游標開始；center/right = 一律在 viewport 左右界內對齊。\r\n"
    ";                         每一行各自對齊（折行後每行各自置中，跟原版一樣）；Hook1 帶 %X,%Y 的標籤不做對齊\r\n"
    ";       Terms=on|off      這條規則是否做「詞彙替換」；不寫 = 沿用 [General] TermFallback\r\n"
    ";\r\n"
    ";   [SceneDrivers]  名稱=位址（或 0xA-0xB 範圍，逗號分隔）：每個場景對應的位址，供 Scene= 引用。\r\n"
    ";                   位址從 log 的 [情境] 行（呼叫端第 3～8 層）找出來，同一場景的所有文字都會經過它。\r\n"
    ";       Text=樣式|樣式    英文原文符合萬用字元樣式（* 與 ?，已把玩家名字還原成 $C/$N）\r\n"
    ";       Priority=數字     多條規則同時符合時，數字大的優先（預設 0；同數字依檔案順序）\r\n"
    ";       Use=字型集名稱    要用的字型集（可用 Font.名稱 的名稱，或 Dialogue_Hook1/2、Combat_Hook1/2）\r\n"
    ";       Shrink=名稱,名稱  Use 的字放不下時，依序改用這些較小的字型集（單行標籤看寬度、多行文字看高度）\r\n"
    ";\r\n"
    "; 沒有任何規則符合時，維持原本「對話/空戰 × Hook1/Hook2」的預設行為。\r\n"
    "; 怎麼知道每個畫面該寫什麼條件？先把 [Log] Enable 設成 1，進入畫面後按標記鍵（MarkKey），打開 wc1cht_debug.log 找\r\n"
    "; 「[情境] 標記#N Hook2 呼叫端=… viewport=… 起點=… 文字=…」那幾行。\r\n"
    ";\r\n"
    "; 範例：\r\n"
    "; [Font.Tiny8]\r\n"
    "; Font=chinese_boutique7.fnt\r\n"
    "; [Rule.RightMFD]\r\n"
    "; Viewport=0x00615460\r\n"
    "; Use=Tiny8\r\n";

static void write_default_ini(const std::string& ini_path) {
    FILE* fp = fopen(ini_path.c_str(), "wb");
    if (!fp) return;
    fwrite(DEFAULT_INI_TEXT, 1, strlen(DEFAULT_INI_TEXT), fp);
    fclose(fp);
}

// 讀 ini 的一個值；found 回報這個 key 到底有沒有寫（沒寫 ≠ 寫了空字串）
static std::string ini_raw(const char* ini, const std::string& sec, const char* key, bool* found) {
    char buf[MAX_PATH * 2];
    GetPrivateProfileStringA(sec.c_str(), key, "\x01", buf, sizeof(buf), ini);
    if (buf[0] == '\x01') { if (found) *found = false; return std::string(); }
    if (found) *found = true;
    std::string s(buf);
    // 去掉行尾註解：只有「前面是空白的 ;」才算（"74; 155" 這種清單分隔用的 ; 要保留）
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == ';' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) { s.erase(i); break; }
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t st = 0; while (st < s.size() && (s[st] == ' ' || s[st] == '\t')) st++;
    return s.substr(st);
}

static std::string ini_get_str(const char* ini, const std::string& sec, const char* key, const char* def) {
    bool f; std::string v = ini_raw(ini, sec, key, &f);
    return f ? v : std::string(def);
}

static int ini_get_int(const char* ini, const std::string& sec, const char* key, int def) {
    bool f; std::string v = ini_raw(ini, sec, key, &f);
    return (f && !v.empty()) ? atoi(v.c_str()) : def;
}

// 依「借用鏈」找第一個有寫這個 key 的區段
static std::string ini_chain_get(const char* ini, const std::vector<std::string>& chain,
                                 const char* key, const char* def) {
    for (const auto& sec : chain) {
        bool f; std::string v = ini_raw(ini, sec, key, &f);
        if (f) return v;
    }
    return def;
}

// 解析 "0x006155e0, 0x00615690" 這種十六進位位址清單
// 位址一律照 WC1 寫；在 SM1/SM2 執行時換成該 exe 的資料位址。"SM2:0x…" 前綴 = 該 exe 專用的原始位址（不轉換）。
static std::vector<DWORD> parse_hex_list(const std::string& s) {
    std::vector<DWORD> out;
    const char* p = s.c_str();
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        std::string tok;                                       // 取到下一個逗號
        const char* q = p; while (*q && *q != ',') q++;
        tok.assign(p, q - p);
        p = q;
        int pf = wcprof::strip_exe_prefix(tok);
        if (pf < 0) continue;                                  // 別的 exe 專用 → 略過
        char* end = nullptr;
        unsigned long v = strtoul(tok.c_str(), &end, 16);
        if (end == tok.c_str()) continue;
        if (pf == 0 && wcprof::id() > 0) {
            uint32_t m = 0;
            if (wcprof::data((uint32_t)v, m)) { wclog_force("[位址表] 資料位址 0x%08lX → 0x%08lX", v, (unsigned long)m); v = m; }
            else { wclog_force("[位址表] 警告：資料位址 0x%08lX 在 %s 沒有對應，已略過（該 exe 沒有這個畫面/函式時屬正常；要手動指定可寫 %s:0x…）", v, wcprof::id_name(wcprof::id()), wcprof::id_prefix(wcprof::id())); continue; }
        }
        out.push_back((DWORD)v);
    }
    return out;
}


// ── 規則解析用的小工具 ──
static std::string str_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}
static std::vector<std::string> str_split(const std::string& s, char sep) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == sep) { out.push_back(str_trim(cur)); cur.clear(); } else cur += c; }
    out.push_back(str_trim(cur));
    std::vector<std::string> r;
    for (auto& x : out) if (!x.empty()) r.push_back(x);
    return r;
}
static bool parse_rect4(const std::string& s, VpRect& r) {
    int v[4]; int n = 0; const char* q = s.c_str(); char* end = nullptr;
    while (n < 4) {
        while (*q == ' ' || *q == ',' || *q == '\t') q++;
        if (!*q) break;
        v[n++] = (int)strtol(q, &end, 10);
        if (end == q) return false;
        q = end;
    }
    if (n != 4) return false;
    r.l = (short)v[0]; r.t = (short)v[1]; r.r = (short)v[2]; r.b = (short)v[3];
    return true;
}
static void parse_range(const std::string& s, int& lo, int& hi) {      // "10-100" 或 "50"（=至少 50）
    size_t d = s.find('-');
    if (d == std::string::npos) { lo = atoi(s.c_str()); return; }
    lo = atoi(s.substr(0, d).c_str());
    hi = atoi(s.substr(d + 1).c_str());
}
// 位址一律照 WC1 的「返回位址」寫；在 SM1/SM2 執行時換成該 exe 的返回位址。
//   • 單一位址：可能對應多個位址（例如 SM2 的空戰通訊字幕有 3 個繪製分支），全部加入。
//   • 範圍 A-B：兩端各自換算。
//   • "SM2:0x…" 前綴 = 該 exe 專用的原始位址（不轉換）；其他 exe 的前綴整筆略過。
static std::vector<std::pair<DWORD, DWORD> > parse_addr_ranges(const std::string& s) {
    std::vector<std::pair<DWORD, DWORD> > out;
    for (auto& tok0 : str_split(s, ',')) {
        std::string tok = tok0;
        int pf = wcprof::strip_exe_prefix(tok);
        if (pf < 0) continue;
        size_t d = tok.find('-');
        DWORD a = (DWORD)strtoul(tok.c_str(), nullptr, 0), b = a;
        bool is_range = (d != std::string::npos);
        if (is_range) b = (DWORD)strtoul(tok.c_str() + d + 1, nullptr, 0);
        if (b < a) std::swap(a, b);
        if (pf == 0 && wcprof::id() > 0) {
            std::vector<uint32_t> ma = wcprof::code_ret_all(a);
            if (!is_range) {
                if (ma.empty()) { wclog_force("[位址表] 警告：返回位址 0x%08lX 在 %s 沒有對應，已略過（該 exe 沒有這個畫面/函式時屬正常；要手動指定可寫 %s:0x…）", (unsigned long)a, wcprof::id_name(wcprof::id()), wcprof::id_prefix(wcprof::id())); continue; }
                for (uint32_t m : ma) { wclog_force("[位址表] 返回位址 0x%08lX → 0x%08lX", (unsigned long)a, (unsigned long)m); out.push_back(std::make_pair((DWORD)m, (DWORD)m)); }
                continue;
            }
            std::vector<uint32_t> mb = wcprof::code_ret_all(b);
            if (ma.empty() || mb.empty()) { wclog_force("[位址表] 警告：範圍 0x%08lX-0x%08lX 的端點在 %s 沒有對應，已略過", (unsigned long)a, (unsigned long)b, wcprof::id_name(wcprof::id())); continue; }
            DWORD na = (DWORD)ma[0], nb = (DWORD)mb[0]; if (nb < na) std::swap(na, nb);
            wclog_force("[位址表] 範圍 0x%08lX-0x%08lX → 0x%08lX-0x%08lX", (unsigned long)a, (unsigned long)b, (unsigned long)na, (unsigned long)nb);
            out.push_back(std::make_pair(na, nb));
            continue;
        }
        out.push_back(std::make_pair(a, b));
    }
    return out;
}

// 依一個「借用鏈」建立一套字型設定（含載入字型檔、字距、行距、垂直修正）
static ChtFont build_font(const char* ini, const std::vector<std::string>& chain, const char* tag,
                          std::unordered_map<std::string, ChtFont>& cache) {
    // Font / GlyphWidth / GlyphHeight / RowAlign 是一組：跟著「第一個有寫 Font 的區段」走，
    // 避免「借了別人的 Font，卻套用自己的 GlyphWidth」造成尺寸錯亂
    std::string font_sec = chain[0];
    for (const auto& sec : chain) { bool f; ini_raw(ini, sec, "Font", &f); if (f) { font_sec = sec; break; } }
    std::string fname = ini_get_str(ini, font_sec, "Font", "wc1cht.fnt");
    int gw = ini_get_int(ini, font_sec, "GlyphWidth", 0);
    int gh = ini_get_int(ini, font_sec, "GlyphHeight", 0);
    int gox = ini_get_int(ini, font_sec, "GlyphOffsetX", 0);
    int goy = ini_get_int(ini, font_sec, "GlyphOffsetY", 0);
    std::string al = ini_get_str(ini, font_sec, "RowAlign", "auto");
    int align_opt = (_stricmp(al.c_str(), "left") == 0) ? 1 : (_stricmp(al.c_str(), "right") == 0) ? 2 : 0;

    int cs = atoi(ini_chain_get(ini, chain, "CharSpacing", "0").c_str());
    int ls = atoi(ini_chain_get(ini, chain, "LineSpacing", "0").c_str());
    std::string ya = ini_chain_get(ini, chain, "YAdjust", "auto");

    wclog("[%s] 設定(字型取自 [%s]): Font=%s GlyphWidth=%d GlyphHeight=%d GlyphOffset=%d,%d RowAlign=%s CharSpacing=%d LineSpacing=%d YAdjust=%s",
          tag, font_sec.c_str(), fname.c_str(), gw, gh, gox, goy, al.c_str(), cs, ls, ya.c_str());

    std::string full = join_dir(fname);
    char key[MAX_PATH * 2 + 32];
    _snprintf(key, sizeof(key), "%s|%d|%d|%d|%d|%d", full.c_str(), gw, gh, align_opt, gox, goy);
    ChtFont f;
    auto hit = cache.find(key);
    if (hit != cache.end()) {
        f = hit->second;
        wclog("[%s] 與先前載入的字型相同，共用資料（%dx%d）", tag, f.w, f.h);
    } else {
        load_cht_font(f, full.c_str(), gw, gh, align_opt, tag, gox, goy);
        cache[key] = f;
    }
    f.char_spacing = cs;
    f.line_spacing = ls;
    f.y_adjust = (_stricmp(ya.c_str(), "auto") == 0 || ya.empty()) ? -((f.h - 8) / 2) : atoi(ya.c_str());
    f.y_offset = atoi(ini_chain_get(ini, chain, "YOffset", "0").c_str());
    return f;
}

static std::string lower_str(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// ── 標記鍵 ──
// 想知道「這個畫面的文字是從哪裡畫出來的」，最直接的方式是：進入畫面後按一下標記鍵。
// log 會寫入分隔線 + 目前各規則的命中統計，之後出現的每一種呼叫情境都會重新記錄（標上編號）。
static void do_mark(const char* how) {
    LONG id = InterlockedIncrement(&g_mark_id);
    std::string st;
    for (const Rule& r : g_rules) {
        char b[96]; _snprintf(b, sizeof(b), " %s=%ld", r.name.c_str(), (long)r.hits);
        st += b;
    }
    wclog_force("==================== 標記 #%ld（%s）====================", (long)id, how);
    wclog_force("[標記#%ld] 到目前為止各規則命中次數:%s", (long)id, st.empty() ? " （沒有規則）" : st.c_str());
}

static DWORD WINAPI mark_thread_proc(LPVOID) {
    bool prev = false;
    for (;;) {
        Sleep(25);
        bool down = (GetAsyncKeyState(g_mark_vk) & 0x8000) != 0;
        if (down && !prev) { do_mark("使用者按下標記鍵"); Beep(1200, 70); }
        prev = down;
    }
    return 0;
}

static int parse_vk(const std::string& s) {
    std::string t = lower_str(str_trim(s));
    if (t.empty() || t == "none" || t == "off" || t == "0") return 0;
    if (t.size() >= 2 && t[0] == 'f' && isdigit((unsigned char)t[1])) { int n = atoi(t.c_str() + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    if (t.size() == 1 && isalnum((unsigned char)t[0])) return toupper((unsigned char)t[0]);
    return (int)strtol(t.c_str(), nullptr, 0);
}

static void load_wrap_config(const char* ini);          // 中文折行規則（避頭尾點，定義在 draw_cht_string 前面）
static void load_bigtext_config(const char* ini);      // Hook3 設定（定義在 10e' 節）

// 讀 ini，載入四套字型。回傳「至少有一套可用」。
static bool load_fonts_from_config() {
    std::string ini_path_s = g_dll_dir + "wc1cht.ini";
    const char* ini = ini_path_s.c_str();
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        wclog_err("找不到 wc1cht.ini，產生預設設定檔: %s", ini);
        write_default_ini(ini_path_s);
    } else {
        wclog("讀取設定檔: %s", ini);
    }

    g_trans_file = ini_get_str(ini, "General", "Translation", "wc1zh.txt");
    g_source_file = ini_get_str(ini, "Log", "SourceFile", "wc1zh.txt");
    g_wct_show_source = g_log_enabled && ini_get_int(ini, "Log", "ShowSource", 0) != 0;   // 原文對照只在開 log 時有意義
    g_verbose_limit = ini_get_int(ini, "General", "VerboseCalls", 500);
    g_combat_max_width = ini_get_int(ini, "Scene", "CombatMaxWidth", 100);
    g_dump_missed  = ini_get_int(ini, "General", "DumpMissed", 0) != 0;
    g_clip_guard   = ini_get_int(ini, "General", "ClipGuard", 1) != 0;
    g_term_fallback = ini_get_int(ini, "General", "TermFallback", 1) != 0;
    {
        std::string da = lower_str(ini_get_str(ini, "General", "DefaultAlign", "auto"));
        g_default_align = (da == "left") ? 0 : (da == "center") ? 1 : (da == "right") ? 2 : 3;
    }
    g_term_max_len  = ini_get_int(ini, "General", "TermMaxLen", 40);
    g_combat_rects.clear();
    {   // "18,14,301,74; 0,0,10,10"
        std::string rs = ini_get_str(ini, "Scene", "CombatRects", "");
        const char* q = rs.c_str();
        while (*q) {
            int v[4]; int n = 0; char* end = nullptr;
            while (n < 4) {
                while (*q == ' ' || *q == ',' || *q == '\t') q++;
                if (!*q || *q == ';' || *q == '|') break;
                v[n++] = (int)strtol(q, &end, 10);
                if (end == q) { n = -1; break; }
                q = end;
            }
            if (n == 4) { VpRect r = { (short)v[0], (short)v[1], (short)v[2], (short)v[3] }; g_combat_rects.push_back(r); }
            while (*q && *q != ';' && *q != '|') q++;
            if (*q == ';' || *q == '|') q++;
        }
    }
    g_combat_vps   = parse_hex_list(ini_get_str(ini, "Scene", "CombatViewports", ""));
    g_dialogue_vps = parse_hex_list(ini_get_str(ini, "Scene", "DialogueViewports", ""));
    wclog_force("場景規則: CombatMaxWidth=%d  強制空戰 viewport %d 個 / 範圍 %d 組  強制對話 viewport %d 個  VerboseCalls=%ld  DumpMissed=%d",
                g_combat_max_width, (int)g_combat_vps.size(), (int)g_combat_rects.size(),
                (int)g_dialogue_vps.size(), g_verbose_limit, (int)g_dump_missed);

    // 同一個檔案 + 同樣解讀方式只載入一份（四套設定常常共用字型）
    std::unordered_map<std::string, ChtFont> cache;

    for (int scene = 0; scene < 2; scene++) {
        for (int hook = 0; hook < 2; hook++) {
            const char* hn = hook == HOOK_1 ? "Hook1" : "Hook2";
            std::string sec_new = std::string(scene == SCENE_DIALOGUE ? "Dialogue_" : "Combat_") + hn;
            std::string sec_dlg = std::string("Dialogue_") + hn;
            std::string sec_old = hn;                          // 舊版相容

            std::vector<std::string> chain;
            chain.push_back(sec_new);
            if (scene == SCENE_COMBAT) chain.push_back(sec_dlg);
            chain.push_back(sec_old);

            char tag[48];
            _snprintf(tag, sizeof(tag), "%s/%s", SCENE_NAME[scene], hn);
            g_font[scene][hook] = build_font(ini, chain, tag, cache);
        }
    }

    // 某一套載入失敗時，依序借用：同場景另一個 hook → 同 hook 另一個場景 → 另一個場景另一個 hook
    bool any = false;
    for (int scene = 0; scene < 2; scene++) {
        for (int hook = 0; hook < 2; hook++) {
            ChtFont& f = g_font[scene][hook];
            if (f.loaded) { any = true; continue; }
            const ChtFont* donors[3] = { &g_font[scene][1 - hook], &g_font[1 - scene][hook], &g_font[1 - scene][1 - hook] };
            for (const ChtFont* d : donors) {
                if (d->loaded) {
                    wclog_err("[%s/Hook%d] 字型載入失敗，改借用 %dx%d 的字型", SCENE_NAME[scene], hook + 1, d->w, d->h);
                    int cs = f.char_spacing, ls = f.line_spacing, ya = f.y_adjust;
                    f = *d;
                    f.char_spacing = cs; f.line_spacing = ls; f.y_adjust = ya;
                    any = true;
                    break;
                }
            }
        }
    }
    // ── 字型集登記：舊版四個區段（借用後的最終結果）+ 新的 [Font.名稱] ──
    g_fontsets.clear();
    static const char* const legacy_names[2][2] = { {"dialogue_hook1", "dialogue_hook2"}, {"combat_hook1", "combat_hook2"} };
    for (int sc = 0; sc < 2; sc++) for (int hk = 0; hk < 2; hk++) g_fontsets[legacy_names[sc][hk]] = g_font[sc][hk];

    std::vector<std::string> sections;
    {
        std::vector<char> nb(65536, 0);
        DWORD n = GetPrivateProfileSectionNamesA(nb.data(), (DWORD)nb.size(), ini);
        for (DWORD i = 0; i < n && nb[i]; ) { std::string nm(&nb[i]); sections.push_back(nm); i += (DWORD)nm.size() + 1; }
    }
    for (const std::string& sec : sections) {
        if (_strnicmp(sec.c_str(), "Font.", 5) != 0) continue;
        std::string nm = lower_str(sec.substr(5));
        std::vector<std::string> chain(1, sec);
        std::string tag = "字型集 " + sec;
        ChtFont f = build_font(ini, chain, tag.c_str(), cache);
        g_fontsets[nm] = f;
        if (f.loaded) any = true;
    }

    g_scene_drivers.clear();
    {
        std::vector<char> kb(16384, 0);
        DWORD n = GetPrivateProfileSectionA("SceneDrivers", kb.data(), (DWORD)kb.size(), ini);
        for (DWORD i = 0; i < n && kb[i]; ) {
            std::string kv(&kb[i]); i += (DWORD)kv.size() + 1;
            size_t eq = kv.find('=');
            if (eq == std::string::npos || kv[0] == ';') continue;
            std::string nm = str_trim(kv.substr(0, eq));
            std::string val = kv.substr(eq + 1);
            size_t semi = val.find(" ;"); if (semi != std::string::npos) val.erase(semi);
            std::vector<std::pair<DWORD, DWORD> > addrs = parse_addr_ranges(str_trim(val));   // 單一位址或 0xA-0xB 範圍，逗號分隔
            if (!nm.empty() && !addrs.empty()) g_scene_drivers.push_back(std::make_pair(nm, addrs));
        }
        wclog_force("場景驅動函式: %d 個場景", (int)g_scene_drivers.size());
    }

    g_rules.clear();
    for (const std::string& sec : sections) {
        if (_strnicmp(sec.c_str(), "Rule.", 5) != 0) continue;
        Rule r;
        r.name = sec.substr(5);
        r.priority = ini_get_int(ini, sec, "Priority", 0);
        std::string hk = lower_str(ini_get_str(ini, sec, "Hook", "any"));
        r.hook = (hk == "1" || hk == "hook1") ? 1 : (hk == "2" || hk == "hook2") ? 2 : 0;
        std::string v;
        v = ini_get_str(ini, sec, "Rect", "");   if (!v.empty()) r.has_rect   = parse_rect4(v, r.rect);
        v = ini_get_str(ini, sec, "Inside", ""); if (!v.empty()) r.has_inside = parse_rect4(v, r.inside);
        v = ini_get_str(ini, sec, "Cursor", ""); if (!v.empty()) r.has_cursor = parse_rect4(v, r.cursor);
        v = ini_get_str(ini, sec, "Width", "");  if (!v.empty()) parse_range(v, r.wmin, r.wmax);
        v = ini_get_str(ini, sec, "Height", ""); if (!v.empty()) parse_range(v, r.hmin, r.hmax);
        r.viewports = parse_hex_list(ini_get_str(ini, sec, "Viewport", ""));
        r.callers   = parse_addr_ranges(ini_get_str(ini, sec, "Caller", ""));
        r.texts     = str_split(ini_get_str(ini, sec, "Text", ""), '|');
        r.skip      = ini_get_int(ini, sec, "Skip", 0) != 0;
        r.yoffset   = ini_get_int(ini, sec, "YOffset", 0);
        {
            std::string al = lower_str(ini_get_str(ini, sec, "Align", ""));
            r.align = al.empty() ? -1 : (al == "center" || al == "centre" || al == "middle") ? 1 : (al == "right") ? 2 : (al == "auto") ? 3 : 0;
            std::string tm = lower_str(ini_get_str(ini, sec, "Terms", ""));
            r.terms = (tm == "on" || tm == "1" || tm == "true") ? 1 : (tm == "off" || tm == "0" || tm == "false") ? 0 : -1;
        }
        r.scene_names = str_split(ini_get_str(ini, sec, "Scene", ""), ',');
        for (const std::string& sn : r.scene_names) {
            bool ok = false;
            for (auto& d : g_scene_drivers)
                if (_stricmp(d.first.c_str(), sn.c_str()) == 0) { ok = true; r.scene_addrs.insert(r.scene_addrs.end(), d.second.begin(), d.second.end()); }
            if (!ok) wclog_err("[規則 %s] 警告：Scene=\"%s\" 在 [SceneDrivers] 裡找不到", r.name.c_str(), sn.c_str());
        }
        r.use_name  = lower_str(ini_get_str(ini, sec, "Use", ""));
        for (auto& x : str_split(ini_get_str(ini, sec, "Shrink", ""), ',')) r.shrink_names.push_back(lower_str(x));

        auto it = g_fontsets.find(r.use_name);
        if (r.skip) {
            r.use = &g_fontsets.begin()->second;              // 不會用到，只是避免空指標
            g_rules.push_back(r);
            continue;
        }
        if (r.use_name.empty() || it == g_fontsets.end()) {
            wclog_err("[規則 %s] 停用：Use=\"%s\" 找不到對應的字型集（可用：Font.名稱 區段，或 Dialogue_Hook1/2、Combat_Hook1/2）",
                        r.name.c_str(), r.use_name.c_str());
            continue;
        }
        r.use = &it->second;
        for (auto& nm : r.shrink_names) {
            auto sj = g_fontsets.find(nm);
            if (sj == g_fontsets.end()) wclog_err("[規則 %s] 警告：Shrink 的字型集 \"%s\" 不存在，已略過", r.name.c_str(), nm.c_str());
            else r.shrink.push_back(&sj->second);
        }
        g_rules.push_back(r);
    }
    std::stable_sort(g_rules.begin(), g_rules.end(), [](const Rule& a, const Rule& b) { return a.priority > b.priority; });
    // 排序後 vector 內容已搬移，字型集指標仍有效（std::map 節點不會搬）
    wclog_force("規則: 共載入 %d 條、字型集 %d 套", (int)g_rules.size(), (int)g_fontsets.size());
    for (const Rule& r : g_rules)
        wclog("  規則 %-24s 優先序=%d Hook=%d  Viewport數=%d Caller段數=%d Text樣式=%d Rect=%d Inside=%d Cursor=%d → 字型 %dx%d%s",
              r.name.c_str(), r.priority, r.hook, (int)r.viewports.size(), (int)r.callers.size(), (int)r.texts.size(),
              (int)r.has_rect, (int)r.has_inside, (int)r.has_cursor, r.use->w, r.use->h,
              r.shrink.empty() ? "" : "（可縮小）");

    load_wrap_config(ini);
    load_bigtext_config(ini);                       // [BigText]：開場字幕／製作名單（Hook3）

    g_log_contexts = ini_get_int(ini, "General", "LogContexts", 1) != 0;
    g_mark_vk = parse_vk(ini_get_str(ini, "General", "MarkKey", "F9"));
    if (g_log_enabled && g_mark_vk && !g_mark_thread) {   // 標記鍵只服務 log，沒開 log 就不啟動
        g_mark_thread = CreateThread(NULL, 0, mark_thread_proc, NULL, 0, NULL);
        wclog_force("標記鍵已啟用（虛擬鍵碼 0x%02X）：進入每個畫面後按一下，log 會標出分段並重新記錄該畫面的呼叫情境", g_mark_vk);
    }

    return any;
}

// ─────────────────────────────────────────────
// 7c. 場景判定（對話 vs 空戰）
// ─────────────────────────────────────────────
//
// 兩個場景都是由同樣的 Hook1/Hook2 擷取文字，函式本身分不出差別，
// 所以改看「文字被畫進哪一個 viewport」：
//   對話場景：字幕是整個 320px 寬的 viewport、星圖約 150px 寬
//   空戰場景：駕駛艙 MFD/HUD 的 viewport 很小
// 判定順序：ini 的位址清單（強制） → 寬度規則 → 預設對話。
// 每個新出現的 viewport 都會強制記一行 log，方便對照、修正規則。

static std::unordered_set<std::string> g_vp_seen;
static std::unordered_set<std::string> g_text_seen;

static int classify_scene(const FontContext* ctx, const char* label, const char* eng_str) {
    const Viewport* vp = ctx->viewport;
    DWORD addr = (DWORD)(uintptr_t)vp;
    int width  = vp->right  - vp->left + 1;
    int height = vp->bottom - vp->top  + 1;

    int scene = SCENE_DIALOGUE;
    const char* why = "預設";
    bool forced = false;
    for (DWORD a : g_combat_vps)   if (a == addr) { scene = SCENE_COMBAT;   why = "ini CombatViewports 指定"; forced = true; }
    for (DWORD a : g_dialogue_vps) if (a == addr) { scene = SCENE_DIALOGUE; why = "ini DialogueViewports 指定"; forced = true; }
    if (!forced) {
        for (const VpRect& r : g_combat_rects)
            if (r.l == vp->left && r.t == vp->top && r.r == vp->right && r.b == vp->bottom) {
                scene = SCENE_COMBAT; why = "ini CombatRects 範圍吻合"; forced = true; break;
            }
    }
    if (!forced && g_combat_max_width > 0 && width <= g_combat_max_width) {
        scene = SCENE_COMBAT; why = "viewport 寬度 <= CombatMaxWidth";
    } else if (!forced && g_combat_max_width > 0) {
        why = "viewport 寬度 > CombatMaxWidth";
    }

    if (g_vp_seen.size() < 200) {
        char key[96];
        _snprintf(key, sizeof(key), "%08X|%d,%d,%d,%d", (unsigned)addr, vp->left, vp->top, vp->right, vp->bottom);
        if (g_vp_seen.insert(key).second) {
            wclog_force("[場景] 新的 viewport 0x%08X  範圍 left=%d top=%d right=%d bottom=%d (%dx%d)  → %s場景（%s）  "
                        "首次出現: [%s] \"%.40s\"",
                        (unsigned)addr, vp->left, vp->top, vp->right, vp->bottom, width, height,
                        SCENE_NAME[scene], why, label, eng_str ? eng_str : "");
        }
    }
    return scene;
}

// 把連續數字摺成 '#'：HUD 的 "35540 km" 每幀都不一樣，摺疊後才不會被當成無數種新文字
static std::string collapse_digits(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (isdigit((unsigned char)c)) { if (o.empty() || o.back() != '#') o += '#'; }
        else o += c;
    }
    return o;
}

static int letter_count(const std::string& s) {
    int n = 0;
    for (char c : s) if (isalpha((unsigned char)c)) n++;
    return n;
}

// 把換行與連續空白展平成單一空白（遊戲折行用的 '\n' 不是原文的一部分）
static std::string flatten_ws(const std::string& s) {
    std::string o; bool sp = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') { sp = true; continue; }
        if (sp && !o.empty()) o += ' ';
        sp = false; o += c;
    }
    return o;
}
static std::string rtrim_ws(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) s.pop_back();
    return s;
}

// 用幾種寫法依序查：原樣 → 去結尾空白 → 換行展平（含去空白）。每種寫法都先查精確表、再查含變數的模板。
static bool lookup_variants(const std::string& key, std::string& zh_out, const char*& how) {
    g_dbg_hash = 0;
    std::string v[4] = { key, rtrim_ws(key), std::string(), std::string() };
    if (key.find('\n') != std::string::npos || key.find("  ") != std::string::npos) v[2] = flatten_ws(key);
    // 第 4 種：連開頭的空白一起去掉（例如 "                         Location: 23996.0.0"，
    // 原版畫這行時開頭那一大串空白並不會讓文字縮排，所以翻譯表直接寫 "Location: $S" 就好）
    {
        size_t a = 0; while (a < key.size() && (key[a] == ' ' || key[a] == '\t')) a++;
        if (a > 0) v[3] = rtrim_ws(flatten_ws(key.substr(a)));
    }
    static const char* const tag_exact[4]   = { "精確", "精確(去結尾空白)", "精確(換行展平)", "精確(去開頭空白)" };
    static const char* const tag_pattern[4] = { "變數模板", "變數模板(去結尾空白)", "變數模板(換行展平)", "變數模板(去開頭空白)" };
    for (int i = 0; i < 4; i++) {
        if (i > 0 && (v[i].empty() || v[i] == v[0] || (i == 2 && v[i] == v[1]) || (i == 3 && (v[i] == v[1] || v[i] == v[2])))) continue;
        if (trans_exact_find(v[i], zh_out, &g_dbg_hash)) { how = tag_exact[i]; return true; }
        if (match_patterns(v[i], zh_out)) { how = tag_pattern[i]; return true; }
    }
    return false;
}

// "SPIRIT: My honor is to obey." → speaker="SPIRIT", rest="My honor is to obey."
// 空戰通訊的台詞前面，遊戲會自己加上說話者名稱，原始資料裡只有後半句，所以要拆開各別查表。
//
// 說話者有兩種寫法（log 實測）：
//   友機呼號     全大寫          SPIRIT / PALADIN / MANIAC
//   敵機/艦名    大小寫混合      Krant / Dralthi / Ralari / Drayman / Tiger's Claw
// 舊版只認全大寫，所以敵方的通訊一句也拆不開、查不到翻譯。
// 現在只要「冒號在 2～24 字元內、前綴由字母/數字/空白/'/-/. 組成、以字母開頭、至少兩個字母」就拆；
// 拆錯了也只是後半句查不到（不會誤翻），所以條件放寬沒有風險。
static bool split_speaker(const std::string& t, std::string& spk, std::string& rest) {
    size_t colon = t.find(": ");
    if (colon == std::string::npos || colon < 2 || colon > 24) return false;
    int letters = 0;
    for (size_t i = 0; i < colon; i++) {
        unsigned char c = (unsigned char)t[i];
        if (isalpha(c)) letters++;
        else if (!(isdigit(c) || c == ' ' || c == '\'' || c == '-' || c == '.')) return false;
    }
    if (letters < 2 || !isalpha((unsigned char)t[0])) return false;
    spk = t.substr(0, colon);
    rest = t.substr(colon + 2);
    return !rest.empty();
}

// 沒翻譯的文字 → wc1cht_missed.tsv（格式跟 wc1zh.txt 一樣：英文<TAB>空白，翻完貼進去即可）
// 不管文字存在遊戲哪個檔案，只要「畫面上出現過」就會被記下來，
// 所以可以補足 wc_extract_text.py 掃不到的空戰訊息等字串。
static std::unordered_set<std::string> g_dump_seen;
static bool g_dump_seeded = false;

static std::string driver_names();

static void dump_missed(const char* label, int scene, const std::string& text, const char* rule_name = nullptr) {
    if (!g_dump_missed) return;
    if (letter_count(text) <= 2) return;                       // "40"、"200"、"35540 km"、"m" 之類的 HUD 數值
    std::string norm = collapse_digits(text);
    std::string path = g_dll_dir + "wc1cht_missed.tsv";

    if (!g_dump_seeded) {                                       // 先讀入舊檔，避免每次執行都重複寫
        g_dump_seeded = true;
        FILE* rf = fopen(path.c_str(), "rb");
        if (rf) {
            std::string line; int ch;
            while ((ch = fgetc(rf)) != EOF) {
                if (ch == '\n') {
                    if (!line.empty() && line[0] != '#') {
                        size_t tab = line.find('\t');
                        g_dump_seen.insert(collapse_digits(unescape_field(line.substr(0, tab))));
                    }
                    line.clear();
                } else if (ch != '\r') line += (char)ch;
            }
            fclose(rf);
        } else {
            FILE* wf = fopen(path.c_str(), "wb");
            if (wf) {
                const char* hdr = "# wc1cht_missed.tsv — 遊戲畫面上出現過、但 wc1zh.txt 沒有翻譯的文字\r\n"
                                  "# 格式：英文<TAB>中文<TAB>來源（第 3 欄僅供參考，載入時會被忽略）。多行用 \\n 表示。\r\n"
                                  "# 中文欄留空的條目不會生效（遊戲繼續顯示英文）。翻完後把整行貼進 wc1zh.txt 即可。\r\n";
                fwrite(hdr, 1, strlen(hdr), wf);
                fclose(wf);
            }
        }
    }
    if (g_dump_seen.size() > 20000 || !g_dump_seen.insert(norm).second) return;

    FILE* fp = fopen(path.c_str(), "ab");
    if (!fp) return;
    std::string hashinfo;                                       // 原文對照開啟時，把這句的雜湊也記下來（對照雜湊格式的翻譯檔用）
    if (g_wct_show_source) { char hb[32]; snprintf(hb, sizeof(hb), " h=%016llx", (unsigned long long)wct_hash(text)); hashinfo = hb; }
    std::string line = escape_field(text) + "\t\t[" + label + "/" + SCENE_NAME[scene] +
                       (rule_name ? std::string("/") + rule_name : std::string()) + "@" + driver_names() + hashinfo + "]\r\n";
    fwrite(line.data(), 1, line.size(), fp);
    fclose(fp);
}

// ─────────────────────────────────────────────
// 8.  中文字幕繪製
// ─────────────────────────────────────────────

// === 這段邏輯依 Viewport 結構（見第 2 節）實作 ===
//
// 最初版本假設 pixels 是「row * 320 + col」的簡單線性排列，
// 這個假設是錯的：實際定址要透過 viewport->rowOffsets[row] 查表，
// 而且邊界要用 viewport 自己的 left/top/right/bottom，不是寫死
// 320×200。這正是先前造成畫面橫向拖影破圖的原因。

// ─────────────────────────────────────────────
// 借用遊戲原生的英文字元繪製函式（0x0043AFD8 = DrawTextCharacter）
// ─────────────────────────────────────────────
//
// === 這是修正「中英文混排時英文消失」問題的關鍵 ===
//
// 翻譯字串裡常常保留 $C/$N 這種變數（玩家 callsign/姓名，
// 通常是英文），apply_variables() 展開後，混在中文句子裡的
// 這段英文文字需要被畫出來。
//
// 這裡不自己刻一套 ASCII 點陣字型，而是直接呼叫遊戲原本、
// 完全沒被我們 patch 過的 DrawTextCharacter(0x0043AFD8)——
// 這個位址只有 Hook 1(0x0044E215) 和 Hook 2(0x0043AD4E) 被
// 改寫成 JMP，0x0043AFD8 本身從頭到尾都是原始未修改的遊戲碼，
// 呼叫它是完全安全的。
//
// DrawTextCharacter 內部會讀取全域的 FontContext（DAT_0062264C，
// 跟我們讀寫 ctx 是同一塊記憶體），用「目前 context 裡設定的字型」
// 畫出一個字元，畫完後會自動更新 context 的 cursor_x/cursor_y。
// 所以呼叫前要把我們目前算好的游標同步寫回 ctx，呼叫後再讀回來，
// 確保雙方的游標狀態不會互相踩到。

typedef void (__stdcall *FnDrawTextChar)(char c);
static FnDrawTextChar ORIG_DRAW_CHAR =
    reinterpret_cast<FnDrawTextChar>(0x0043AFD8);      // WC1；其他版本由 apply_profile() 改寫

// 依 exe 版本設定三個寫死位址，並用機器碼特徵驗證；驗證失敗就讓兩個 hook 一律交還原函式（英文原文），絕不亂寫記憶體。
static bool g_profile_ok = true;
static void apply_profile() {
    int id = wcprof::id();
    ADDR_FONT_CTX   = wcprof::sym(wcprof::tbl::S_FONT_CTX);
    ADDR_PLAYER_REC = wcprof::sym(wcprof::tbl::S_PLAYER_REC);
    ORIG_DRAW_CHAR  = reinterpret_cast<FnDrawTextChar>(wcprof::sym(wcprof::tbl::S_DRAW_CHAR));
    // DrawTextCharacter：push ebp; mov ebp,esp; sub esp,8; push ebx/esi/edi; movsx eax,[ebp+8] …，+22 處是 mov eax,[FontContext 指標]
    static const BYTE head[] = { 0x55,0x8B,0xEC,0x83,0xEC,0x08,0x53,0x56,0x57,0x0F,0xBE,0x45,0x08 };
    const BYTE* p = (const BYTE*)(uintptr_t)ORIG_DRAW_CHAR;
    bool ok = !IsBadReadPtr(p, 40) && memcmp(p, head, sizeof(head)) == 0 && p[22] == 0xA1 &&
              *(const DWORD*)(p + 23) == ADDR_FONT_CTX &&
              (DWORD)(uintptr_t)GetModuleHandleA(NULL) == 0x00400000;
    g_profile_ok = ok;
    if (ok)
        wclog_force("[位址表] %s：FontContext=0x%08lX PlayerRecord=0x%08lX DrawTextCharacter=0x%08lX → 驗證通過",
                    wcprof::id_name(id), (unsigned long)ADDR_FONT_CTX, (unsigned long)ADDR_PLAYER_REC,
                    (unsigned long)(uintptr_t)ORIG_DRAW_CHAR);
    else
        wclog_err("[位址表] %s：FontContext=0x%08lX PlayerRecord=0x%08lX DrawTextCharacter=0x%08lX → 驗證失敗（hook 停用，一律顯示英文原文；請確認 exe 版本）",
                  wcprof::id_name(id), (unsigned long)ADDR_FONT_CTX, (unsigned long)ADDR_PLAYER_REC,
                  (unsigned long)(uintptr_t)ORIG_DRAW_CHAR);
}

// ─────────────────────────────────────────────
// 格式字串裡的「版面控制碼」（Hook1 專用）
// ─────────────────────────────────────────────
//
// DrawFormattedText 的格式字串例如 "%X%Y%F%s"：
//   %X → 設定游標 x      %Y → 設定游標 y      %F → 設定前景色
//   %s → 要印出的字串
// 原函式會先依序處理這些控制碼（改 FontContext 的游標與顏色），
// 最後才印字。Hook 把原函式整個接管之後，這些「副作用」必須由我們
// 自己補做，否則中文會畫在上一次殘留的游標位置、用上一次殘留的顏色
// （這就是星圖標籤「巡航點1」「虎爪號」跑位、變色的原因）。
struct FmtLayout {
    bool  has_x = false, has_y = false, has_f = false;
    short x = 0, y = 0;
    BYTE  f = 0;
    bool  simple = true;        // 只含 %X %Y %F 和「剛好一個」%s，沒有其他字元
    const char* why = "";       // simple=false 時的原因（給 log）
};

// 單行標籤模式：不自動換行，太寬時整段左移貼齊右界
// （星圖標籤是單行，換行會跑到 viewport 最左邊，位置完全不對）
static bool g_single_line = false;

// 文字對齊：0=靠左（從游標開始）、1=在 viewport 內置中、2=靠右。每一個「視覺行」各自對齊（跟原版一樣）。
static int g_draw_align = 0;

// 垂直位移（字型的 y_offset + 規則的 YOffset）。只影響「畫在哪」，不影響遊戲的排版游標（見 draw_cht_string 結尾）。
static int g_draw_yoff = 0;

// 星圖標籤的垂直對齊修正改為每個字型各自一個 y_adjust（見 ChtFont），
// 預設 auto = 讓字格垂直中心對齊原版 8px 英文，可在 wc1cht.ini 的 YAdjust 覆寫。

// 目前是哪個 hook 在呼叫 draw_cht_string（由 process_dialog_text 設定，
// 純粹給 log 分辨用，讓每一行 draw 相關的 log 都能看出來源）
static const char* g_cur_label = "?";

// 逐字元 log 的額度：這類 log 量很大（每句對話幾十個字元），
// 前面一定完整記錄（足以涵蓋 crash 現場），額度用完後就只保留
// 「缺字」「進出函式」這類關鍵事件，避免 log 檔無限膨脹拖慢遊戲。
static long g_char_log_budget   = 2000;
static long g_missing_glyph_cnt = 0;
static long g_draw_call_cnt     = 0;

// ── ASCII 字元寬度（對齊用）──
// 遊戲原生字型是變寬的（M=11、i=4…），而且不同字型（對話字幕 vs HUD）寬度不同。
// 要把「中英夾雜」的一行置中，就需要知道每個英數字實際多寬。
// 做法：借原生 DrawTextCharacter 量——把 ctx->viewport 暫時換成我們自己的緩衝區，
// 畫一個字、讀游標前進多少、再完全還原。畫面不會被碰到；結果依 font_data 位址快取。
static BYTE g_scratch_pix[320 * 200];
static WORD g_scratch_rows[200];
static bool g_scratch_ready = false;
static std::map<DWORD, std::vector<signed char> > g_adv_cache;

static int est_ascii_adv(char c) {                  // 量不到時的估計（對話字幕用的大字型）
    if (c == ' ') return 4;
    if (strchr("il.,:;'!|1", c)) return 4;
    if (strchr("()[]", c)) return 5;
    if (strchr("mMwW", c)) return 11;
    if (c >= 'A' && c <= 'Z') return 9;
    if (c >= '0' && c <= '9') return 8;
    return 7;
}

static int ascii_adv(FontContext* ctx, char c) {
    unsigned uc = (unsigned char)c;
    if (uc >= 128) return 7;
    std::vector<signed char>& tab = g_adv_cache[(DWORD)(uintptr_t)ctx->font_data];
    if (tab.empty()) tab.assign(128, -1);
    if (tab[uc] >= 0) return tab[uc];

    int d = 0;
    if (ctx->viewport) {
        if (!g_scratch_ready) { for (int y = 0; y < 200; y++) g_scratch_rows[y] = (WORD)(y * 320); g_scratch_ready = true; }
        Viewport tmp = *ctx->viewport;
        tmp.pixels = g_scratch_pix; tmp.rowOffsets = g_scratch_rows;
        tmp.left = 0; tmp.top = 0; tmp.right = 319; tmp.bottom = 199;
        Viewport* old_vp = ctx->viewport; short ox = ctx->cursor_x, oy = ctx->cursor_y;
        ctx->viewport = &tmp; ctx->cursor_x = 100; ctx->cursor_y = 100;
        ORIG_DRAW_CHAR(c);
        d = ctx->cursor_x - 100;
        ctx->viewport = old_vp; ctx->cursor_x = ox; ctx->cursor_y = oy;
    }
    if (d <= 0 || d > 24) d = est_ascii_adv(c);
    tab[uc] = (signed char)d;
    return d;
}

// <<WRAP_BEGIN>>
// ── 中文折行的標點規則（避頭點／避尾點／句號不成行）──
// 遊戲的字幕框最多兩行，多出來的會直接翻到下一頁，所以規則絕對「不能讓行數變多」：
//   1. 避頭點：下一行第一個字不能是 。，、；：？！」』）》 等 → 把前一行最後一個字一起擠到下一行
//   2. 避尾點：行尾不能是 「『（《 等 → 它跟後面的字一起換行
//   3. …… 與 —— 這類兩格符號不從中間拆開；本身可以在行首／行尾
//   4. 整段結尾的 。 如果剛好折到下一行變成孤字 → 直接不畫
//   5. 連續的英數字符號（中間沒有空白）視為一個詞，不從中間拆開；換行時空白留在上一行，下一行不以空白開頭
// 如果套用 1/2/3 之後行數比「單純折行」多（例如第二行也已經滿了），這一段就退回單純折行，不冒險多出一行。
static bool g_kinsoku = true;                       // 規則 1~3
static bool g_drop_final_period = true;             // 規則 4
static bool g_keep_words = true;                    // 規則 5：連續的英數字（00:19、2654.125、(Grathal)）不從中間拆開
static std::unordered_set<DWORD> g_no_line_start;   // 不可在行首
static std::unordered_set<DWORD> g_no_line_end;     // 不可在行尾
static std::unordered_set<DWORD> g_no_split;        // 兩個相同的這種字不可從中間拆開

static const char* DEFAULT_NO_LINE_START = "。，、；：？！）」』》〉】〕］｝”’．,.:!?)]}";
static const char* DEFAULT_NO_LINE_END   = "「『（《〈【〔［｛“‘([{";
static const char* DEFAULT_NO_SPLIT      = "…—⋯―";

static void utf8_to_set(const std::string& s, std::unordered_set<DWORD>& out) {
    out.clear();
    const char* q = s.c_str();
    while (*q) { DWORD c = utf8_next(q); if (c == 0) break; if (c != ' ' && c != 0xFFFD) out.insert(c); }
}

struct WrapPlan {
    bool valid = false;
    std::vector<char> brk;      // brk[位元組位移] = 1 → 這個字之前要換行
    std::vector<char> skip;     // skip[位元組位移] = 1 → 這個字不畫（規則 4）
};

// start_x = 第一行的起點 x；left/right = viewport 左右界；wrap_ascii = 英數字也檢查超出右界（置中/靠右模式）
static WrapPlan plan_wrap(const std::string& s, const ChtFont& font, int start_x, int left, int right,
                          bool wrap_ascii, const std::function<int(char)>& ascii_w) {
    struct Item { size_t off; DWORD cp; int adv, w; bool fit, nl; };
    WrapPlan P;
    P.brk.assign(s.size() + 1, 0); P.skip.assign(s.size() + 1, 0);
    std::vector<Item> it;
    const char* base = s.c_str(); const char* q = base; const char* end = base + s.size();
    while (q < end) {
        const char* st = q; DWORD cp = utf8_next(q); if (cp == 0) break;
        Item w = { (size_t)(st - base), cp, 0, 0, false, false };
        if (cp == '\n') w.nl = true;
        else if (cp < 0x80) { w.adv = w.w = ascii_w((char)cp); w.fit = wrap_ascii; }
        else if (font.find(cp)) { w.adv = font.advance_x(); w.w = font.w; w.fit = true; }
        else continue;                                           // 缺字不佔位（跟畫字的迴圈一致）
        it.push_back(w);
    }
    auto ascii_tok = [](DWORD c) { return c >= 0x21 && c <= 0x7E; };
    auto bad_break = [&](size_t k, bool kin) {                   // 在 it[k] 之前斷行是否違規
        DWORD a = it[k - 1].cp, b = it[k].cp;
        if (g_keep_words) {
            if (ascii_tok(a) && ascii_tok(b)) return true;       // 詞中間不斷
            if (b == ' ') return true;                           // 不以空白開頭
        }
        if (!kin) return false;
        if (g_no_line_start.count(b) || g_no_line_end.count(a)) return true;
        if (a == b && g_no_split.count(a)) return true;
        return false;
    };
    std::vector<char> skipi(it.size(), 0);
    // 排一段 [a,b)：回傳行數，br 放每個「換行前的字」索引
    auto layout = [&](size_t a, size_t b, int x0, bool kin, std::vector<size_t>& br) -> int {
        br.clear();
        size_t ls = a, i = a; int x = x0;
        while (i < b) {
            if (skipi[i]) { i++; continue; }
            const Item& w = it[i];
            if (w.fit && w.cp != ' ' && x > left && x + w.w - 1 > right) {   // 空白超出右界不算（行尾空白看不見）
                size_t bp = i;
                if (kin || g_keep_words) {
                    while (bp > ls + 1 && bad_break(bp, kin)) bp--;
                    if (bp <= ls || bad_break(bp, kin)) bp = i;  // 整行都是禁則字元／一個詞比整行還長：維持原斷點
                }
                br.push_back(bp); ls = bp; i = bp; x = left;
                continue;
            }
            x += w.adv; i++;
        }
        return (int)br.size() + 1;
    };
    bool any = false;
    size_t a = 0; int x0 = start_x;
    while (a <= it.size()) {
        size_t b = a; while (b < it.size() && !it[b].nl) b++;
        if (b > a) {
            std::vector<size_t> plain, kin;
            // 規則 4：整段最後一個字是 。，而單純折行時它會自己排到下一行 → 不畫
            if (g_drop_final_period && it[b - 1].cp == 0x3002 && b - a > 1) {
                layout(a, b, x0, false, plain);
                if (!plain.empty() && plain.back() == b - 1) { skipi[b - 1] = 1; P.skip[it[b - 1].off] = 1; any = true; }
            }
            int n_plain = layout(a, b, x0, false, plain);
            const std::vector<size_t>* use = &plain;
            if (g_kinsoku) {
                int n_kin = layout(a, b, x0, true, kin);
                if (n_kin <= n_plain) use = &kin;               // 行數不能變多
            }
            for (size_t k : *use) { P.brk[it[k].off] = 1; any = true; }
        }
        a = b + 1; x0 = left;
    }
    P.valid = any;
    return P;
}
// <<WRAP_END>>

static void load_wrap_config(const char* ini) {
    g_kinsoku           = ini_get_int(ini, "Wrap", "Kinsoku", 1) != 0;
    g_drop_final_period = ini_get_int(ini, "Wrap", "DropFinalPeriod", 1) != 0;
    g_keep_words        = ini_get_int(ini, "Wrap", "KeepWords", 1) != 0;
    utf8_to_set(ini_get_str(ini, "Wrap", "NoLineStart", DEFAULT_NO_LINE_START), g_no_line_start);
    utf8_to_set(ini_get_str(ini, "Wrap", "NoLineEnd",   DEFAULT_NO_LINE_END),   g_no_line_end);
    utf8_to_set(ini_get_str(ini, "Wrap", "NoSplit",     DEFAULT_NO_SPLIT),      g_no_split);
    wclog_force("[Wrap] 避頭尾點=%s  結尾孤立句號略過=%s  英數字不拆開=%s  （不可在行首 %d 種字、不可在行尾 %d 種字、不可拆開 %d 種字）",
                g_kinsoku ? "開" : "關", g_drop_final_period ? "開" : "關", g_keep_words ? "開" : "關",
                (int)g_no_line_start.size(), (int)g_no_line_end.size(), (int)g_no_split.size());
}

static void draw_cht_string(const std::string& zh_str, FontContext* ctx, const ChtFont& font) {
    g_draw_call_cnt++;
    wclog("[%s] draw_cht_string: 進入 #%ld  ctx=0x%p  字串=\"%.100s\"  對齊=%s  FontContext.align_mode=%u",
          g_cur_label, g_draw_call_cnt, (void*)ctx, zh_str.c_str(),
          g_draw_align == 1 ? "置中" : g_draw_align == 2 ? "靠右" : "靠左", ctx ? (unsigned)ctx->align_mode : 0u);

    if (!ctx || !ctx->viewport) {
        wclog("[%s] draw_cht_string: ctx 或 viewport 為 NULL，直接返回", g_cur_label);
        return;
    }

    Viewport* vp = ctx->viewport;
    wclog("[%s] draw_cht_string: viewport=0x%p  pixels=0x%p  rowOffsets=0x%p",
          g_cur_label, (void*)vp, (void*)vp->pixels, (void*)vp->rowOffsets);

    if (!vp->pixels || !vp->rowOffsets) {
        wclog("[%s] draw_cht_string: 警告 - viewport->pixels 或 rowOffsets 為 NULL，放棄繪製",
              g_cur_label);
        return;
    }

    short cur_x = ctx->cursor_x;
    short cur_y = ctx->cursor_y;
    BYTE  color = ctx->colour;

    const short vp_left   = vp->left;
    const short vp_top    = vp->top;
    const short vp_right  = vp->right;
    const short vp_bottom = vp->bottom;

    wclog("[%s] draw_cht_string: viewport 範圍 left=%d top=%d right=%d bottom=%d  "
          "起始游標=(%d,%d)  colour=%u  font_data=0x%p  字格=%dx%d",
          g_cur_label, vp_left, vp_top, vp_right, vp_bottom,
          cur_x, cur_y, (unsigned)color, ctx->font_data, font.w, font.h);

    // YOffset 只套用在「中文字形的點陣」：英數字是交給遊戲原生函式畫的，維持原本的位置（跟原版英文一樣），
    // 遊戲的排版游標也不動。所以中文比英文低/高幾個像素，就用 YOffset 單獨調中文。
    const int yoff = g_draw_yoff;

    // ── 垂直擺位（單行文字）──
    // 原版英文字約 8px 高，訊息框/狀態列是照 8px 設計的；中文字格比較高時：
    //   1. 小方框（確認框、訊息框：高度不到兩行）→ 垂直置中
    //   2. 放不下 → 往上推到貼齊 viewport 下緣，而不是整行丟掉不畫
    // （Hook1 的星圖標籤已由 %Y + YAdjust 決定位置，只套用第 2 項）
    if (zh_str.find('\n') == std::string::npos) {
        const int vp_h = vp_bottom - vp_top + 1;
        int new_y = cur_y;
        if (!g_single_line && vp_h < 2 * font.h && vp_h <= 40)
            new_y = vp_top + (vp_h - font.h) / 2;
        if (new_y + yoff + font.h - 1 > vp_bottom) new_y = vp_bottom - font.h + 1 - yoff;
        if (new_y + yoff < vp_top) new_y = vp_top - yoff;
        if (new_y != cur_y) {
            wclog("[%s] draw_cht_string: 垂直擺位 y=%d → %d（viewport 高 %d，字高 %d）",
                  g_cur_label, cur_y, new_y, vp_h, font.h);
            cur_y = (short)new_y;
        }
    }

    if (yoff != 0)
        wclog("[%s] draw_cht_string: 中文字形垂直位移 YOffset=%+d（英數字維持原位 y=%d）", g_cur_label, yoff, cur_y);

    // 依照 rowOffsets 表寫入單一像素，並做邊界檢查
    // （row/col 是相對於 viewport 左上角的座標，跟 cursor_x/y 一致）
    auto put_pixel = [&](int row, int col, BYTE c) {
        if (row < vp_top || row > vp_bottom) return;
        if (col < vp_left || col > vp_right) return;
        BYTE* row_ptr = vp->pixels + vp->rowOffsets[row];
        row_ptr[col] = c;
    };

    // 折行計畫（避頭尾點）。單行標籤不折行，不需要
    WrapPlan plan;
    if (!g_single_line && (g_kinsoku || g_drop_final_period || g_keep_words))
        plan = plan_wrap(zh_str, font, cur_x, vp_left, vp_right, true,
                         [&](char c) { return ascii_adv(ctx, c); });
    if (plan.valid) {
        std::string br, sk;
        for (size_t i = 0; i < plan.brk.size(); i++) { char b[16]; if (plan.brk[i]) { _snprintf(b, sizeof(b), "%d ", (int)i); br += b; } if (plan.skip[i]) { _snprintf(b, sizeof(b), "%d ", (int)i); sk += b; } }
        wclog("[%s] draw_cht_string: 折行計畫  換行位置(位元組) = %s 略過 = %s", g_cur_label, br.empty() ? "-" : br.c_str(), sk.empty() ? "-" : sk.c_str());
    }
    const char* const zh_base = zh_str.c_str();

    // 單行標籤模式：先估算整串寬度，會超出右界就整段左移貼齊右界，
    // 避免被「超出右界換行」邏輯拆成兩行。
    // （ASCII 寬度用 7px 估算，原版字型是變寬的，略高估比被截斷好）
    if (g_single_line && zh_str.find('\n') == std::string::npos) {
        int width = 0;
        const char* q = zh_str.c_str();
        while (*q) {
            DWORD c2 = utf8_next(q);
            if (c2 == 0) break;
            if (c2 < 0x80) width += 7;
            else if (font.find(c2)) width += font.advance_x();
        }
        if (cur_x + width > vp_right && width < (vp_right - vp_left)) {
            short new_x = (short)(vp_right - width);
            wclog("[%s] draw_cht_string: 單行標籤太寬(x=%d width=%d right=%d)，左移到 x=%d",
                  g_cur_label, cur_x, width, vp_right, new_x);
            cur_x = new_x;
        }
    }

    int char_index = 0;

    // 畫 [p, end) 這一段（可含 '\n'；超出右界會自動換行）。回傳 false = 超出下緣被截斷
    auto run = [&](const char* p, const char* end, bool honor_brk) -> bool {
        while (p < end && *p) {
            const size_t off = (size_t)(p - zh_base);
            DWORD cp = utf8_next(p);
            if (cp == 0) break;
            char_index++;

            bool verbose = (g_char_log_budget > 0);
            if (verbose) {
                g_char_log_budget--;
                wclog("[%s]   字元#%d  U+%04X  游標=(%d,%d)",
                      g_cur_label, char_index, cp, cur_x, cur_y);
            }

            // 換行控制要放在「一般 ASCII」判斷之前，
            // 否則 '\n'(0x0A) 會被 cp < 0x80 那個分支吃掉、永遠執行不到
            if (cp == '\n') {
                cur_x = vp_left;
                cur_y += font.advance_y();
                continue;
            }

            if (plan.valid && plan.skip[off]) continue;                 // 規則 4：結尾孤立的 。 不畫
            if (plan.valid && honor_brk && plan.brk[off]) {             // 折行計畫指定的換行（避頭尾點）
                cur_x = vp_left;
                cur_y += font.advance_y();
            }

            if (cp < 0x80) {
                // ASCII 字元（例如展開後的玩家 callsign、翻譯裡的數字）：
                // 交給遊戲原生函式畫，不要自己跳過不畫
                if (verbose) {
                    wclog("[%s]   -> ASCII '%c'，即將呼叫原生 DrawTextCharacter(0x0043AFD8)"
                          "  ctx->cursor 同步為 (%d,%d)",
                          g_cur_label, (char)cp, cur_x, cur_y);
                }
                ctx->cursor_x = cur_x;
                ctx->cursor_y = cur_y;
                ORIG_DRAW_CHAR((char)cp);
                // 順便記下這個字元在目前字型的實際寬度（對齊用）
                int dx = ctx->cursor_x - cur_x;
                if (dx > 0 && dx <= 24) {
                    std::vector<signed char>& tab = g_adv_cache[(DWORD)(uintptr_t)ctx->font_data];
                    if (tab.empty()) tab.assign(128, -1);
                    tab[cp & 127] = (signed char)dx;
                }
                // DrawTextCharacter 執行完會自動更新 ctx 的游標，讀回來同步
                cur_x = ctx->cursor_x;
                cur_y = ctx->cursor_y;
                if (verbose) {
                    wclog("[%s]   -> 原生 DrawTextCharacter 已返回，游標更新為 (%d,%d)",
                          g_cur_label, cur_x, cur_y);
                }
                continue;
            }

            // 查字形
            const BYTE* glyph = font.find(cp);
            if (!glyph) {
                // 缺字一律記錄（有上限），這是判斷「字型檔缺哪些字」的關鍵資訊
                g_missing_glyph_cnt++;
                if (g_missing_glyph_cnt <= 300) {
                    wclog("[%s]   !! 字型檔缺字 U+%04X（第 %ld 個缺字）在字串=\"%.60s\"",
                          g_cur_label, cp, g_missing_glyph_cnt, zh_str.c_str());
                }
                continue;
            }

            // 超出右界換行（用 viewport 自己的 right，不是寫死 320）
            if (cur_x + font.w - 1 > vp_right) {
                cur_x = vp_left;
                cur_y += font.advance_y();
            }

            // 超出 viewport 下緣，截斷（用 viewport 自己的 bottom）
            if (cur_y + yoff + font.h - 1 > vp_bottom) {
                wclog("[%s]   !! 超出 viewport 下緣（cur_y=%d + 位移%d + 字高%d > bottom=%d），"
                      "後面的字元截斷不畫", g_cur_label, cur_y, yoff, font.h, vp_bottom);
                return false;
            }

            // 把 font.h × font.w 的點陣寫入正確的列位址
            for (int row = 0; row < font.h; row++) {
                for (int col = 0; col < font.w; col++) {
                    if (glyph[row * font.w + col]) {
                        put_pixel(cur_y + yoff + row, cur_x + col, color);
                    }
                }
            }
            cur_x += font.advance_x();
        }
        return true;
    };

    const char* text_begin = zh_str.c_str();
    const char* text_end   = text_begin + zh_str.size();

    if (g_draw_align == 0 || g_single_line) {
        // 靠左：從游標開始，照原本的方式畫
        run(text_begin, text_end, true);
    } else {
        // 置中/靠右：先把文字切成「視覺行」（遇到 '\n' 或放不下就換行），
        // 每一行各自算出寬度，再從對齊後的 x 開始畫（原版也是每行各自置中）
        struct VLine { const char* b; const char* e; int w; };
        std::vector<VLine> lines;
        const int avail = vp_right - vp_left + 1;
        const char* q = text_begin;
        const char* line_b = q;
        int w = 0, w_trim = 0;                              // w_trim = 不含行尾空白的寬度
        while (q < text_end) {
            const char* cp_start = q;
            DWORD cp = utf8_next(q);
            if (cp == 0) break;
            if (cp == '\n') { lines.push_back({ line_b, cp_start, w_trim }); line_b = q; w = w_trim = 0; continue; }
            int glyph_w, adv;
            if (cp < 0x80) { adv = glyph_w = ascii_adv(ctx, (char)cp); }
            else if (font.find(cp)) { glyph_w = font.w; adv = font.advance_x(); }
            else continue;                                  // 缺字不佔位（跟 run 一致）
            if (plan.valid && plan.skip[(size_t)(cp_start - zh_base)]) continue;       // 規則 4
            if (plan.valid ? (plan.brk[(size_t)(cp_start - zh_base)] != 0 && w > 0)
                           : (w + glyph_w > avail && w > 0)) {             // 放不下 → 換行
                lines.push_back({ line_b, cp_start, w_trim }); line_b = cp_start; w = w_trim = 0;
            }
            w += adv;
            if (cp != ' ') w_trim = w - (cp < 0x80 ? 0 : font.char_spacing);
        }
        lines.push_back({ line_b, text_end, w_trim });

        for (size_t i = 0; i < lines.size(); i++) {
            int x0 = vp_left;
            if (g_draw_align == 1) x0 = vp_left + (avail - lines[i].w) / 2;
            else if (g_draw_align == 2) x0 = vp_right + 1 - lines[i].w;
            if (x0 < vp_left) x0 = vp_left;
            // 空行（文字結尾的 "\n" 之後那一行）沒有任何字：游標要停在「左邊界」，
            // 不能停在置中算出來的 x。否則下一段靠左的文字（例如 "Sector:"、"Proceed to…"）會從那個 x 開始，整行縮排。
            // 原版也是：換行之後游標回到左邊界。
            if (lines[i].b == lines[i].e) x0 = vp_left;
            cur_x = (short)x0;
            wclog("[%s] draw_cht_string: 第 %d/%d 行 寬 %d → %s 起點 x=%d", g_cur_label, (int)i + 1, (int)lines.size(),
                  lines[i].w, g_draw_align == 1 ? "置中" : "靠右", x0);
            if (!run(lines[i].b, lines[i].e, false)) break;
            if (i + 1 < lines.size()) { cur_x = vp_left; cur_y += font.advance_y(); }
        }
    }

    // 回寫更新後的游標位置（YOffset 沒有動到 cur_y，所以不會累加）
    ctx->cursor_x = cur_x;
    ctx->cursor_y = cur_y;
    wclog("[%s] draw_cht_string: 結束  共處理 %d 個字元  最終游標=(%d,%d)",
          g_cur_label, char_index, cur_x, cur_y);
}

// ─────────────────────────────────────────────
// 9.  玩家名稱更新
// ─────────────────────────────────────────────

static void update_player_names() {
    // DAT_00625860 是玩家 record 的指標
    void** p_player_ptr = (void**)ADDR_PLAYER_REC;
    if (!p_player_ptr || !*p_player_ptr) {
        g_callsign[0] = 0;
        g_lastname[0] = 0;
        return;
    }
    PlayerRecord* rec = (PlayerRecord*)*p_player_ptr;

    // 安全複製，保證 NUL 結尾
    strncpy(g_callsign, rec->callsign, sizeof(g_callsign) - 1);
    g_callsign[sizeof(g_callsign) - 1] = 0;

    strncpy(g_lastname, rec->last_name, sizeof(g_lastname) - 1);
    g_lastname[sizeof(g_lastname) - 1] = 0;
}

// ─────────────────────────────────────────────
// 10. Hook 函式（替換 thunk_FUN_0044e215）
// ─────────────────────────────────────────────

// ─────────────────────────────────────────────
// 10a. 確定性格式字串解析（取代舊版堆疊啟發式掃描）
// ─────────────────────────────────────────────
//
// === 這段邏輯依分析與動態驗證的結果實作 ===
//
// 之前的做法（scan_stack_for_string）是「猜」堆疊上哪個位置像字串，
// 命中率不穩定。後來經靜態分析與動態驗證，確認
// FUN_0044e215／FUN_0044df55／FUN_0043afd8／FUN_0045efe9 這四個函式
// 分別是 DrawFormattedText／FormatTextTokens／
// DrawTextCharacter／DrawFontGlyph（字形 blit 會排除 7 個高位元組值
// 0x81/0x84/0x8E/0x94/0x99/0x9A/0xE1，可當作指紋），
// 證實我們原本鎖定的 hook 目標本身沒有錯。
//
// 關鍵發現：FormatTextTokens 的分析結果顯示，不管遇到哪個逃逸碼
// （%B/%D/%F/%J/%P/%U/%X/%Y/%c/%d/%s/%u/%x），全部一律用
// unsigned long *arguments 搭配 *arguments++ 消耗「剛好一個
// 4-byte 欄位」，不會因為型別不同（byte/short/long/指標）
// 而消耗不同大小。這代表只要照著格式字串逐字元解析，
// 完全可以「精確算出」%s 對應的堆疊位置，不需要用猜的。
//
// 這個函式模擬同樣的解析邏輯，走過 param_1 指向的格式字串，
// 每遇到一個逃逸碼就把 args 指標往後移一格（4 bytes），
// 遇到 %s 就記錄下當時 *args 的值（那就是真正的對話文字指標）。
// 若格式字串裡有多個 %s（少數情況），取其中「最長」的一個
// 字串當作主要對話內容（短的通常是次要標籤/名稱）。

static const char* parse_format_string_for_dialog(const char* format, DWORD* args) {
    wclog("[Hook1] parse_format_string_for_dialog: 進入函式  format=0x%p  args=0x%p",
          (void*)format, (void*)args);

    const char* best_text = nullptr;
    size_t best_len = 0;

    const char* p = format;
    if (IsBadReadPtr((void*)p, 1)) {
        wclog("[Hook1] parse_format_string_for_dialog: format 位址不可讀，回傳 nullptr");
        return nullptr;
    }

    // 先把整個格式字串印出來，方便對照（最多 60 字元避免洗爆）
    wclog("[Hook1] parse_format_string_for_dialog: format 內容=\"%.60s\"", format);

    char c = *p++;
    int guard = 0;
    int escape_count = 0;

    while (c != 0 && guard++ < 2000) {
        if (IsBadReadPtr((void*)p, 1) && c != 0) {
            wclog("[Hook1] parse_format_string_for_dialog: 掃描到不可讀位址，中止迴圈 (guard=%d)", guard);
            break;
        }

        if (c != '%') {
            c = *p++;
            continue;
        }

        c = *p++;
        escape_count++;
        wclog("[Hook1] parse_format_string_for_dialog: 逃逸碼 #%d = '%%%c'", escape_count, c);

        switch (c) {
            case 'B': case 'F': case 'J': case 'X': case 'Y':
            case 'c': case 'd': case 'u': case 'x':
            case 'D': case 'U':
                wclog("[Hook1]   -> 消耗一個欄位，目前 args 位址=0x%p  該欄位值=0x%08X",
                      (void*)args, *args);
                args++;
                break;

            case 'P':
                wclog("[Hook1]   -> %%P 不消耗欄位");
                break;

            case 's': {
                wclog("[Hook1]   -> %%s，讀取 args=0x%p 的值 = 0x%08X（當成字串指標）",
                      (void*)args, *args);
                const char* candidate = reinterpret_cast<const char*>(*args);
                args++;
                if (IsBadReadPtr((void*)candidate, 1)) {
                    wclog("[Hook1]   -> 候選指標 0x%p 不可讀，跳過", (void*)candidate);
                } else {
                    size_t len = strlen(candidate);
                    wclog("[Hook1]   -> 候選字串（長度%zu）=\"%.80s\"", len, candidate);
                    if (len > best_len && len < 260) {
                        best_len = len;
                        best_text = candidate;
                    }
                }
                break;
            }

            default:
                wclog("[Hook1]   -> 不認得的逃逸碼 '%c'，當作消耗一個欄位", c);
                args++;
                break;
        }

        c = *p++;
    }

    wclog("[Hook1] parse_format_string_for_dialog: 結束，共處理 %d 個逃逸碼，"
          "best_text=%s", escape_count, best_text ? best_text : "(null)");

    return best_text;
}

// 走過格式字串，把 %X %Y %F 的值收集起來（用法跟 parse_format_string_for_dialog
// 相同：args 指向第一個可變參數，每個控制碼消耗一格 4 bytes）。
// 只要出現我們不確定語意的控制碼（%J %B %d %c...）或額外的文字字元，
// 就標記 simple=false，呼叫端會把這次繪製「交還給原函式」，寧可顯示英文
// 也不要畫錯位置。
static void collect_layout(const char* format, const DWORD* args, FmtLayout& L) {
    L = FmtLayout();
    int s_count = 0;
    const char* p = format;
    for (int guard = 0; *p && guard < 200; guard++) {
        char c = *p++;
        if (c != '%') { L.simple = false; L.why = "含有格式以外的文字"; continue; }
        c = *p++;
        switch (c) {
            case 'X': L.has_x = true; L.x = (short)*args++; break;
            case 'Y': L.has_y = true; L.y = (short)*args++; break;
            case 'F': L.has_f = true; L.f = (BYTE)*args++;  break;
            case 's': args++; s_count++; break;
            default:
                L.simple = false; L.why = "含有未處理的控制碼";
                if (c == 'P') break;     // %P 不消耗欄位
                args++; break;
        }
        if (c == 0) break;
    }
    if (s_count != 1) { L.simple = false; L.why = "%s 數量不是 1"; }
}

// ── 多段位置的格式字串（Hook1）──
// 有些畫面一次 DrawFormattedText 就畫好幾段，各段用自己的 %X（或 %Y/%F）定位，例如：
//   電玩機台 HUD   "%X%YScore: %s0 %XTime: %u %X1 UP"   → "Score: 40000"@x=10、"Time: 25"@x=140、"1 UP"@x=200
//   電玩機台高分榜 "%X%Y%d. %s%X%s0"                    → "1. PALADIN"@x=58、"87510"@x=198
// expand_format() 把整行接成一個字串、只留「最後一個」%X，翻譯後會整行畫在最右邊，位置全錯。
// 這裡改成：在「前一段已有文字」後遇到 %X/%Y/%F 就開新的一段，每段各自查表、各自定位。
struct FmtSeg { FmtLayout L; std::string text; };

static size_t safe_cstr_len(const char* sp, size_t max);

static bool expand_format_segments(const char* format, const DWORD* args, std::vector<FmtSeg>& segs) {
    segs.clear();
    FmtSeg cur;
    const char* p = format;
    for (int guard = 0; *p && guard < 400; guard++) {
        char c = *p++;
        if (c != '%') { cur.text += c; continue; }
        c = *p++;
        switch (c) {
            case 'X': case 'Y': case 'F': {
                if (!cur.text.empty()) {                       // 前一段已經有字 → 開新的一段；y / 顏色沿用，x 沒再指定就接在游標後面
                    FmtSeg nx;
                    nx.L.has_y = cur.L.has_y; nx.L.y = cur.L.y;
                    nx.L.has_f = cur.L.has_f; nx.L.f = cur.L.f;
                    segs.push_back(cur);
                    cur = nx;
                }
                DWORD v = *args++;
                if (c == 'X')      { cur.L.has_x = true; cur.L.x = (short)v; }
                else if (c == 'Y') { cur.L.has_y = true; cur.L.y = (short)v; }
                else               { cur.L.has_f = true; cur.L.f = (BYTE)v; }
                break;
            }
            case 's': {
                const char* sp = (const char*)(uintptr_t)*args++;
                size_t n = safe_cstr_len(sp, 256);
                if (!sp || (n == 0 && IsBadReadPtr(sp, 1))) return false;
                cur.text.append(sp, n);
                break;
            }
            case 'd': { char b[16]; _snprintf(b, sizeof(b), "%d", (int)*args++); cur.text += b; break; }
            case 'u': { char b[16]; _snprintf(b, sizeof(b), "%u", (unsigned)*args++); cur.text += b; break; }
            case 'x': { char b[16]; _snprintf(b, sizeof(b), "%x", (unsigned)*args++); cur.text += b; break; }
            case 'c': cur.text += (char)*args++; break;
            case '%': cur.text += '%'; break;
            default:  return false;                            // 不認得的控制碼：放棄（維持舊流程／交還原函式）
        }
    }
    if (!cur.text.empty()) segs.push_back(cur);
    for (auto& s : segs) s.L.simple = true;
    return !segs.empty();
}

// 安全地量字串長度（最多 max；跨頁時先探測可讀）
static size_t safe_cstr_len(const char* sp, size_t max) {
    size_t n = 0;
    if (!sp || IsBadReadPtr(sp, 1)) return 0;
    while (n < max) {
        if (n > 0 && (((uintptr_t)(sp + n)) & 0xFFF) == 0 && IsBadReadPtr(sp + n, 1)) break;
        if (sp[n] == 0) break;
        n++;
    }
    return n;
}

// 把 Hook1 的格式字串依「實際參數」展開成最後畫在畫面上的英文，同時把 %X %Y %F 記進 L。
//   例："\nWeapon: %s" + "Dart DF"            → "\nWeapon: Dart DF"
//       "\n%d %s" + (1, "Keep radio silence")  → "\n1 Keep radio silence"
//       "\n\nDESTINATION\n  %s" + "Nav 1"      → "\n\nDESTINATION\n  Nav 1"
// 展開後就能當成一般文字走完整的查表流程（精確 → 模板 → 詞彙替換），不必再「交還原函式」。
// 遇到不認得語意的控制碼（%B %D %J %P…）就放棄，回傳 false，維持交還原函式的保守做法。
static bool expand_format(const char* format, const DWORD* args, std::string& out, FmtLayout& L) {
    L = FmtLayout();
    out.clear();
    const char* p = format;
    for (int guard = 0; *p && guard < 400; guard++) {
        char c = *p++;
        if (c != '%') { out += c; continue; }
        c = *p++;
        switch (c) {
            case 'X': L.has_x = true; L.x = (short)*args++; break;
            case 'Y': L.has_y = true; L.y = (short)*args++; break;
            case 'F': L.has_f = true; L.f = (BYTE)*args++;  break;
            case 's': {
                const char* sp = (const char*)(uintptr_t)*args++;
                size_t n = safe_cstr_len(sp, 256);
                if (!sp || (n == 0 && IsBadReadPtr(sp, 1))) return false;
                out.append(sp, n);
                break;
            }
            case 'd': { char b[16]; _snprintf(b, sizeof(b), "%d", (int)*args++); out += b; break; }
            case 'u': { char b[16]; _snprintf(b, sizeof(b), "%u", (unsigned)*args++); out += b; break; }
            case 'x': { char b[16]; _snprintf(b, sizeof(b), "%x", (unsigned)*args++); out += b; break; }
            case 'c': out += (char)*args++; break;
            case '%': out += '%'; break;
            default:  return false;
        }
    }
    L.simple = true;
    return !out.empty();
}

// ─────────────────────────────────────────────
// 10b. 初始化介面（wc1cht_patch.exe 的 dispatcher 首次呼叫時使用）
// ─────────────────────────────────────────────
//
// __stdcall：callee（也就是這個函式）自己清堆疊，
// 對應 dispatcher 那邊 "push trampoline_va; call eax" 後
// 不需要再 "add esp,4" 的組譯邏輯。
//
// trampoline 位址存進 wc1cht_orig1_addr / wc1cht_orig2_addr，
// 這兩個全域變數會被下方 naked 進入點的組合語言直接用
// "jmp [變數]" 讀取，所以必須是非 static 的 extern "C" 變數。

extern "C" {
    DWORD wc1cht_orig1_addr WC1CHT_ASMLABEL(wc1cht_orig1_addr) = 0;   // Hook1 trampoline
    DWORD wc1cht_orig2_addr WC1CHT_ASMLABEL(wc1cht_orig2_addr) = 0;   // Hook2 trampoline
}

extern "C" __declspec(dllexport)
void __stdcall wc1cht_set_trampoline(DWORD trampoline_addr) {
    wc1cht_orig1_addr = trampoline_addr;
    wclog("wc1cht_set_trampoline: 收到 trampoline_addr=0x%08X，hook 初始化完成",
          trampoline_addr);
}

extern "C" __declspec(dllexport)
void __stdcall wc1cht_set_trampoline2(DWORD trampoline_addr) {
    wc1cht_orig2_addr = trampoline_addr;
    wclog("wc1cht_set_trampoline2: 收到 trampoline_addr=0x%08X，hook2 初始化完成",
          trampoline_addr);
}

// ─────────────────────────────────────────────
// 10c. 共用邏輯：翻譯比對 + 中文繪製
// ─────────────────────────────────────────────
//
// 回傳值：
//   true  = 已經用中文畫完了，呼叫端「不要」再執行原始函式
//   false = 沒有處理（沒翻譯／出錯），呼叫端必須把「原封不動的」
//           呼叫交給原始函式（由 naked 進入點用 tail JMP 完成）
//
// 【重要設計】這個函式「絕對不呼叫」原始函式。
// 舊版是在這裡呼叫 g_orig_draw(param_1)，但 Hook1 的原始函式是
// 可變參數函式，真正的參數（%X %Y %F %s 對應的值）在呼叫端
// 的堆疊上，C++ 這邊用 param_1 一個參數重新 call 一次，
// 原始函式往後讀可變參數時讀到的是 hook 自己的 frame 垃圾，
// 其中 %s 的指標是垃圾 → 存取違規 → crash。
// 現在「要不要退回英文」只回報 bool，真正的退回由 naked 進入點
// 把堆疊還原成進入時的樣子再 JMP 到 trampoline，達成 100% 透明。

// ─────────────────────────────────────────────
// 10a'. 規則比對、呼叫端追蹤、放得下嗎（Shrink）、情境記錄
// ─────────────────────────────────────────────

// 萬用字元比對：* = 任意長度、? = 任意單一字元（區分大小寫）
static bool wild_match(const char* pat, const char* str) {
    const char* star = nullptr; const char* ss = str;
    while (*str) {
        if (*pat == '?' || *pat == *str) { pat++; str++; }
        else if (*pat == '*') { star = pat++; ss = str; }
        else if (star) { pat = star + 1; str = ++ss; }
        else return false;
    }
    while (*pat == '*') pat++;
    return *pat == 0;
}

// 本次 hook 呼叫的來源資訊。fmt_slot[-1] = 返回位址、fmt_slot[-2] = 呼叫端保存的 EBP。
// 沿著 EBP 鏈再往上追兩層（遊戲程式碼用標準 frame，鏈通常是完整的；追不到就停）。
static void capture_callers(const DWORD* arg_slot) {
    g_ret_chain[0] = arg_slot[-1];
    for (int i = 1; i < CALLER_DEPTH; i++) g_ret_chain[i] = 0;
    const DWORD* prev = arg_slot - 2;
    const DWORD* fp = (const DWORD*)(uintptr_t)arg_slot[-2];
    for (int i = 1; i < CALLER_DEPTH; i++) {
        uintptr_t f = (uintptr_t)fp, p = (uintptr_t)prev;
        if (!fp || f <= p || f - p > 0x10000 || (f & 3)) break;
        if (IsBadReadPtr(fp, 8)) break;
        g_ret_chain[i] = fp[1];
        prev = fp; fp = (const DWORD*)(uintptr_t)fp[0];
    }
}

struct MatchInfo {
    int hook;                  // 1 / 2
    const Viewport* vp;
    int cx, cy;                // 文字起點（%X/%Y 若有就用，否則游標）
    const std::string* text;   // 英文 key
    int align_mode;            // FontContext.align_mode（遊戲自己設定的對齊：0=靠左 2=置中）
};

static bool rule_matches(const Rule& r, const MatchInfo& m) {
    if (r.hook && r.hook != m.hook) return false;
    const Viewport* v = m.vp;
    if (!r.viewports.empty()) {
        bool ok = false; DWORD a = (DWORD)(uintptr_t)v;
        for (DWORD x : r.viewports) if (x == a) { ok = true; break; }
        if (!ok) return false;
    }
    if (r.has_rect && !(v->left == r.rect.l && v->top == r.rect.t && v->right == r.rect.r && v->bottom == r.rect.b)) return false;
    if (r.has_inside && !(v->left >= r.inside.l && v->top >= r.inside.t && v->right <= r.inside.r && v->bottom <= r.inside.b)) return false;
    int w = v->right - v->left + 1, h = v->bottom - v->top + 1;
    if (w < r.wmin || w > r.wmax || h < r.hmin || h > r.hmax) return false;
    if (r.has_cursor && !(m.cx >= r.cursor.l && m.cx <= r.cursor.r && m.cy >= r.cursor.t && m.cy <= r.cursor.b)) return false;
    if (!r.scene_addrs.empty()) {
        bool ok = false;
        for (int i = 0; i < CALLER_DEPTH && !ok; i++) {
            if (!g_ret_chain[i]) continue;
            for (auto& rg : r.scene_addrs) if (g_ret_chain[i] >= rg.first && g_ret_chain[i] <= rg.second) { ok = true; break; }
        }
        if (!ok) return false;
    }
    if (!r.callers.empty()) {
        bool ok = false;
        for (int i = 0; i < CALLER_DEPTH && !ok; i++) {
            if (!g_ret_chain[i]) continue;
            for (auto& rg : r.callers) if (g_ret_chain[i] >= rg.first && g_ret_chain[i] <= rg.second) { ok = true; break; }
        }
        if (!ok) return false;
    }
    if (!r.texts.empty()) {
        bool ok = false;
        for (auto& pt : r.texts) if (wild_match(pt.c_str(), m.text->c_str())) { ok = true; break; }
        if (!ok) return false;
    }
    return true;
}

static Rule* find_rule(const MatchInfo& m) {
    for (Rule& r : g_rules) if (rule_matches(r, m)) return &r;
    return nullptr;
}

// 這段中文用這套字型，放得進目前 viewport 嗎？（照 draw_cht_string 的換行規則模擬一遍）
//   單行標籤（Hook1 格式化文字）：整段寬度要放得進從起點到右界，不允許換行
//   多行文字：允許換行，但最後一個字的下緣不能超出 viewport 下緣
// ASCII 寬度用 8px 估算（原版字型是變寬的，略高估比放不下好）。
static bool font_fits(const ChtFont& f, const std::string& zh, const FmtLayout* layout, const FontContext* ctx) {
    if (!f.loaded || !ctx || !ctx->viewport) return f.loaded;
    const Viewport* vp = ctx->viewport;
    int left = vp->left, right = vp->right, top = vp->top, bottom = vp->bottom;
    bool single = layout != nullptr;
    int cx = (layout && layout->has_x) ? layout->x : ctx->cursor_x;
    int cy = (layout && layout->has_y) ? layout->y + f.y_adjust : ctx->cursor_y;
    if (cy < top) cy = top;
    if (f.h > bottom - top + 1) return false;
    int last_y = -1;
    const char* p = zh.c_str();
    while (*p) {
        DWORD cp = utf8_next(p);
        if (cp == 0) break;
        if (cp == '\n') { cx = left; cy += f.advance_y(); continue; }
        int adv, w;
        if (cp < 0x80) { adv = w = 8; }
        else if (f.find(cp)) { adv = f.advance_x(); w = f.w; }
        else continue;
        if (cx + w - 1 > right) {
            if (single) return false;
            cx = left; cy += f.advance_y();
        }
        last_y = cy;
        cx += adv;
    }
    if (single) return true;                         // 單行的垂直位置由 draw 負責貼齊
    return last_y < 0 || last_y + f.h - 1 <= bottom;
}

// 目前這次呼叫的呼叫鏈上，經過哪些「場景驅動函式」→ 場景名稱（沒有 = "-"）
static std::string driver_names() {
    std::string out;
    for (auto& d : g_scene_drivers) {
        bool in = false;
        for (int i = 0; i < CALLER_DEPTH && !in; i++)
            for (auto& rg : d.second) if (g_ret_chain[i] >= rg.first && g_ret_chain[i] <= rg.second) { in = true; break; }
        if (in) { if (!out.empty()) out += "+"; out += d.first; }
    }
    return out.empty() ? "-" : out;
}

// ── 情境記錄 ──
// 每種「新的呼叫情境」記一行：hook、呼叫端（三層返回位址）、viewport 範圍/位址、文字起點、
// 目前命中的規則與字型、文字內容。設計規則時，就是從這些行挑出能把畫面分開的欄位。
// 每次按標記鍵後會重新記一輪，所以可以一個畫面一個畫面地收集。
static std::unordered_set<std::string> g_ctx_seen;
static LONG g_ctx_mark = 0;
static int  g_ctx_count = 0;

static void note_context(const MatchInfo& m, const std::string& tmpl, const char* rule_name,
                         int scene, const ChtFont& font) {
    if (!g_log_enabled || !g_log_contexts) return;
    LONG mk = g_mark_id;
    if (mk != g_ctx_mark) { g_ctx_seen.clear(); g_ctx_mark = mk; g_ctx_count = 0; }
    if (letter_count(tmpl) == 0) return;                 // 純數字（HUD 數值）不記
    if (g_ctx_count >= 800) return;
    const Viewport* v = m.vp;
    char key[320];
    _snprintf(key, sizeof(key), "%d|%08X|%08X|%08X|%08X|%08X|%08X|%08X|%08X|%08X|%d,%d,%d,%d|%d|%s", m.hook,
              (unsigned)g_ret_chain[0], (unsigned)g_ret_chain[1], (unsigned)g_ret_chain[2], (unsigned)g_ret_chain[3],
              (unsigned)g_ret_chain[4], (unsigned)g_ret_chain[5], (unsigned)g_ret_chain[6], (unsigned)g_ret_chain[7],
              (unsigned)(uintptr_t)v, v->left, v->top, v->right, v->bottom, m.cy / 8, rule_name ? rule_name : "-");
    if (!g_ctx_seen.insert(key).second) return;
    g_ctx_count++;
    std::string shown = escape_field(tmpl);
    if (shown.size() > 48) shown.resize(48);
    std::string drv = driver_names();
    wclog_force("[情境] 標記#%ld Hook%d  呼叫端=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X  viewport=0x%08X (%d,%d,%d,%d)=%dx%d  起點=(%d,%d)  對齊=%d  場景名=%s  場景=%s  規則=%s  字型=%dx%d  文字=\"%s\"",
                (long)mk, m.hook, (unsigned)g_ret_chain[0], (unsigned)g_ret_chain[1], (unsigned)g_ret_chain[2],
                (unsigned)g_ret_chain[3], (unsigned)g_ret_chain[4], (unsigned)g_ret_chain[5], (unsigned)g_ret_chain[6], (unsigned)g_ret_chain[7],
                (unsigned)(uintptr_t)v, v->left, v->top, v->right, v->bottom,
                v->right - v->left + 1, v->bottom - v->top + 1, m.cx, m.cy, m.align_mode,
                drv.c_str(), SCENE_NAME[scene], rule_name ? rule_name : "(無，用預設)", font.w, font.h, shown.c_str());
}

// 決定「這次要交還原函式畫英文」是否安全。
//
// 為什麼需要：原函式是照原版 8px 英文字的行距在排版。中文字比較高，
// 譯文一行會把游標推得比原本更下面；後面「沒翻譯、要交還原函式畫」的英文，
// 就會被畫在 viewport（面板）下緣之外。log 顯示第二關簡報星圖就是這樣：
// 右側面板下緣是 155，但「Proceed to Nav 2」被排在 y=152（8px 字，會有 4 行跑到面板外），
// 而且這個畫面每次都在同一個位置崩潰。
// 這裡把「超出下緣太多」的英文直接擋掉（不畫），並且每次交還前都記一行 log。
//   回傳 true  = 已擋下（呼叫端當作已處理，不要交還原函式）
//   回傳 false = 放行，交還原函式
static const int ORIG_FONT_H = 8;      // 原版英文字高（粗估）
static const int CLIP_TOLERANCE = 2;   // 允許超出下緣幾列（駕駛艙 MFD 的原版文字本來就會超出 1~2 列）

static bool block_original_if_unsafe(FontContext* ctx, const char* label, const char* text,
                                     const FmtLayout* layout) {
    if (!ctx || !ctx->viewport) {
        wclog("[%s] 交還原函式（沒有 viewport 資訊）  文字=\"%.40s\"", label, text);
        return false;
    }
    const Viewport* vp = ctx->viewport;
    int y = (layout && layout->has_y) ? layout->y : ctx->cursor_y;
    int x = (layout && layout->has_x) ? layout->x : ctx->cursor_x;
    bool overflow = (y + ORIG_FONT_H - 1 > vp->bottom + CLIP_TOLERANCE);
    wclog("[%s] 交還原函式: 游標=(%d,%d) viewport=(%d,%d)-(%d,%d)%s  文字=\"%.40s\"",
          label, x, y, vp->left, vp->top, vp->right, vp->bottom,
          overflow ? "  ★超出下緣" : "", text);
    if (overflow && g_clip_guard) {
        wclog_force("[%s] ★擋下英文原文：游標 y=%d 起的 %dpx 字會超出 viewport 下緣(%d)太多，不交還原函式  文字=\"%.40s\"",
                    label, y, ORIG_FONT_H, vp->bottom, text);
        return true;
    }
    return false;
}

// 初始化（兩個 hook 共用同一份翻譯表/字型資料，只需要 load 一次）
static void ensure_initialized(const char* caller_label) {
    if (g_initialized) return;
    // DLL 所在目錄（翻譯表、字型、wc1cht.ini 都以它為基準）
    char dll_path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA("wc1cht.dll"), dll_path, MAX_PATH);
    char* slash = strrchr(dll_path, '\\');
    if (slash) slash[1] = 0;
    g_dll_dir = dll_path;

    wclog("[%s] process_dialog_text: 首次找到有效字串，開始載入設定與資料檔（目錄=%s）",
          caller_label, g_dll_dir.c_str());
    bool font_ok  = load_fonts_from_config();          // 同時會讀出翻譯表檔名
    g_wct_source_path = join_dir(g_source_file);
    bool trans_ok = load_translation_table(join_dir(g_trans_file).c_str());
    if (trans_ok && font_ok)
        wclog("[%s] process_dialog_text: 初始化結果 - 翻譯表=成功 字型=成功", caller_label);
    else
        wclog_err("[%s] 初始化結果 - 翻譯表=%s 字型=%s（失敗的項目請看上面的錯誤訊息）",
                  caller_label, trans_ok ? "成功" : "失敗", font_ok ? "成功" : "失敗");
    g_initialized = true;
}

// process_dialog_text 的工作模式
//   PM_NORMAL       查表 + 用中文畫（預設）
//   PM_IDENTITY     不查表，原文當譯文照畫（多段格式裡的純數字段，例如分數 "87510"）：維持原樣，但跟其他段走同一套字型/定位
//   PM_CHECK        只查表、不畫：回傳「這段有沒有翻譯」；沒有的會記進 missed.tsv（多段格式預查用）
//   PM_CHECK_EXACT  只查「精確/模板」、不畫、不記錄、不做詞彙替換：用來判斷「整行」是否已有完整翻譯（舊流程相容）
enum { PM_NORMAL = 0, PM_IDENTITY = 1, PM_CHECK = 2, PM_CHECK_EXACT = 3 };

static bool process_dialog_text(const char* eng_str, const char* caller_label,
                                int hook_idx, const FmtLayout* layout = nullptr, int mode = PM_NORMAL) {
    g_stage = caller_label;
    lstrcpynA(g_stage_text, eng_str ? eng_str : "(null)", (int)sizeof(g_stage_text));
    wclog("[%s] process_dialog_text: 進入函式  eng_str=\"%.100s\"",
          caller_label, eng_str ? eng_str : "(null)");

    if (!eng_str || *eng_str == 0) {
        wclog("[%s] process_dialog_text: eng_str 為空，交還原始函式", caller_label);
        return false;
    }

    ensure_initialized(caller_label);

    // 取得 FontContext 與目前 viewport，判定場景（對話/空戰），再選對應的字型
    FontContext* ctx = *reinterpret_cast<FontContext**>(ADDR_FONT_CTX);
    if (ctx && (IsBadReadPtr(ctx, sizeof(FontContext)) || !ctx->viewport ||
                IsBadReadPtr(ctx->viewport, sizeof(Viewport)))) ctx = nullptr;
    int scene = ctx ? classify_scene(ctx, caller_label, eng_str) : SCENE_DIALOGUE;
    const ChtFont* base_font = &g_font[scene][hook_idx];

    update_player_names();
    std::string tmpl = make_template_key(eng_str);

    // ── 規則：依畫面位置/呼叫端/文字決定字型（沒有符合的規則才用「場景 × Hook」的預設字型）──
    Rule* rule = nullptr;
    MatchInfo mi = { hook_idx + 1, nullptr, 0, 0, &tmpl, 0 };
    if (ctx) {
        mi.vp = ctx->viewport;
        mi.align_mode = ctx->align_mode;
        mi.cx = (layout && layout->has_x) ? layout->x : ctx->cursor_x;
        mi.cy = (layout && layout->has_y) ? layout->y : ctx->cursor_y;
        rule = find_rule(mi);
        if (rule) rule->hits++;
    }
    if (rule && rule->skip) return false;           // Skip=1 的規則（例如存檔名輸入框逐字回顯）：原樣交給遊戲
    const char* rule_name = rule ? rule->name.c_str() : nullptr;
    const ChtFont* sel_font = (rule && rule->use && rule->use->loaded) ? rule->use : base_font;
    if (ctx && mode != PM_CHECK_EXACT) note_context(mi, tmpl, rule_name, scene, *sel_font);

    // ── 查表：精確 → 含 $T/$D 的模板 → 拆掉「說話者前綴」再查（每一步都會試幾種寫法）──
    // 遊戲會自己把長的通訊字幕折行（把某個空白換成 '\n'），例如
    //   "PALADIN: Blast! The enemy\nare upon us!"
    // 但翻譯表（來自資料檔）的原文是 "Blast! The enemy are upon us!"，所以要把換行展平成空白再查。
    std::string zh_tmpl;
    bool found = false;
    const char* how = "精確";
    if (mode == PM_IDENTITY) {
        zh_tmpl = tmpl; found = true; how = "原樣";
    } else {
        {
            const char* h2 = nullptr;
            if (lookup_variants(tmpl, zh_tmpl, h2)) { found = true; how = h2; }
            else {
                std::string spk, rest, zr;
                if (split_speaker(tmpl, spk, rest) && lookup_variants(rest, zr, h2)) {
                    std::string zs = spk;                         // 說話者名稱若有翻譯（詞彙表）就用翻譯，否則保留原文
                    std::string zspk;
                    if (glossary_lookup(spk, zspk)) zs = zspk;
                    zh_tmpl = zs + ": " + zr;
                    found = true; how = "說話者前綴";
                }
            }
        }

        // 整句查不到：試「詞彙替換」（句中有翻譯的名稱/詞彙單獨換成中文，其餘保留英文）
        if (!found && mode != PM_CHECK_EXACT) {
            bool use_terms = (rule && rule->terms >= 0) ? (rule->terms == 1) : g_term_fallback;
            std::string zt;
            if (use_terms && (int)tmpl.size() <= g_term_max_len && apply_terms(tmpl, zt)) {
                zh_tmpl = zt; found = true; how = "詞彙替換";
                dump_missed(caller_label, scene, tmpl, rule_name);       // 整句仍然沒翻譯，照樣記進 missed.tsv，方便補成完整的句子/模板
            }
        }
    }

    // 只查不畫（多段格式的預查）：回報有沒有翻譯；沒有的記進 missed.tsv，由呼叫端決定整行要不要交還原函式
    if (mode == PM_CHECK_EXACT) return found;
    if (mode == PM_CHECK) {
        if (!found) {
            dump_missed(caller_label, scene, tmpl, rule_name);
            wclog("[%s] 多段格式：這一段沒有翻譯，整行交還原函式  key=\"%s\"", caller_label, tmpl.c_str());
        }
        return found;
    }

    // 靜音期間（空戰 HUD 每幀重畫同樣的字）只強制記錄「第一次出現」的文字，
    // 讓你仍然能從 log 看到命中/未命中的清單，又不會每幀寫檔
    // （數字摺疊成 '#'，"35540 km" 之類每幀都在變的字只會記一次）
    if (g_log_enabled && mode == PM_NORMAL && g_log_muted && g_text_seen.size() < 5000) {
        std::string k = std::string(caller_label) + "|" + SCENE_NAME[scene] + "|" + collapse_digits(tmpl);
        if (g_text_seen.insert(k).second)
            wclog_force("[%s/%s%s%s] 新文字 %s  key=\"%.80s\"", caller_label, SCENE_NAME[scene],
                        rule_name ? "/" : "", rule_name ? rule_name : "",
                        found ? "命中" : "未命中", tmpl.c_str());
    }

    if (!found) {
        dump_missed(caller_label, scene, tmpl, rule_name);
        g_trans_miss_count++;
        if (g_trans_miss_count <= 500) {
            wclog("[%s] 翻譯表未命中 #%ld  key=\"%s\"",
                  caller_label, g_trans_miss_count, tmpl.c_str());
        } else if (g_trans_miss_count % 500 == 0) {
            wclog("[%s] 翻譯表未命中累計 %ld 次（訊息已精簡）",
                  caller_label, g_trans_miss_count);
        }
        return block_original_if_unsafe(ctx, caller_label, tmpl.c_str(), layout);
    }

    g_trans_hit_count++;
    wclog("[%s] 翻譯命中(%s) #%ld  key=\"%s\" -> \"%s\"",
          caller_label, how, g_trans_hit_count, tmpl.c_str(), zh_tmpl.c_str());
    if (g_wct_show_source && g_log_enabled) {                // 原文對照（除錯）：雜湊 / 命中的原文或模板
        if (g_wct_last_pat >= 0 && how && strstr(how, "模板"))
            wclog("    └ 原文對照：模板 #%d \"%s\"", g_wct_last_pat, escape_field(g_patterns[g_wct_last_pat].key).c_str());
        else if (g_dbg_hash)
            wclog("    └ 原文對照：hash=%016llx 原文=\"%s\"", (unsigned long long)g_dbg_hash,
                  escape_field(wct_source_of(g_dbg_hash)).c_str());
    }

    std::string zh = apply_variables(zh_tmpl);

    // 決定最終字型：規則指定的字型放不下（寬度/高度超出 viewport）時，依 Shrink 清單改用較小的
    const ChtFont* fp = sel_font;
    if (rule && !rule->shrink.empty() && rule->use && rule->use->loaded) {
        std::vector<const ChtFont*> cands(1, rule->use);
        for (const ChtFont* c : rule->shrink) cands.push_back(c);
        fp = nullptr;
        for (const ChtFont* c : cands) if (c->loaded && font_fits(*c, zh, layout, ctx)) { fp = c; break; }
        if (!fp) {                                            // 全都放不下：用最小的（最後一個載入成功的）
            for (auto it = cands.rbegin(); it != cands.rend(); ++it) if ((*it)->loaded) { fp = *it; break; }
            if (!fp) fp = sel_font;
            wclog("[%s] 規則 %s：所有候選字型都放不下，使用最小的 %dx%d", caller_label, rule_name, fp->w, fp->h);
        } else if (fp != rule->use) {
            wclog("[%s] 規則 %s：%dx%d 放不下，改用 %dx%d", caller_label, rule_name, rule->use->w, rule->use->h, fp->w, fp->h);
        }
    }
    const ChtFont& font = *fp;

    if (!font.loaded) {
        wclog("[%s/%s] process_dialog_text: 這套字型沒有載入成功，回退英文顯示", caller_label, SCENE_NAME[scene]);
        return block_original_if_unsafe(ctx, caller_label, tmpl.c_str(), layout);
    }

    // viewport 比字格還矮：中文字一定放不下，與其畫出被切掉的字，不如顯示原文
    if (ctx && (ctx->viewport->bottom - ctx->viewport->top + 1) < font.h) {
        wclog("[%s/%s] viewport 高度(%d) < 字高(%d)，放不下，回退英文顯示", caller_label, SCENE_NAME[scene],
              ctx->viewport->bottom - ctx->viewport->top + 1, font.h);
        return block_original_if_unsafe(ctx, caller_label, tmpl.c_str(), layout);
    }

    if (!ctx) {
        wclog("[%s] process_dialog_text: 警告 - FontContext 為 NULL，回退英文顯示",
              caller_label);
        return false;
    }

    g_cur_label = caller_label;   // 讓 draw_cht_string 的 log 也能標示來源 hook
    g_stage = "正在用中文繪製";
    wclog("[%s] 場景=%s  規則=%s  使用字型 %dx%d（%s）", caller_label, SCENE_NAME[scene],
          rule_name ? rule_name : "(預設)", font.w, font.h, font.path.c_str());

    // 補做原函式處理 %X %Y %F 時對 FontContext 的修改（見 FmtLayout 說明）
    if (layout) {
        if (layout->has_x) ctx->cursor_x = layout->x;
        if (layout->has_y) {
            int y = layout->y;                      // 英數字維持原版的 Y；中文字形的垂直修正（y_adjust）走 g_draw_yoff
            if (ctx->viewport && y < ctx->viewport->top) y = ctx->viewport->top;
            ctx->cursor_y = (short)y;
        }
        if (layout->has_f) ctx->colour = layout->f;
        wclog("[%s] 套用版面: x=%s%d y=%s%d colour=%s%u（Y 已含垂直修正 %d）", caller_label,
              layout->has_x ? "" : "(沿用)", (int)ctx->cursor_x,
              layout->has_y ? "" : "(沿用)", (int)ctx->cursor_y,
              layout->has_f ? "" : "(沿用)", (unsigned)ctx->colour, font.y_adjust);
    }
    g_draw_yoff = font.y_offset + (rule ? rule->yoffset : 0) + (layout && layout->has_y ? font.y_adjust : 0);
    g_single_line = (layout != nullptr);
    // 對齊：Hook1 的格式化標籤是「從 %X,%Y 開始」的，不做對齊；Hook2 的整段文字才套用 Align
    g_draw_align = 0;
    if (!layout) {
        int al = (rule && rule->align >= 0) ? rule->align : g_default_align;
        if (al == 3) al = (ctx->align_mode == 2) ? 1 : 0;   // auto：跟著遊戲設定的 FontContext.align_mode（2=置中，0=靠左）
        g_draw_align = al;
    }
    draw_cht_string(zh, ctx, font);
    g_single_line = false;
    g_draw_align = 0;
    g_draw_yoff = 0;
    wclog("[%s] process_dialog_text: draw_cht_string 已返回，已用中文取代英文", caller_label);
    return true;
}

// ── 多段位置的格式字串：逐段查表、逐段定位繪製（見 expand_format_segments 的說明）──
//
// 回傳：  1 = 已用中文畫完（進入點直接 ret）
//         0 = 交還原函式（整行維持英文；沒有翻譯的那一段已記進 missed.tsv）
//        -1 = 這個格式不適合分段（有換行、整行本來就有完整翻譯…），請走舊流程
//
// 原則「整行全有或全無」：每一段都有翻譯（或是不含字母的純數字/符號段）才畫中文；
// 只要有一段查不到，整行都交還原函式，避免畫成「一半中文一半英文、位置還對不齊」。
static int draw_format_segments(std::vector<FmtSeg>& segs) {
    for (const FmtSeg& s : segs)
        if (s.text.find('\n') != std::string::npos) return -1;       // 含換行的多行文字不在這裡處理
    ensure_initialized("Hook1");

    // 舊流程相容：「整行接起來」若翻譯表本來就有完整條目（精確或模板），照舊流程畫
    {
        std::string whole;
        for (const FmtSeg& s : segs) whole += s.text;
        if (process_dialog_text(whole.c_str(), "Hook1", HOOK_1, &segs.back().L, PM_CHECK_EXACT)) return -1;
    }

    // 去掉每段前後的空白（段與段之間靠 %X 定位，空白沒有意義），丟掉變成空的段
    std::vector<FmtSeg> use;
    for (FmtSeg s : segs) {
        while (!s.text.empty() && (s.text.back() == ' ' || s.text.back() == '\t')) s.text.pop_back();
        if (s.L.has_x) { size_t a = 0; while (a < s.text.size() && (s.text[a] == ' ' || s.text[a] == '\t')) a++; s.text.erase(0, a); }
        if (!s.text.empty()) use.push_back(s);
    }
    if (use.size() < 2) return -1;

    // 預查：每一段都要有翻譯（不含字母的段原樣畫）；至少要有一段真的需要翻譯
    std::vector<char> need(use.size(), 0);
    int need_cnt = 0;
    for (size_t i = 0; i < use.size(); i++) {
        need[i] = letter_count(use[i].text) > 0;
        if (need[i]) need_cnt++;
    }
    if (need_cnt == 0) return 0;
    for (size_t i = 0; i < use.size(); i++)
        if (need[i] && !process_dialog_text(use[i].text.c_str(), "Hook1", HOOK_1, &use[i].L, PM_CHECK)) return 0;

    // 逐段畫
    static long s_seg_log = 0;
    if (++s_seg_log <= 40) wclog("[Hook1] 多段格式：共 %d 段，逐段翻譯並各自定位", (int)use.size());
    bool any = false;
    for (size_t i = 0; i < use.size(); i++) {
        bool ok = process_dialog_text(use[i].text.c_str(), "Hook1", HOOK_1, &use[i].L, need[i] ? PM_NORMAL : PM_IDENTITY);
        if (ok) any = true;
        else if (!any) return 0;                           // 第一段就畫不出來（例如字型沒載入）：還來得及整行交還原函式
    }
    return 1;
}

// ─────────────────────────────────────────────
// 10d. Hook 1 實作（DrawFormattedText = FUN_0044e215）
// ─────────────────────────────────────────────
//
// fmt_slot 指向「原函式第一個參數（格式字串指標）在堆疊上的位置」，
// 所以 fmt_slot[0] = format，fmt_slot[1...] = 可變參數（一格 4 bytes）。
// 這個位址由 naked 進入點算好直接傳進來，不再依賴 EBP 推算或
// 編譯器最佳化行為。
//
// 回傳 1 = 已畫中文，進入點直接 ret；0 = 交還原始函式。

extern "C" WC1CHT_USED
int __cdecl wc1cht_hook1_impl(DWORD* fmt_slot) WC1CHT_ASMLABEL(wc1cht_hook1_impl);

extern "C" WC1CHT_USED
int __cdecl wc1cht_hook1_impl(DWORD* fmt_slot) {
    try {
        if (!g_profile_ok) return 0;                  // 位址表驗證失敗：一律交還原函式
        g_call_count++;
        LogMuteGuard mute(g_verbose_limit > 0 && g_call_count > g_verbose_limit);
        g_stage = "Hook1 入口";
        capture_callers(fmt_slot);
        const char* format = reinterpret_cast<const char*>(fmt_slot[0]);
        const char* eng_str = nullptr;

        if (g_call_count <= 50) {
            wclog("[Hook1] #%ld  fmt_slot=0x%p  format=0x%p  orig=0x%08X",
                  g_call_count, (void*)fmt_slot, (const void*)format, wc1cht_orig1_addr);
        }

        if (!wc1cht_orig1_addr) return 0;                 // 尚未初始化：不要冒險
        if (IsBadReadPtr((const void*)format, 1)) return 0;

        // 一次畫好幾段、各段自己用 %X/%Y 定位的格式（電玩機台 HUD、高分榜）：逐段翻譯、各自定位
        if (strchr(format, '%')) {
            std::vector<FmtSeg> segs;
            if (expand_format_segments(format, fmt_slot + 1, segs) && segs.size() >= 2) {
                int r = draw_format_segments(segs);
                if (r >= 0) return r;
            }
        }

        FmtLayout layout;
        bool has_layout = false;
        if (strchr(format, '%')) {
            eng_str = parse_format_string_for_dialog(format, fmt_slot + 1);
            collect_layout(format, fmt_slot + 1, layout);
            has_layout = true;
        } else if (*format != 0) {
            eng_str = format;
        }

        if (!eng_str) {
            g_scan_miss_count++;
            return 0;
        }

        // 格式字串除了 %X %Y %F %s 還有別的東西（固定文字、%d…）：
        // 依實際參數「展開」成完整的英文再查表（例："\nWeapon: %s" → "\nWeapon: Dart DF"）。
        // 展開不了（出現不認得的控制碼）才交還原函式。
        std::string expanded;
        if (has_layout && !layout.simple) {
            FmtLayout L2;
            if (expand_format(format, fmt_slot + 1, expanded, L2)) {
                static long s_exp_log = 0;
                if (++s_exp_log <= 40)
                    wclog("[Hook1] 格式字串含固定文字（%s），展開查表  format=\"%.40s\" → \"%.60s\"", layout.why, format, expanded.c_str());
                eng_str = expanded.c_str();
                layout = L2;
                // 有 %X/%Y/%F 才算「指定了位置/顏色」；只有固定文字 + 參數的，照遊戲目前的游標畫（可套用對齊、換行）
                has_layout = (L2.has_x || L2.has_y || L2.has_f);
            } else {
                static long s_skip_log = 0;
                if (++s_skip_log <= 30)
                    wclog("[Hook1] 格式字串無法展開（%s），交還原函式  format=\"%.60s\"  text=\"%.60s\"",
                          layout.why, format, eng_str);
                return 0;
            }
        }
        return process_dialog_text(eng_str, "Hook1", HOOK_1, has_layout ? &layout : nullptr) ? 1 : 0;
    } catch (...) {
        return 0;     // 任何 C++ 例外都不可以穿出去污染遊戲堆疊
    }
}

// ─────────────────────────────────────────────
// 10e. Hook 2 實作（DrawTextString = FUN_0043ad4e）
// ─────────────────────────────────────────────
//
// fmt_slot[0] = 已展開 $C/$N 的完整對話文字指標。

static long g_dialogue_call_count = 0;

extern "C" WC1CHT_USED
int __cdecl wc1cht_hook2_impl(DWORD* arg_slot) WC1CHT_ASMLABEL(wc1cht_hook2_impl);

extern "C" WC1CHT_USED
int __cdecl wc1cht_hook2_impl(DWORD* arg_slot) {
    try {
        if (!g_profile_ok) return 0;                  // 位址表驗證失敗：一律交還原函式
        g_dialogue_call_count++;
        LogMuteGuard mute(g_verbose_limit > 0 && g_dialogue_call_count > g_verbose_limit);
        g_stage = "Hook2 入口";
        capture_callers(arg_slot);
        const char* text = reinterpret_cast<const char*>(arg_slot[0]);

        if (!wc1cht_orig2_addr) return 0;
        if (IsBadReadPtr((const void*)text, 1) || *text == 0) return 0;

        return process_dialog_text(text, "Hook2", HOOK_2) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// ─────────────────────────────────────────────
// 10e'. Hook 3 實作（DrawBigText = FUN_0046B85D：開場字幕 + 製作名單）
// ─────────────────────────────────────────────
//
// 這個函式是「專用」的：開場的 "In the distant future, mankind is locked in a deadly war..." 和
// 製作名單都由它畫，完全不經過 Hook1 / Hook2。
//   原型：cdecl  int DrawBigText(Viewport* vp, short y_ignored, const char* text)  （回傳恆為 0）
//   行為：數 '\n' 得到行數 n，整段在 128 列高的區域內垂直置中（y0 = (128 - 16n)/2，傳進來的 y 會被蓋掉）、
//         每行在 320 寬內水平置中、行距 16；每個字元是「精靈圖」：
//             'A'..'z' → 精靈索引 c-'A'，'.' → 58（冒號圖）、',' → 59（分號圖），空格 +6，其餘（數字、'、-、! …）直接略過
//         字距 = 精靈寬度 + 2。精靈用遊戲的 DrawSprite 貼進 vp，字母圖本身就帶漸層（每列一個調色盤索引）。
//   所以這個 hook 做的是：查翻譯 → 自己排版 → 中文字形用「依列漸層」畫，英文（人名）仍用遊戲原本的精靈。
//
// 需要的遊戲內部位址（DrawSprite / 量寬度 / 精靈組 / 螢幕 viewport / 換頁旗標函式）
// 都「從 DrawBigText 函式本體讀出來」（位置固定、讀不到或對不上就整個停用），所以不用另外維護 SM1/SM2 的位址表。

typedef void (__cdecl   *FnBtSprite)(Viewport* vp, int x, int y, DWORD shapeset, int index);
typedef WORD (__stdcall *FnBtMetric)(int a, int b, DWORD shapeset, int index, int sel);
typedef void (__cdecl   *FnBtPresent)(void);

extern "C" {
    DWORD wc1cht_orig3_addr WC1CHT_ASMLABEL(wc1cht_orig3_addr) = 0;   // Hook3 trampoline
}

static bool          g_bt_ok = false;               // 位址都解出來、且驗證通過
static DWORD         g_bt_start = 0;
static FnBtSprite    g_bt_sprite  = nullptr;        // DrawSprite（cdecl）
static FnBtMetric    g_bt_metric  = nullptr;        // 精靈尺寸查詢（stdcall；sel=2 → 寬度）
static FnBtPresent   g_bt_present = nullptr;        // 設「畫面已更新」旗標
static const DWORD*  g_bt_shapeset_ptr = nullptr;   // 存放「大字型精靈組」指標的全域變數
static const DWORD*  g_bt_screen_ptr   = nullptr;   // 存放「螢幕緩衝區」指標的全域變數

// [BigText] 設定
struct BigTextCfg {
    bool enable = true;
    std::string use_name = "intro24";
    const ChtFont* font = nullptr;
    std::string gradient = "auto";   // auto / none / "idx,idx,..."
    int  color = -1;                 // gradient=none 或讀不到漸層時使用的調色盤索引；-1 = 不指定
    int  line_spacing = 4;           // 含中文的行：行高 = 字高 + 此值
    int  latin_line = 16;            // 純英文行的行高（原版 16）
    int  region_w = 320, region_h = 128;
    int  yoffset = 0;                // 中文字形整體垂直位移
    int  latin_yoffset = 0;          // 中英混排行裡英文精靈的垂直位移
    bool grad_absolute = true;       // true = 跟英文字母圖「同一個水平的列」同色（色帶厚度與英文相同）；false = 把漸層拉伸到整個字高
    int  grad_offset = 0;            // 色帶整體垂直位移（列，正=往下），微調中文與英文的色帶對齊
    int  grad_jitter = 2;            // 每一欄像素的色帶邊界隨機上下位移 -N..+N 列（模仿英文字母圖參差不齊的漸層）；0 = 關閉（每一列整齊同色）
    int  grad_jitter_run = 1;        // 連續幾欄共用同一個位移（1 = 每欄各自隨機；2~3 = 較粗的階梯）
    int  grad_jitter_seed = 0;       // 亂數種子（換一個數字就換一種參差）
};
static BigTextCfg g_bt;
static std::vector<BYTE> g_bt_grad;                 // 由上而下的調色盤索引（長度 = 字母 'I' 的高度）
static bool g_bt_grad_done = false;
static int  g_bt_grad_d = 0;                        // 'I' 的第一列有字的位置，相對於「畫 'I' 時給 DrawSprite 的 y」
static std::unordered_map<std::string, std::string> g_bt_cache;     // 英文原文 → 中文（空字串 = 沒翻譯）

static void load_bigtext_config(const char* ini) {
    g_bt = BigTextCfg();
    g_bt.enable       = ini_get_int(ini, "BigText", "Enable", 1) != 0;
    g_bt.use_name     = lower_str(ini_get_str(ini, "BigText", "Use", "intro24"));
    g_bt.gradient     = lower_str(str_trim(ini_get_str(ini, "BigText", "Gradient", "auto")));
    g_bt.color        = ini_get_int(ini, "BigText", "Color", -1);
    g_bt.line_spacing = ini_get_int(ini, "BigText", "LineSpacing", 4);
    g_bt.latin_line   = ini_get_int(ini, "BigText", "LatinLine", 16);
    g_bt.region_w     = ini_get_int(ini, "BigText", "RegionWidth", 320);
    g_bt.region_h     = ini_get_int(ini, "BigText", "RegionHeight", 128);
    g_bt.yoffset      = ini_get_int(ini, "BigText", "YOffset", 0);
    g_bt.latin_yoffset = ini_get_int(ini, "BigText", "LatinYOffset", 0);
    g_bt.grad_absolute = lower_str(str_trim(ini_get_str(ini, "BigText", "GradientMode", "absolute"))) != "stretch";
    g_bt.grad_offset   = ini_get_int(ini, "BigText", "GradientOffset", 0);
    g_bt.grad_jitter      = ini_get_int(ini, "BigText", "GradientJitter", 2);
    g_bt.grad_jitter_run  = ini_get_int(ini, "BigText", "GradientJitterRun", 1);
    g_bt.grad_jitter_seed = ini_get_int(ini, "BigText", "GradientJitterSeed", 0);
    if (g_bt.grad_jitter < 0) g_bt.grad_jitter = 0;
    if (g_bt.grad_jitter > 8) g_bt.grad_jitter = 8;
    if (g_bt.grad_jitter_run < 1) g_bt.grad_jitter_run = 1;
    auto it = g_fontsets.find(g_bt.use_name);
    if (it != g_fontsets.end() && it->second.loaded) g_bt.font = &it->second;
    g_bt_grad.clear(); g_bt_grad_done = false; g_bt_cache.clear();
    wclog_force("[BigText] Enable=%d Use=%s → %s  Gradient=%s(%s,位移%d) Color=%d LineSpacing=%d LatinLine=%d Region=%dx%d YOffset=%d LatinYOffset=%d  Hook3位址=%s",
                (int)g_bt.enable, g_bt.use_name.c_str(),
                g_bt.font ? (std::to_string(g_bt.font->w) + "x" + std::to_string(g_bt.font->h)).c_str() : "找不到／未載入的字型集（開場字幕維持英文）",
                g_bt.gradient.c_str(), g_bt.grad_absolute ? "與英文同列" : "拉伸", g_bt.grad_offset, g_bt.color, g_bt.line_spacing, g_bt.latin_line, g_bt.region_w, g_bt.region_h,
                g_bt.yoffset, g_bt.latin_yoffset, g_bt_ok ? "已就緒" : "尚未就緒");
}

// 從 DrawBigText 函式本體解出內部位址並驗證。tramp = wc1cht_patch 寫的 trampoline（前 6 bytes 是原函式開頭，接著 E9 跳回 函式+6）
static bool bt_resolve(DWORD tramp) {
    const BYTE* t = (const BYTE*)(uintptr_t)tramp;
    if (IsBadReadPtr(t, 11) || t[6] != 0xE9) { wclog_err("[Hook3] trampoline 格式不符，停用"); return false; }
    static const BYTE prologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 };
    if (memcmp(t, prologue, 6) != 0) { wclog_err("[Hook3] 原函式開頭不是預期的 55 8B EC 83 EC 18，停用"); return false; }
    DWORD cont = tramp + 11 + *(const int*)(t + 7);
    DWORD fn = cont - 6;
    const BYTE* f = (const BYTE*)(uintptr_t)fn;
    if (IsBadReadPtr(f, 0x260)) { wclog_err("[Hook3] 函式本體讀不到，停用"); return false; }
    bool ok = f[0x9F] == 0x83 && f[0xA0] == 0xF8 && f[0xA1] == 0x41 &&         // cmp eax,'A'
              f[0xC5] == 0xA1 &&                                                  // mov eax,[精靈組指標]
              f[0xD7] == 0xE8 && f[0xF1] == 0xE8 &&                              // call DrawSprite / call 精靈尺寸
              f[0x23C] == 0x8B && f[0x23D] == 0x0D && f[0x24A] == 0xE8;          // mov ecx,[螢幕]; ... call 設旗標
    if (!ok) { wclog_err("[Hook3] 函式內部結構與預期不符（可能是不同版本），停用"); return false; }
    auto tgt = [&](int off) -> DWORD { return fn + off + 5 + *(const int*)(f + off + 1); };
    g_bt_start        = fn;
    g_bt_sprite       = reinterpret_cast<FnBtSprite>(tgt(0xD7));
    g_bt_metric       = reinterpret_cast<FnBtMetric>(tgt(0xF1));
    g_bt_present      = reinterpret_cast<FnBtPresent>(tgt(0x24A));
    g_bt_shapeset_ptr = reinterpret_cast<const DWORD*>(*(const DWORD*)(f + 0xC6));
    g_bt_screen_ptr   = reinterpret_cast<const DWORD*>(*(const DWORD*)(f + 0x23E));
    wclog_force("[Hook3] DrawBigText=0x%08lX  DrawSprite=0x%08lX  精靈尺寸=0x%08lX  設旗標=0x%08lX  精靈組指標@0x%08lX  螢幕指標@0x%08lX  → 驗證通過",
                (unsigned long)fn, (unsigned long)(uintptr_t)g_bt_sprite, (unsigned long)(uintptr_t)g_bt_metric,
                (unsigned long)(uintptr_t)g_bt_present, (unsigned long)(uintptr_t)g_bt_shapeset_ptr, (unsigned long)(uintptr_t)g_bt_screen_ptr);
    return true;
}

extern "C" __declspec(dllexport)
void __stdcall wc1cht_set_trampoline3(DWORD trampoline_addr) {
    wc1cht_orig3_addr = trampoline_addr;
    wclog("wc1cht_set_trampoline3: 收到 trampoline_addr=0x%08X，hook3 初始化完成", trampoline_addr);
    g_bt_ok = bt_resolve(trampoline_addr);
}

// 取得「英文大字母的漸層」：用遊戲自己的 DrawSprite 把 'I' 畫出來，逐列讀出最常見的調色盤索引。
// 【重要】不能畫進我們自己的暫存緩衝區：遊戲的 DrawSprite 會檢查「這個 viewport 的像素緩衝區是不是遊戲登記過的緩衝區」
//   （0x45E8C6：不是螢幕緩衝區就去 0x00627A70 的登記表找，找不到就呼叫 0x004019F6 報錯結束遊戲——第一版就是這樣崩潰的）。
// 所以改成直接畫在「真正要畫字的 viewport」上：先把一小塊區域存起來 → 清成 0 → 畫 'I' → 讀 → 完整還原。
// 整個過程在同一個 hook 呼叫內完成（還沒回到遊戲、還沒換頁），只做一次。
static void bt_probe_gradient(Viewport* vp) {
    g_bt_grad_done = true;
    g_bt_grad.clear();
    const DWORD set = *g_bt_shapeset_ptr;
    if (!set) return;
    const int vl = vp->left, vt = vp->top, vr = vp->right, vb = vp->bottom;
    const int RW = 48, RH = 44;                                     // 取樣區域大小
    if (vr - vl + 1 < RW + 8 || vb - vt + 1 < RH + 8) { wclog_force("[Hook3] viewport 太小（%dx%d），無法取樣漸層", vr - vl + 1, vb - vt + 1); return; }
    const int x0 = vl + ((vr - vl + 1) - RW) / 2, y0 = vt + 4;       // 區域左上角（完全在 viewport 內）
    static const int cands[3] = { 'I' - 'A', 'H' - 'A', 'E' - 'A' };
    BYTE saved[44][48];
    for (int ci = 0; ci < 3 && g_bt_grad.empty(); ci++) {
        for (int r = 0; r < RH; r++) {
            BYTE* row = vp->pixels + vp->rowOffsets[y0 + r] + x0;
            memcpy(saved[r], row, RW);
            memset(row, 0, RW);
        }
        g_bt_sprite(vp, x0 + 16, y0 + 8, set, cands[ci]);
        int t0 = -1, b0 = -1;
        std::vector<BYTE> col;
        for (int r = 0; r < RH; r++) {
            const BYTE* row = vp->pixels + vp->rowOffsets[y0 + r] + x0;
            int hist[256] = {0}; int total = 0;
            for (int c = 0; c < RW; c++) { BYTE v = row[c]; if (v) { hist[v]++; total++; } }
            if (!total) { if (t0 >= 0 && b0 < 0) b0 = r - 1; continue; }
            if (t0 < 0) t0 = r;
            if (b0 >= 0) { col.clear(); break; }                    // 字形中間斷開 → 不是預期的字形
            int best = 1; for (int v = 1; v < 256; v++) if (hist[v] > hist[best]) best = v;
            col.push_back((BYTE)best);
        }
        for (int r = 0; r < RH; r++) memcpy(vp->pixels + vp->rowOffsets[y0 + r] + x0, saved[r], RW);   // 還原
        if (t0 >= 0 && col.size() >= 6) { g_bt_grad = col; g_bt_grad_d = t0 - 8; }     // 8 = 上面畫 'I' 時 y 比取樣區頂端低的列數
        std::string txt; for (BYTE b : col) { char q[8]; _snprintf(q, sizeof(q), "%u ", (unsigned)b); txt += q; }
        wclog_force("[Hook3] 英文字母 #%d 的漸層取樣：高 %d 列，由上到下的調色盤索引 = %s（第一列在 DrawSprite 的 y %+d）", cands[ci], (int)col.size(), txt.c_str(), t0 - 8);
    }
}

// 解析 ini 的 Gradient= 清單
static std::vector<BYTE> bt_parse_gradient_list(const std::string& s) {
    std::vector<BYTE> v;
    for (const std::string& tok : str_split(s, ',')) {
        std::string t = str_trim(tok);
        if (t.empty()) continue;
        v.push_back((BYTE)strtol(t.c_str(), nullptr, 0));
    }
    return v;
}

static int bt_latin_index(unsigned char c) {      // 與原函式相同的對應；-1 = 原函式會略過這個字元
    if (c >= 'A' && c <= 'z') return c - 'A';
    if (c == '.') return 58;
    if (c == ',') return 59;
    return -1;
}

static int bt_latin_adv(int idx, DWORD set) {
    short w = (short)g_bt_metric(0, 0, set, idx, 2);
    if (w < 0 || w > 64) w = 8;
    return w + 2;
}

struct BtLine { std::vector<DWORD> cps; int width = 0; int height = 0; bool has_cht = false; };

// 畫中文版。回傳 false = 放棄（交還原函式）
static bool bt_draw(Viewport* vp, const std::string& zh) {
    const ChtFont& font = *g_bt.font;
    const DWORD set = *g_bt_shapeset_ptr;
    if (!set) return false;

    // 漸層色表
    std::vector<BYTE> grad;
    if (g_bt.gradient == "none") { /* 單色 */ }
    else if (g_bt.gradient == "auto" || g_bt.gradient.empty()) {
        if (!g_bt_grad_done) bt_probe_gradient(vp);
        grad = g_bt_grad;
    } else grad = bt_parse_gradient_list(g_bt.gradient);
    const int grad_d = (g_bt.gradient == "auto" || g_bt.gradient.empty()) ? g_bt_grad_d : 0;
    if (grad.empty() && g_bt.color < 0) {
        wclog_err("[Hook3] 取不到漸層、ini 也沒指定 Color，放棄中文（維持英文）");
        return false;
    }

    // 拆行、量寬（太寬的行自動折行：任何字之間都可以斷，但標點不放行首）
    std::vector<BtLine> lines(1);
    {
        const int max_w = g_bt.region_w - 16;
        auto is_closing = [](DWORD cp) {
            static const DWORD cl[] = { 0xFF0C, 0x3002, 0x3001, 0xFF01, 0xFF1F, 0xFF1B, 0xFF1A, 0xFF09, 0x300D, 0x300F, 0x2026, 0x2014, 0x2027, 0xFF0E, '.', ',' };
            for (DWORD c : cl) if (c == cp) return true;
            return false;
        };
        const char* p = zh.c_str();
        while (*p) {
            DWORD cp = utf8_next(p);
            if (cp == 0) break;
            if (cp == '\r') continue;
            if (cp == '\n') { lines.emplace_back(); continue; }
            int adv, idx = -1;
            bool cht = false;
            if (cp < 0x80) {
                if (cp == ' ') adv = 6;
                else { idx = bt_latin_index((unsigned char)cp); if (idx < 0) continue; adv = bt_latin_adv(idx, set); }   // 跟原函式一樣略過
            } else {
                if (!font.find(cp)) { if (g_missing_glyph_cnt++ < 300) wclog("[Hook3] !! 字型檔缺字 U+%04X", cp); continue; }
                adv = font.advance_x(); cht = true;
            }
            if (lines.back().width + adv > max_w && !lines.back().cps.empty() && !is_closing(cp)) {
                if (cp == ' ') continue;                                    // 折行處的空白不畫
                lines.emplace_back();
            }
            BtLine& L = lines.back();
            L.cps.push_back(cp); L.width += adv; if (cht) L.has_cht = true;
        }
    }
    if (lines.size() > 1 && lines.back().cps.empty()) lines.pop_back();     // 結尾的 "\n" 不另外佔一行

    int ls = g_bt.line_spacing;
    auto total_h = [&]() { int h = 0; for (BtLine& L : lines) { L.height = L.has_cht ? font.h + ls : g_bt.latin_line; h += L.height; } return h; };
    int H = total_h();
    if (H > g_bt.region_h && ls > 0) { ls = 0; H = total_h(); }              // 放不下：先把行距收掉
    int y = (g_bt.region_h - H) / 2; if (y < 0) y = 0;
    wclog("[Hook3] 版面：%d 行  總高 %d（區域高 %d）起點 y=%d  行距 %d  字格 %dx%d", (int)lines.size(), H, g_bt.region_h, y, ls, font.w, font.h);

    const short vl = vp->left, vt = vp->top, vr = vp->right, vb = vp->bottom;
    auto put = [&](int row, int col, BYTE c) {
        if (row < vt || row > vb || col < vl || col > vr) return;
        vp->pixels[vp->rowOffsets[row] + col] = c;
    };

    for (size_t li = 0; li < lines.size(); li++) {
        const BtLine& L = lines[li];
        int x = (g_bt.region_w - L.width) / 2; if (x < 0) x = 0;
        for (DWORD cp : L.cps) {
            if (cp < 0x80) {
                if (cp == ' ') { x += 6; continue; }
                int idx = bt_latin_index((unsigned char)cp);
                int sy = y + (L.height - 16) / 2 + g_bt.latin_yoffset;
                g_bt_sprite(vp, x, sy, set, idx);
                x += bt_latin_adv(idx, set);
            } else {
                const BYTE* glyph = font.find(cp);
                int gy = y + (L.height - font.h) / 2 + g_bt.yoffset;
                const int sref = y + (L.height - 16) / 2 + g_bt.latin_yoffset;   // 這一行裡英文字母圖的頂端：色帶以它為基準，中文與英文的同色層才會在同一個水平
                // 每一欄的隨機垂直位移（由「螢幕欄位置＋字碼＋種子」雜湊決定：同一段文字每次重畫結果一致）
                int jit[64] = {0};
                const int fw = font.w < 64 ? font.w : 64;
                if (g_bt.grad_jitter > 0 && !grad.empty()) {
                    for (int c = 0; c < fw; c++) {
                        DWORD h = (DWORD)(c / g_bt.grad_jitter_run) * 2654435761u ^ (cp * 40503u) ^ ((DWORD)g_bt.grad_jitter_seed * 2246822519u);
                        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
                        jit[c] = (int)((h >> 8) % (DWORD)(2 * g_bt.grad_jitter + 1)) - g_bt.grad_jitter;
                    }
                }
                for (int r = 0; r < font.h; r++) {
                    for (int c = 0; c < font.w; c++) {
                        if (!glyph[r * font.w + c]) continue;
                        BYTE colr;
                        if (grad.empty()) colr = (BYTE)g_bt.color;
                        else {
                            int k;
                            if (g_bt.grad_absolute) k = (gy + r) - sref - grad_d + g_bt.grad_offset + (c < 64 ? jit[c] : 0);
                            else k = r * (int)grad.size() / font.h + (c < 64 ? jit[c] : 0);
                            k = k < 0 ? 0 : (k >= (int)grad.size() ? (int)grad.size() - 1 : k);
                            colr = grad[(size_t)k];
                        }
                        put(gy + r, x + c, colr);
                    }
                }
                x += font.advance_x();
            }
        }
        y += L.height;
    }
    // 跟原函式結尾一致：畫進螢幕緩衝區時要設「畫面已更新」旗標
    if (g_bt_screen_ptr && vp->pixels == (BYTE*)(uintptr_t)(*g_bt_screen_ptr)) g_bt_present();
    return true;
}

static long g_bigtext_call_count = 0;

extern "C" WC1CHT_USED
int __cdecl wc1cht_hook3_impl(DWORD* arg_slot) WC1CHT_ASMLABEL(wc1cht_hook3_impl);

extern "C" WC1CHT_USED
int __cdecl wc1cht_hook3_impl(DWORD* arg_slot) {
    try {
        if (!g_profile_ok || !g_bt_ok || !wc1cht_orig3_addr) return 0;
        g_bigtext_call_count++;
        LogMuteGuard mute(g_verbose_limit > 0 && g_bigtext_call_count > g_verbose_limit);
        g_stage = "Hook3 入口";
        Viewport* vp = reinterpret_cast<Viewport*>(arg_slot[0]);
        const char* text = reinterpret_cast<const char*>(arg_slot[2]);
        if (IsBadReadPtr(text, 1) || *text == 0 || !vp || IsBadReadPtr(vp, sizeof(Viewport)) || !vp->pixels || !vp->rowOffsets) return 0;
        ensure_initialized("Hook3");
        if (!g_bt.enable || !g_bt.font) return 0;
        g_cur_label = "Hook3";
        g_stage = "Hook3 查翻譯";
        lstrcpynA(g_stage_text, text, (int)sizeof(g_stage_text));

        std::string key(text);
        auto it = g_bt_cache.find(key);
        if (it == g_bt_cache.end()) {
            std::string zh; const char* how = nullptr;
            if (!lookup_variants(key, zh, how)) {
                zh.clear();
                dump_missed("Hook3", SCENE_DIALOGUE, key);
                wclog("[Hook3] 翻譯表未命中  key=\"%.100s\"", key.c_str());
            } else {
                wclog("[Hook3] 翻譯命中(%s)  key=\"%.60s\" → \"%.60s\"", how, key.c_str(), zh.c_str());
            }
            if (g_bt_cache.size() < 256) g_bt_cache[key] = zh;
            it = g_bt_cache.find(key);
            if (it == g_bt_cache.end()) { if (zh.empty()) return 0; g_stage = "Hook3 繪製"; return bt_draw(vp, zh) ? 1 : 0; }
        }
        if (it->second.empty() || it->second == key) return 0;
        {
            static std::unordered_set<std::string> first_seen;      // 每種文字第一次畫時強制記一行（之後 log 可能被靜音）
            if (first_seen.size() < 64 && first_seen.insert(key).second)
                wclog_force("[Hook3] 首次繪製中文  viewport 0x%p 範圍 %d,%d,%d,%d  y參數=%d  呼叫#%ld  原文=\"%.50s\"",
                            (void*)vp, vp->left, vp->top, vp->right, vp->bottom, (int)(short)arg_slot[1], g_bigtext_call_count, key.c_str());
        }
        g_stage = "Hook3 繪製";
        return bt_draw(vp, it->second) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// ─────────────────────────────────────────────
// 10f. Naked 進入點（真正被 dispatcher JMP 進來的函式）
// ─────────────────────────────────────────────
//
// 進入時堆疊（跟原函式被正常呼叫的當下完全相同）：
//     [esp]   = 呼叫端的返回位址
//     [esp+4] = 第一個參數
//     [esp+8] = 第二個參數／第一個可變參數 ...
//
// 流程：
//   1. 建 frame，把「第一個參數所在位址」[ebp+8] 傳給 C++ 實作
//   2. 實作回傳 1（已畫中文）→ 拆 frame 後依原函式的呼叫慣例 ret
//   3. 實作回傳 0（沒翻譯）→ 拆 frame（堆疊 100% 還原成進入時狀態）
//      再 JMP 到 trampoline，原始英文邏輯拿到的參數一個都沒少
//
// 「ret 幾個 bytes」要跟原函式的結尾一致：
//   Hook1 是可變參數函式 → 一定是 cdecl → 單純 ret，由呼叫端清參數
//   Hook2 是 stdcall（依分析判定）→ ret 4，自己清掉 1 個參數
//   ★ 若 Hook2 的原函式其實是 cdecl，把下面 WC1CHT_HOOK2_RET 改成 "ret"
//     驗證方法：x32dbg 到 0x0043AD4E 往下找函式結尾，
//     看到 C2 04 00 = ret 4（stdcall），看到 C3 = ret（cdecl）。
//     【已確認】Wing1_wcdx.exe / SM1_wcdx.exe / SM2_wcdx.exe 三個 exe 的 DrawTextString 行為相同，
//     函式結尾都是 C2 04 00 = ret 4，所以 "ret $4" 是對的。
//     （hook 位址在 SM1 = 0x0043AD7E、SM2 = 0x0043B41E；由 wc1cht_patch 用特徵碼找，這裡不依賴位址。）

#define WC1CHT_HOOK2_RET "ret $4"

#if defined(_MSC_VER)

extern "C" __declspec(dllexport) __declspec(naked)
void hook_draw_string(void) {
    __asm {
        push ebp
        mov  ebp, esp
        lea  eax, [ebp + 8]
        push eax
        call wc1cht_hook1_impl
        add  esp, 4
        test eax, eax
        jz   passthru
        pop  ebp
        ret
    passthru:
        pop  ebp
        jmp  dword ptr [wc1cht_orig1_addr]
    }
}

extern "C" __declspec(dllexport) __declspec(naked)
void hook_draw_dialogue(void) {
    __asm {
        push ebp
        mov  ebp, esp
        lea  eax, [ebp + 8]
        push eax
        call wc1cht_hook2_impl
        add  esp, 4
        test eax, eax
        jz   passthru
        pop  ebp
        ret  4
    passthru:
        pop  ebp
        jmp  dword ptr [wc1cht_orig2_addr]
    }
}

// Hook3：DrawBigText 是 cdecl，原函式回傳 0（xor eax,eax），所以命中時也要回 0
extern "C" __declspec(dllexport) __declspec(naked)
void hook_draw_bigtext(void) {
    __asm {
        push ebp
        mov  ebp, esp
        lea  eax, [ebp + 8]
        push eax
        call wc1cht_hook3_impl
        add  esp, 4
        test eax, eax
        jz   passthru
        xor  eax, eax
        pop  ebp
        ret
    passthru:
        pop  ebp
        jmp  dword ptr [wc1cht_orig3_addr]
    }
}

#else   // GCC / MinGW

extern "C" __declspec(dllexport) __attribute__((naked))
void hook_draw_string(void) {
    __asm__ __volatile__(
        "push %ebp\n\t"
        "mov  %esp, %ebp\n\t"
        "lea  8(%ebp), %eax\n\t"
        "push %eax\n\t"
        "call wc1cht_hook1_impl\n\t"
        "add  $4, %esp\n\t"
        "test %eax, %eax\n\t"
        "jz   1f\n\t"
        "pop  %ebp\n\t"
        "ret\n"
        "1:\n\t"
        "pop  %ebp\n\t"
        "jmp  *wc1cht_orig1_addr\n\t"
    );
}

extern "C" __declspec(dllexport) __attribute__((naked))
void hook_draw_dialogue(void) {
    __asm__ __volatile__(
        "push %ebp\n\t"
        "mov  %esp, %ebp\n\t"
        "lea  8(%ebp), %eax\n\t"
        "push %eax\n\t"
        "call wc1cht_hook2_impl\n\t"
        "add  $4, %esp\n\t"
        "test %eax, %eax\n\t"
        "jz   1f\n\t"
        "pop  %ebp\n\t"
        WC1CHT_HOOK2_RET "\n"
        "1:\n\t"
        "pop  %ebp\n\t"
        "jmp  *wc1cht_orig2_addr\n\t"
    );
}

// Hook3：DrawBigText 是 cdecl，原函式回傳 0（xor eax,eax），所以命中時也要回 0
extern "C" __declspec(dllexport) __attribute__((naked))
void hook_draw_bigtext(void) {
    __asm__ __volatile__(
        "push %ebp\n\t"
        "mov  %esp, %ebp\n\t"
        "lea  8(%ebp), %eax\n\t"
        "push %eax\n\t"
        "call wc1cht_hook3_impl\n\t"
        "add  $4, %esp\n\t"
        "test %eax, %eax\n\t"
        "jz   1f\n\t"
        "xor  %eax, %eax\n\t"
        "pop  %ebp\n\t"
        "ret\n"
        "1:\n\t"
        "pop  %ebp\n\t"
        "jmp  *wc1cht_orig3_addr\n\t"
    );
}

#endif

// ─────────────────────────────────────────────
// 10g. 支線模組：F12 debug 模式 + 降速（見 WC1_debug_speed_branch.md）
// ─────────────────────────────────────────────
#include "wc1cht_ext.inc"
#include "wc1cht_probe.inc"   // 實驗支線 v2：逐格記憶體觀測器（唯讀）
#include "wc1cht_interp.inc"  // 實驗支線 v3：子畫面內插原型

// ─────────────────────────────────────────────
// 11. DLL 進入點
// ─────────────────────────────────────────────
//
// trampoline 位址不在這裡設定！改由 dispatcher 首次呼叫時
// 透過 wc1cht_set_trampoline() 動態告知正確的 trampoline 位址。
// 這裡只需要停用 thread attach/detach 通知，減少不必要的呼叫。

// 載入時最先做的事：讀 wc1cht.ini 的 [Log] Enable（找不到 ini 或沒寫 = 關閉）
static void read_log_switch() {
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(GetModuleHandleA("wc1cht.dll"), p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    char* slash = strrchr(p, '\\');
    if (slash) slash[1] = 0;
    std::string ini = std::string(p) + "wc1cht.ini";
    if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    g_log_enabled = ini_get_int(ini.c_str(), "Log", "Enable", 0) != 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        wclog_init_path();
        read_log_switch();         // 預設不記錄；[Log] Enable=1 才寫一般診斷 log
        wclog("==================================================");
        wclog("DllMain: wc1cht.dll 已被載入 (DLL_PROCESS_ATTACH)");
        wclog("Log 路徑: %s", g_log_path);
        if (g_log_enabled) {       // 崩潰記錄器也只服務 log，沒開 log 就不安裝
            AddVectoredExceptionHandler(1, wc_veh);
            wclog("崩潰記錄器已安裝：遊戲發生存取違規等致命例外時，會把出事位址/暫存器/堆疊寫進這個 log");
        }
        apply_profile();           // 辨識 WC1 / SM1 / SM2，設定寫死位址並驗證（必須在 ini 解析與任何 hook 之前）
        wccext_attach(hInst);      // 支線模組（debug 模式 / 降速），只建立執行緒
        wccprobe_attach(hInst);    // 實驗支線 v2：觀測器（ini [Probe] Enable=1 才會啟動）
        wccinterp_attach(hInst);   // 實驗支線 v3：內插原型（ini [Interp] Enable=1 才會啟動）
    }
    return TRUE;
}
