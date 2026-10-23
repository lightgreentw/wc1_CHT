/*
 * wc1cht_patch.cpp — Wing Commander 1 中文字幕 Detour Hook 安裝工具（雙 Hook 版）
 *
 * === 技術架構 ===
 *
 * 這一版同時安裝三個 Hook（第三個 Hook3 見檔尾說明；找不到時只會略過它，不影響前兩個）：
 *
 *   Hook 1：FUN_0044e215（函式名稱：DrawFormattedText）
 *     用途：星圖標籤等單行、帶 %X/%Y/%s 等逃逸碼的格式化文字
 *     （之前已經驗證成功，Nav 1/Asteroids/Tiger's Claw 這些）
 *
 *   Hook 2：FUN_0043ad4e（函式名稱：DrawTextString）
 *     用途：人物立繪對話這種多行、需要自動換行的長句子
 *     參數直接就是「已展開 $C/$N 之後的完整對話文字」，
 *     比 Hook 1 單純很多，不需要解析格式字串
 *
 * 兩個 hook 都是同一種 Detour Hook 手法（開頭若干 bytes 換成 JMP，
 * 原始 bytes 搬進 trampoline，跳回續接點），差別只在目標位址、
 * 使用的字型/續接位址，其餘機制完全共用。
 *
 * 使用方式：
 *   wc1cht_patch.exe Wing1_wcdx.exe        （本篇 WC1）
 *   wc1cht_patch.exe SM1_wcdx.exe          （資料片 1）
 *   wc1cht_patch.exe SM2_wcdx.exe          （資料片 2）
 *   wc1cht_patch.exe Transfer_wcdx.exe     （人物傳送工具；更早的 build，位址又完全不同，特徵碼照樣找得到）
 *   （在原檔旁建立 *_cht.exe，原始 exe 完全不動）
 *
 * === 多版本支援（WC1 / SM1 / SM2 共用同一支 patch 程式）===
 *   三個 exe 的引擎相同，但重新編譯過，函式位址不一樣
 *   （SM1 約 +0x30，SM2 約 +0x6D0，且各區段位移不同）。
 *   所以不再寫死位址，改用「遮蔽位址運算元的位元組特徵碼」在 .text 裡搜尋：
 *     DrawTextCharacter ：特徵碼唯一
 *     DrawTextString    ：特徵碼唯一（Hook2）
 *     DrawFormattedText ：特徵碼會同時命中幾個長得一樣的包裝函式，
 *                         再用「它 push 的回呼 thunk 最終跳到 DrawTextCharacter」
 *                         這個條件鎖定唯一的那一個（Hook1）
 *   也可用 --hook1=0xVA --hook2=0xVA 手動指定（找不到特徵碼的未知版本）。
 *
 * 編譯（MSVC，開發人員命令提示字元，32-bit）：
 *   cl /O2 /Fe:wc1cht_patch.exe wc1cht_patch.cpp
 *
 * 編譯（MSYS2 MINGW32 shell）：
 *   g++ -O2 -static -o wc1cht_patch.exe wc1cht_patch.cpp
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

// ─────────────────────────────────────────────
// 已知位址（靜態分析 + x32dbg 動態驗證）
// ─────────────────────────────────────────────

static const DWORD IMAGE_BASE = 0x00400000;

// 以下兩組是「WC1 原版」(Wing1_wcdx.exe) 的位址，只在 log 裡拿來對照，
// 實際位址一律由特徵碼搜尋決定（見 find_hook_targets）。
static const DWORD WC1_VA_HOOK1 = 0x0044E215;
static const DWORD WC1_VA_HOOK2 = 0x0043AD4E;
static const DWORD WC1_VA_HOOK3 = 0x0046B85D;      // DrawBigText（開場字幕／製作名單，精靈圖字型）

// Hook 1: DrawFormattedText —— x32dbg 動態驗證過，開頭 6 bytes 已知
static const BYTE  HOOK1_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x04 };
// Hook 2: DrawTextString —— 開頭 6 bytes（三個 exe 經比對一致）
static const BYTE  HOOK2_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20 };

// Hook 3: DrawBigText(Viewport*, short y, const char* text) —— cdecl，開頭 6 bytes
static const BYTE  HOOK3_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 };

// 已知的 exe 版本（PE TimeDateStamp），僅用於顯示
struct KnownExe { DWORD timestamp; const char* name; };
static const KnownExe KNOWN_EXES[] = {
    { 0x32821571, "WC1  Wing1_wcdx.exe" },
    { 0x3282101C, "SM1  SM1_wcdx.exe (Secret Missions 1)" },
    { 0x3294AA25, "SM2  SM2_wcdx.exe (Secret Missions 2)" },
    { 0x325D4FED, "TRF  Transfer_wcdx.exe (character transfer tool)" },
};

// 特徵碼（?? = 萬用）。由 WC1 產生：位址、相對跳轉位移的位元組都已遮蔽。
static const char* SIG_DRAW_CHAR =        // DrawTextCharacter(char c)  ret 4
    "55 8B EC 83 EC 08 53 56 57 0F BE 45 08 83 F8 0A 0F 85 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 00 "
    "66 8B 40 08 8B 0D ?? ?? ?? ?? 66 89 41 04";
static const char* SIG_DRAW_STRING =      // DrawTextString(const char*)  ret 4（Hook2）
    "55 8B EC 83 EC 20 53 56 57 C7 45 EC 00 00 00 00 8B 45 08 89 45 F8 C7 45 FC 00 00 00 00 "
    "83 7D EC 00 0F 85 ?? ?? ?? ?? A1 ?? ?? ?? ?? 0F BF 40 04 89 45 E0 8B 45 F8 0F BE 00 83 F8 20 "
    "0F 85 ?? ?? ?? ?? FF 45 F8 E9 ?? ?? ?? ?? 8B 45 F8";
static const char* SIG_DRAW_FORMATTED =   // DrawFormattedText(fmt, ...)（Hook1；會有同形姊妹函式，需再驗證）
    "55 8B EC 83 EC 04 53 56 57 8D 45 0C 89 45 FC 8B 45 FC 50 8B 45 08 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "83 C4 0C C7 45 FC 00 00 00 00 A1 ?? ?? ?? ?? 8B 00 8B 0D ?? ?? ?? ?? 39 08 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "5F 5E 5B C9";
// DrawBigText：開場「In the distant future…」與製作名單用的專用函式（用精靈圖字母逐字貼，自己置中、行距 16）。
// 特徵碼 = 函式開頭 0xC5 bytes（遮蔽唯一的 call 位移）。Transfer 沒有這個函式。
static const char* SIG_BIGTEXT =
    "55 8B EC 83 EC 18 53 56 57 66 C7 45 EC 01 00 8B 45 10 89 45 E8 8B 45 E8 "
    "8A 00 88 45 F4 FF 45 E8 0F BE 45 F4 85 C0 0F 84 16 00 00 00 0F BE 45 F4 "
    "83 F8 0A 0F 85 04 00 00 00 66 FF 45 EC E9 D3 FF FF FF 0F BF 45 EC C1 E0 "
    "04 66 89 45 FC B8 80 00 00 00 0F BF 4D FC 2B C1 99 2B C2 C1 F8 01 66 89 "
    "45 0C 8B 45 10 50 E8 ?? ?? ?? ?? 83 C4 04 66 89 45 F8 B8 40 01 00 00 0F "
    "BF 4D F8 2B C1 C1 F8 01 66 89 45 F0 8B 45 10 8A 00 88 45 F4 FF 45 10 0F "
    "BE 45 F4 85 C0 0F 84 9E 01 00 00 0F BE 45 F4 83 F8 41 0F 8C 6B 00 00 00 "
    "0F BE 45 F4 83 F8 7A 0F 8F 5E 00 00 00 0F BE 45 F4 83 E8 41 88 45 F4 66 "
    "0F BE 45 F4 50";
static const int SIG_DRAW_FORMATTED_CALLBACK_OFS = 24;   // 特徵碼內 "68 imm32" 的 imm32 位置

// ─────────────────────────────────────────────
// PE 工具函式
// ─────────────────────────────────────────────

static DWORD rva_to_offset(const std::vector<BYTE>& pe, DWORD rva) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)pe.data();
    const IMAGE_NT_HEADERS* nt  = (const IMAGE_NT_HEADERS*)(pe.data() + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    WORD num_sec = nt->FileHeader.NumberOfSections;
    for (int i = 0; i < num_sec; i++) {
        if (rva >= sec[i].VirtualAddress &&
            rva <  sec[i].VirtualAddress + sec[i].SizeOfRawData) {
            return sec[i].PointerToRawData + (rva - sec[i].VirtualAddress);
        }
    }
    return 0;
}

static DWORD offset_to_va(const std::vector<BYTE>& pe, DWORD file_off, DWORD image_base) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)pe.data();
    const IMAGE_NT_HEADERS* nt  = (const IMAGE_NT_HEADERS*)(pe.data() + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    WORD num_sec = nt->FileHeader.NumberOfSections;
    for (int i = 0; i < num_sec; i++) {
        if (file_off >= sec[i].PointerToRawData &&
            file_off <  sec[i].PointerToRawData + sec[i].SizeOfRawData) {
            return image_base + sec[i].VirtualAddress + (file_off - sec[i].PointerToRawData);
        }
    }
    return 0;
}

static DWORD find_code_cave(const std::vector<BYTE>& pe, int needed) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)pe.data();
    const IMAGE_NT_HEADERS* nt  = (const IMAGE_NT_HEADERS*)(pe.data() + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    WORD num_sec = nt->FileHeader.NumberOfSections;

    for (int i = 0; i < num_sec; i++) {
        if (strncmp((const char*)sec[i].Name, ".text", 5) != 0 &&
            strncmp((const char*)sec[i].Name, "CODE",  4) != 0) continue;

        DWORD raw_off  = sec[i].PointerToRawData;
        DWORD raw_sz   = sec[i].SizeOfRawData;
        DWORD virt_sz  = sec[i].Misc.VirtualSize;
        DWORD safe_sz  = (virt_sz != 0 && virt_sz < raw_sz) ? virt_sz : raw_sz;

        for (int j = (int)safe_sz - needed; j >= 0; j--) {
            bool ok = true;
            for (int k = 0; k < needed && ok; k++)
                if (pe[raw_off + j + k] != 0x00) ok = false;
            if (ok) return raw_off + j;
        }
    }
    return 0;
}

static DWORD align_up(DWORD value, DWORD alignment) {
    if (alignment == 0) return value;
    return (value + alignment - 1) / alignment * alignment;
}

struct NewSectionResult {
    DWORD section_va = 0;
    DWORD file_off   = 0;
};

// 新增 PE 節區（find_code_cave 找不到足夠空間時的主要方案，見先前版本
// 註解說明；這次支援多次呼叫，每次都在檔案最後面接續附加）
static NewSectionResult add_new_section(std::vector<BYTE>& pe, DWORD needed_size,
                                         DWORD image_base, const char* section_name) {
    NewSectionResult result;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)pe.data();
    IMAGE_NT_HEADERS*  nt  = (IMAGE_NT_HEADERS*)(pe.data() + dos->e_lfanew);

    DWORD file_align = nt->OptionalHeader.FileAlignment;
    DWORD sect_align = nt->OptionalHeader.SectionAlignment;

    DWORD sec_table_start = dos->e_lfanew + 4
                            + sizeof(IMAGE_FILE_HEADER)
                            + nt->FileHeader.SizeOfOptionalHeader;
    DWORD sec_table_end   = sec_table_start
                            + nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    WORD num_sec = nt->FileHeader.NumberOfSections;
    DWORD first_data_off = 0xFFFFFFFF;
    for (int i = 0; i < num_sec; i++) {
        if (sec[i].PointerToRawData && sec[i].PointerToRawData < first_data_off)
            first_data_off = sec[i].PointerToRawData;
    }

    DWORD gap = (first_data_off > sec_table_end) ? (first_data_off - sec_table_end) : 0;
    printf("  Section table ends at 0x%08lX, first section data starts at 0x%08lX, gap = %lu bytes\n",
           sec_table_end, first_data_off, gap);

    if (gap < sizeof(IMAGE_SECTION_HEADER)) {
        printf("  ERROR: gap is smaller than %zu bytes; cannot insert a new section header (%s)\n",
               sizeof(IMAGE_SECTION_HEADER), section_name);
        return result;
    }

    DWORD max_end_va = 0;
    for (int i = 0; i < num_sec; i++) {
        DWORD end_va = sec[i].VirtualAddress + sec[i].Misc.VirtualSize;
        if (end_va > max_end_va) max_end_va = end_va;
    }

    DWORD new_va_rva  = align_up(max_end_va, sect_align);
    DWORD new_raw_off = align_up((DWORD)pe.size(), file_align);
    DWORD new_raw_sz  = align_up(needed_size, file_align);
    DWORD new_va      = image_base + new_va_rva;

    printf("  New section %s: RVA=0x%08lX  VA=0x%08lX  FileOffset=0x%08lX  RawSize=0x%08lX\n",
           section_name, new_va_rva, new_va, new_raw_off, new_raw_sz);

    IMAGE_SECTION_HEADER new_sec = {};
    memcpy(new_sec.Name, section_name, std::min(strlen(section_name), (size_t)8));
    new_sec.Misc.VirtualSize     = needed_size;
    new_sec.VirtualAddress       = new_va_rva;
    new_sec.SizeOfRawData        = new_raw_sz;
    new_sec.PointerToRawData     = new_raw_off;
    new_sec.Characteristics      = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE |
                                    IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    memcpy(&pe[sec_table_end], &new_sec, sizeof(IMAGE_SECTION_HEADER));

    nt->FileHeader.NumberOfSections += 1;
    nt->OptionalHeader.SizeOfImage = align_up(new_va_rva + needed_size, sect_align);

    if (pe.size() < new_raw_off) pe.resize(new_raw_off, 0);
    pe.resize(new_raw_off + new_raw_sz, 0);

    result.section_va = new_va;
    result.file_off    = new_raw_off;
    return result;
}

struct KernelIat {
    DWORD load_library_a   = 0;
    DWORD get_proc_address = 0;
};

static KernelIat find_kernel_iat(const std::vector<BYTE>& pe, DWORD image_base) {
    KernelIat result;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)pe.data();
    const IMAGE_NT_HEADERS* nt  = (const IMAGE_NT_HEADERS*)(pe.data() + dos->e_lfanew);

    IMAGE_DATA_DIRECTORY import_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!import_dir.VirtualAddress) return result;

    DWORD imt_off = rva_to_offset(pe, import_dir.VirtualAddress);
    if (!imt_off) return result;

    const IMAGE_IMPORT_DESCRIPTOR* imd =
        (const IMAGE_IMPORT_DESCRIPTOR*)(pe.data() + imt_off);

    while (imd->Name) {
        DWORD name_off = rva_to_offset(pe, imd->Name);
        const char* dll_name = (const char*)(pe.data() + name_off);

        if (_stricmp(dll_name, "kernel32.dll") == 0) {
            DWORD iat_off = rva_to_offset(pe, imd->FirstThunk);
            DWORD int_rva = imd->OriginalFirstThunk ? imd->OriginalFirstThunk : imd->FirstThunk;
            DWORD int_off = rva_to_offset(pe, int_rva);

            const IMAGE_THUNK_DATA* iat  = (const IMAGE_THUNK_DATA*)(pe.data() + iat_off);
            const IMAGE_THUNK_DATA* hint = (const IMAGE_THUNK_DATA*)(pe.data() + int_off);

            for (int i = 0; iat[i].u1.Function; i++) {
                if (hint[i].u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
                DWORD hint_off = rva_to_offset(pe, (DWORD)hint[i].u1.AddressOfData);
                const char* fn_name = (const char*)(pe.data() + hint_off + 2);
                DWORD iat_va = image_base + imd->FirstThunk + i * 4;

                if (strcmp(fn_name, "LoadLibraryA") == 0)   result.load_library_a   = iat_va;
                if (strcmp(fn_name, "GetProcAddress") == 0) result.get_proc_address = iat_va;
            }
        }
        imd++;
    }
    return result;
}

// ─────────────────────────────────────────────
// Prologue 長度自動偵測（給沒有事先用 x32dbg 驗證過確切 bytes
// 的 Hook 2 用；涵蓋 MSVC 最常見的三種函式開頭樣式）
// ─────────────────────────────────────────────
//
//   樣式 A（6 bytes）：55 8B EC 83 EC XX          push ebp; mov ebp,esp; sub esp,imm8
//   樣式 B（9 bytes）：55 8B EC 81 EC XX XX XX XX  push ebp; mov ebp,esp; sub esp,imm32
//   樣式 C（3 bytes）：55 8B EC                    push ebp; mov ebp,esp（無區域變數配置）
//
// 這三種涵蓋絕大多數簡單 C 函式的開頭。偵測失敗回傳 0，
// 呼叫端要自行決定是否中止。

static int detect_prologue_len(const BYTE* p) {
    if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC) return 0;   // 不是標準 push ebp;mov ebp,esp 開頭
    if (p[3] == 0x83 && p[4] == 0xEC) return 6;   // 樣式 A: sub esp, imm8
    if (p[3] == 0x81 && p[4] == 0xEC) return 9;   // 樣式 B: sub esp, imm32
    return 3;                                      // 樣式 C: 沒有區域變數配置
}

// ─────────────────────────────────────────────
// 特徵碼搜尋（多版本支援）
// ─────────────────────────────────────────────

static std::vector<int> parse_pattern(const char* s) {
    std::vector<int> out;
    const char* p = s;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') { out.push_back(-1); p += 2; continue; }
        char* end = nullptr;
        long v = strtol(p, &end, 16);
        if (end == p) break;
        out.push_back((int)v);
        p = end;
    }
    return out;
}

// 在所有程式碼節區裡找符合特徵碼的位置，回傳 VA 清單
static std::vector<DWORD> scan_code_sections(const std::vector<BYTE>& pe, DWORD image_base,
                                              const char* pattern_str) {
    std::vector<DWORD> hits;
    std::vector<int> pat = parse_pattern(pattern_str);
    if (pat.empty()) return hits;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)pe.data();
    const IMAGE_NT_HEADERS* nt  = (const IMAGE_NT_HEADERS*)(pe.data() + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        if (strncmp((const char*)sec[i].Name, ".wc1cht", 7) == 0) continue;   // 自己加的節區不搜
        DWORD raw = sec[i].PointerToRawData, sz = sec[i].SizeOfRawData;
        if (raw + sz > pe.size()) sz = (DWORD)pe.size() - raw;
        for (DWORD j = 0; j + pat.size() <= sz; j++) {
            bool ok = true;
            for (size_t k = 0; k < pat.size() && ok; k++)
                if (pat[k] >= 0 && pe[raw + j + k] != (BYTE)pat[k]) ok = false;
            if (ok) hits.push_back(image_base + sec[i].VirtualAddress + j);
        }
    }
    return hits;
}

// JMP rel32 thunk（E9 xx xx xx xx）→ 最終目標；不是 thunk 就回傳 false
static bool resolve_jmp_thunk(const std::vector<BYTE>& pe, DWORD image_base, DWORD thunk_va, DWORD& target) {
    DWORD off = rva_to_offset(pe, thunk_va - image_base);
    if (!off || off + 5 > pe.size() || pe[off] != 0xE9) return false;
    int rel = *(const int*)&pe[off + 1];
    target = thunk_va + 5 + (DWORD)rel;
    return true;
}

struct HookTargets {
    DWORD draw_char = 0;        // DrawTextCharacter（不 hook，僅用來鎖定 Hook1）
    DWORD hook1 = 0;            // DrawFormattedText
    DWORD hook2 = 0;            // DrawTextString
    DWORD hook3 = 0;            // DrawBigText（開場字幕／製作名單）
};

static HookTargets find_hook_targets(const std::vector<BYTE>& pe, DWORD image_base) {
    HookTargets t;

    std::vector<DWORD> h = scan_code_sections(pe, image_base, SIG_DRAW_CHAR);
    printf("[Signature] DrawTextCharacter: %zu match(es)", h.size());
    for (DWORD v : h) printf("  0x%08lX", (unsigned long)v);
    printf("\n");
    if (h.size() == 1) t.draw_char = h[0];

    h = scan_code_sections(pe, image_base, SIG_DRAW_STRING);
    printf("[Signature] DrawTextString   : %zu match(es)", h.size());
    for (DWORD v : h) printf("  0x%08lX", (unsigned long)v);
    printf("\n");
    if (h.size() == 1) t.hook2 = h[0];

    h = scan_code_sections(pe, image_base, SIG_BIGTEXT);
    printf("[Signature] DrawBigText      : %zu match(es)", h.size());
    for (DWORD v : h) printf("  0x%08lX", (unsigned long)v);
    printf("\n");
    if (h.size() == 1) t.hook3 = h[0];

    h = scan_code_sections(pe, image_base, SIG_DRAW_FORMATTED);
    printf("[Signature] DrawFormattedText: %zu match(es) (includes a look-alike sibling function; filtered by callback thunk)", h.size());
    for (DWORD v : h) printf("  0x%08lX", (unsigned long)v);
    printf("\n");
    if (t.draw_char) {
        std::vector<DWORD> good;
        for (DWORD v : h) {
            DWORD off = rva_to_offset(pe, v - image_base);
            if (!off) continue;
            DWORD cb = *(const DWORD*)&pe[off + SIG_DRAW_FORMATTED_CALLBACK_OFS];
            DWORD tgt = 0;
            bool thunk = resolve_jmp_thunk(pe, image_base, cb, tgt);
            printf("           Candidate 0x%08lX: callback 0x%08lX -> %s0x%08lX%s\n", (unsigned long)v, (unsigned long)cb,
                   thunk ? "thunk target " : "not a thunk, ", (unsigned long)(thunk ? tgt : cb),
                   (thunk && tgt == t.draw_char) ? "  <- equals DrawTextCharacter, selected" : "");
            if (thunk && tgt == t.draw_char) good.push_back(v);
        }
        if (good.size() == 1) t.hook1 = good[0];
    } else if (h.size() == 1) {
        t.hook1 = h[0];      // 沒有 DrawTextCharacter 可交叉驗證，但特徵碼唯一
    }
    return t;
}

static bool parse_hex_arg(const char* a, const char* key, DWORD& out) {
    size_t n = strlen(key);
    if (strncmp(a, key, n) != 0) return false;
    out = (DWORD)strtoul(a + n, nullptr, 0);
    return true;
}

// ─────────────────────────────────────────────
// x86 位元組碼組裝工具（邏輯已用 Python 模擬器驗證過）
// ─────────────────────────────────────────────

struct Assembler {
    std::vector<BYTE> buf;
    DWORD base_va;

    explicit Assembler(DWORD base) : base_va(base) {}

    size_t pos() const { return buf.size(); }
    DWORD  va()  const { return base_va + (DWORD)buf.size(); }

    void b(BYTE v) { buf.push_back(v); }
    void dw(DWORD v) {
        buf.push_back(v & 0xFF);
        buf.push_back((v >> 8) & 0xFF);
        buf.push_back((v >> 16) & 0xFF);
        buf.push_back((v >> 24) & 0xFF);
    }

    void push_imm32(DWORD v)       { b(0x68); dw(v); }
    void push_eax()                 { b(0x50); }
    void pushad()                    { b(0x60); }
    void popad()                     { b(0x61); }
    void test_eax_eax()             { b(0x85); b(0xC0); }
    void mov_mem32_eax(DWORD addr)  { b(0xA3); dw(addr); }
    void mov_eax_mem32(DWORD addr)  { b(0xA1); dw(addr); }
    void cmp_mem32_imm32(DWORD addr, DWORD imm) { b(0x81); b(0x3D); dw(addr); dw(imm); }
    void mov_mem32_imm32(DWORD addr, DWORD imm) { b(0xC7); b(0x05); dw(addr); dw(imm); }
    void call_indirect_iat(DWORD iat_va) { b(0xFF); b(0x15); dw(iat_va); }
    void call_eax()                       { b(0xFF); b(0xD0); }

    size_t jz_short_ph()  { b(0x74); b(0x00); return buf.size() - 1; }
    size_t je_short_ph()  { b(0x74); b(0x00); return buf.size() - 1; }
    size_t jne_short_ph() { b(0x75); b(0x00); return buf.size() - 1; }
    size_t jmp_short_ph() { b(0xEB); b(0x00); return buf.size() - 1; }

    void backpatch(size_t delta_pos, size_t target_pos) {
        int delta = (int)target_pos - (int)delta_pos - 1;
        buf[delta_pos] = (BYTE)(delta & 0xFF);
    }

    void jmp_rel32_to_va(DWORD to_va) {
        DWORD from_va = va() + 5;
        b(0xE9); dw(to_va - from_va);
    }
    void jmp_indirect(DWORD mem_addr) { b(0xFF); b(0x25); dw(mem_addr); }

    void raw_bytes(const BYTE* data, int len) { for (int i = 0; i < len; i++) b(data[i]); }
};

// ─────────────────────────────────────────────
// 通用 Hook 安裝函式（Hook 1 / Hook 2 共用同一套邏輯）
// ─────────────────────────────────────────────

struct HookConfig {
    const char* label;              // 用於 log 輸出的名稱，例如 "Hook1(DrawFormattedText)"
    DWORD       target_va_base;     // 目標函式位址（尚未套用 addr_adjust）
    const BYTE* known_prologue;     // 若已知確切 bytes 填這裡；不知道填 nullptr
    int         known_prologue_len; // known_prologue 的長度；跟上面一起用
    const char* hook_fn_name;       // wc1cht.dll 裡負責處理的函式名稱
    const char* setter_fn_name;     // wc1cht.dll 裡設定 trampoline 位址的函式名稱
    const char* section_name;       // 這個 hook 專屬節區的名稱（8 bytes 內）
};

struct HookInstallResult {
    bool  ok = false;
    DWORD hook_va = 0;
    DWORD dispatcher_va = 0;
    DWORD trampoline_va = 0;
    DWORD continue_va = 0;
};

static HookInstallResult install_hook(std::vector<BYTE>& pe, DWORD image_base,
                                       const KernelIat& kiat, const HookConfig& cfg) {
    HookInstallResult result;

    DWORD addr_adjust = image_base - IMAGE_BASE;
    DWORD hook_va = cfg.target_va_base + addr_adjust;

    DWORD hook_off = rva_to_offset(pe, hook_va - image_base);
    if (!hook_off) {
        fprintf(stderr, "[%s] Cannot locate target address (VA=0x%08lX)\n", cfg.label, hook_va);
        return result;
    }
    printf("[%s] Target: VA=0x%08lX  FileOffset=0x%08lX\n", cfg.label, hook_va, hook_off);

    // ── 決定 prologue 長度 ──
    int prologue_len;
    if (cfg.known_prologue) {
        if (memcmp(&pe[hook_off], cfg.known_prologue, cfg.known_prologue_len) != 0) {
            printf("[%s] Current first %d bytes: ", cfg.label, cfg.known_prologue_len);
            for (int i = 0; i < cfg.known_prologue_len; i++) printf("%02X ", pe[hook_off + i]);
            printf("\n[%s] Bytes do not match the expected prologue (already patched, or a different version). Aborting.\n", cfg.label);
            return result;
        }
        prologue_len = cfg.known_prologue_len;
        printf("[%s] Prologue verified (matches the known bytes)\n", cfg.label);
    } else {
        prologue_len = detect_prologue_len(&pe[hook_off]);
        printf("[%s] Current first 8 bytes: ", cfg.label);
        for (int i = 0; i < 8; i++) printf("%02X ", pe[hook_off + i]);
        printf("\n");
        if (prologue_len == 0) {
            fprintf(stderr,
                "[%s] Function does not start with the standard 55 8B EC (push ebp; mov ebp,esp),\n"
                "   so a safe prologue length cannot be determined. Skipping this hook\n"
                "   (the other hooks are not affected and will still be installed).\n", cfg.label);
            return result;
        }
        printf("[%s] Auto-detected prologue length = %d bytes (pattern %s)\n",
               cfg.label, prologue_len,
               prologue_len == 6 ? "A: sub esp,imm8" :
               prologue_len == 9 ? "B: sub esp,imm32" : "C: no local variables");
    }

    DWORD continue_va = hook_va + prologue_len;

    const int TRAMPOLINE_LEN = prologue_len + 5;
    const int DISPATCHER_MAX = 160;
    const char DLL_NAME[] = "wc1cht.dll";
    int dll_name_len  = (int)strlen(DLL_NAME) + 1;
    int hook_name_len = (int)strlen(cfg.hook_fn_name) + 1;
    int sett_name_len = (int)strlen(cfg.setter_fn_name) + 1;

    const int STUB_MAX_SIZE = TRAMPOLINE_LEN + DISPATCHER_MAX + 12
                             + dll_name_len + hook_name_len + sett_name_len + 32;

    DWORD cave_off, cave_va;
    {
        NewSectionResult new_sec = add_new_section(pe, STUB_MAX_SIZE, image_base, cfg.section_name);
        if (new_sec.section_va) {
            cave_va  = new_sec.section_va;
            cave_off = new_sec.file_off;
            printf("[%s] Added dedicated section %s\n", cfg.label, cfg.section_name);
        } else {
            printf("[%s] Not enough room in the section table; searching .text for an existing code cave...\n", cfg.label);
            cave_off = find_code_cave(pe, STUB_MAX_SIZE);
            if (!cave_off) {
                fprintf(stderr, "[%s] Could not find a large enough code cave (need about %d bytes)\n",
                        cfg.label, STUB_MAX_SIZE);
                return result;
            }
            cave_va = offset_to_va(pe, cave_off, image_base);
        }
    }
    printf("[%s] Code cave: FileOffset=0x%08lX  VA=0x%08lX\n", cfg.label, cave_off, cave_va);

    {
        DWORD end_va = cave_va + STUB_MAX_SIZE - 1;
        DWORD end_off = rva_to_offset(pe, end_va - image_base);
        if (!end_off) {
            fprintf(stderr, "[%s] Safety check failed: the cave end address does not map to a valid file offset\n", cfg.label);
            return result;
        }
        printf("[%s] Second verification passed\n", cfg.label);
    }

    DWORD trampoline_va = cave_va;
    DWORD dispatcher_va = trampoline_va + TRAMPOLINE_LEN;

    DWORD data_hook_ptr_va  = dispatcher_va + DISPATCHER_MAX;
    DWORD data_state_va     = data_hook_ptr_va + 4;
    DWORD data_hmodule_va   = data_state_va + 4;
    DWORD data_dll_name_va  = data_hmodule_va + 4;
    DWORD data_proc_hook_va = data_dll_name_va + dll_name_len;
    DWORD data_proc_sett_va = data_proc_hook_va + hook_name_len;

    Assembler tramp(trampoline_va);
    tramp.raw_bytes(&pe[hook_off], prologue_len);
    tramp.jmp_rel32_to_va(continue_va);
    if ((int)tramp.pos() != TRAMPOLINE_LEN) {
        fprintf(stderr, "[%s] Internal error: trampoline length mismatch (%zu != %d)\n",
                cfg.label, tramp.pos(), TRAMPOLINE_LEN);
        return result;
    }

    Assembler disp(dispatcher_va);

    disp.cmp_mem32_imm32(data_state_va, 0);
    size_t jne_skip_init = disp.jne_short_ph();

    disp.pushad();

    disp.push_imm32(data_dll_name_va);
    disp.call_indirect_iat(kiat.load_library_a);
    disp.test_eax_eax();
    size_t jz_fail_1 = disp.jz_short_ph();
    disp.mov_mem32_eax(data_hmodule_va);

    disp.push_imm32(data_proc_hook_va);
    disp.mov_eax_mem32(data_hmodule_va);
    disp.push_eax();
    disp.call_indirect_iat(kiat.get_proc_address);
    disp.test_eax_eax();
    size_t jz_fail_2 = disp.jz_short_ph();
    disp.mov_mem32_eax(data_hook_ptr_va);

    disp.push_imm32(data_proc_sett_va);
    disp.mov_eax_mem32(data_hmodule_va);
    disp.push_eax();
    disp.call_indirect_iat(kiat.get_proc_address);
    disp.test_eax_eax();
    size_t jz_fail_3 = disp.jz_short_ph();
    disp.push_imm32(trampoline_va);
    disp.call_eax();

    disp.mov_mem32_imm32(data_state_va, 1);
    size_t jmp_init_done = disp.jmp_short_ph();

    size_t init_failed_pos = disp.pos();
    disp.mov_mem32_imm32(data_state_va, 2);

    size_t init_done_pos = disp.pos();
    disp.popad();

    size_t skip_init_pos = disp.pos();
    disp.cmp_mem32_imm32(data_state_va, 2);
    size_t je_call_tramp = disp.je_short_ph();

    disp.jmp_indirect(data_hook_ptr_va);

    size_t call_tramp_pos = disp.pos();
    disp.jmp_rel32_to_va(trampoline_va);

    disp.backpatch(jne_skip_init, skip_init_pos);
    disp.backpatch(jz_fail_1,     init_failed_pos);
    disp.backpatch(jz_fail_2,     init_failed_pos);
    disp.backpatch(jz_fail_3,     init_failed_pos);
    disp.backpatch(jmp_init_done, init_done_pos);
    disp.backpatch(je_call_tramp, call_tramp_pos);

    printf("[%s] Dispatcher length: %zu bytes (reserved %d)\n", cfg.label, disp.pos(), DISPATCHER_MAX);
    if ((int)disp.pos() > DISPATCHER_MAX) {
        fprintf(stderr, "[%s] Internal error: dispatcher exceeds the reserved space\n", cfg.label);
        return result;
    }

    DWORD total_needed = (data_proc_sett_va + (DWORD)sett_name_len) - cave_va;
    if (total_needed > (DWORD)STUB_MAX_SIZE) {
        fprintf(stderr, "[%s] Internal error: total size %lu exceeds the reserved %d bytes\n",
                cfg.label, total_needed, STUB_MAX_SIZE);
        return result;
    }

    memcpy(&pe[cave_off], tramp.buf.data(), tramp.buf.size());
    memcpy(&pe[cave_off + (dispatcher_va - trampoline_va)], disp.buf.data(), disp.buf.size());

    DWORD hook_ptr_off  = cave_off + (data_hook_ptr_va  - trampoline_va);
    DWORD state_off     = cave_off + (data_state_va     - trampoline_va);
    DWORD hmodule_off   = cave_off + (data_hmodule_va   - trampoline_va);
    DWORD dll_name_off  = cave_off + (data_dll_name_va  - trampoline_va);
    DWORD proc_hook_off = cave_off + (data_proc_hook_va - trampoline_va);
    DWORD proc_sett_off = cave_off + (data_proc_sett_va - trampoline_va);

    *(DWORD*)&pe[hook_ptr_off] = 0;
    *(DWORD*)&pe[state_off]    = 0;
    *(DWORD*)&pe[hmodule_off]  = 0;
    memcpy(&pe[dll_name_off],  DLL_NAME,           dll_name_len);
    memcpy(&pe[proc_hook_off], cfg.hook_fn_name,   hook_name_len);
    memcpy(&pe[proc_sett_off], cfg.setter_fn_name, sett_name_len);

    Assembler entry_jmp(hook_va);
    entry_jmp.jmp_rel32_to_va(dispatcher_va);
    memcpy(&pe[hook_off], entry_jmp.buf.data(), entry_jmp.buf.size());

    printf("[%s] Hook installed\n\n", cfg.label);

    result.ok = true;
    result.hook_va = hook_va;
    result.dispatcher_va = dispatcher_va;
    result.trampoline_va = trampoline_va;
    result.continue_va = continue_va;
    return result;
}

// ─────────────────────────────────────────────
// 主程式
// ─────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: wc1cht_patch.exe <Wing1_wcdx.exe | SM1_wcdx.exe | SM2_wcdx.exe | Transfer_wcdx.exe> [--hook1=0xVA] [--hook2=0xVA] [--hook3=0xVA]\n");
        printf("      Writes *_cht.exe next to the input file (the original is left untouched)\n");
        return 1;
    }
    DWORD force_hook1 = 0, force_hook2 = 0, force_hook3 = 0;
    for (int i = 2; i < argc; i++) {
        if (!parse_hex_arg(argv[i], "--hook1=", force_hook1) &&
            !parse_hex_arg(argv[i], "--hook2=", force_hook2) &&
            !parse_hex_arg(argv[i], "--hook3=", force_hook3))
            fprintf(stderr, "Ignoring unknown argument: %s\n", argv[i]);
    }

    FILE* fp = fopen(argv[1], "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp); rewind(fp);
    std::vector<BYTE> pe(sz);
    fread(pe.data(), 1, sz, fp);
    fclose(fp);

    if (pe.size() < sizeof(IMAGE_DOS_HEADER)) { fprintf(stderr, "File is too small\n"); return 1; }
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)pe.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { fprintf(stderr, "Not a valid PE file\n"); return 1; }
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(pe.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { fprintf(stderr, "Not a valid NT PE header\n"); return 1; }

    DWORD image_base = nt->OptionalHeader.ImageBase;
    printf("ImageBase: 0x%08lX\n", image_base);
    {
        const IMAGE_SECTION_HEADER* s0 = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (strncmp((const char*)s0[i].Name, ".wc1cht", 7) == 0) {
                fprintf(stderr, "This exe already has a %.8s section, so it has already been patched. Please use the original (unpatched) exe.\n",
                        (const char*)s0[i].Name);
                return 1;
            }
        DWORD ts = nt->FileHeader.TimeDateStamp;
        const char* known = nullptr;
        for (const KnownExe& k : KNOWN_EXES) if (k.timestamp == ts) known = k.name;
        printf("PE TimeDateStamp: 0x%08lX  -> %s\n\n", (unsigned long)ts,
               known ? known : "unknown version (using signature search; after patching, enable logging in wc1cht.ini and check wc1cht_debug.log)");
    }

    KernelIat kiat = find_kernel_iat(pe, image_base);
    if (!kiat.load_library_a || !kiat.get_proc_address) {
        fprintf(stderr, "Could not find the IAT entries for LoadLibraryA/GetProcAddress\n");
        return 1;
    }
    printf("LoadLibraryA   IAT: 0x%08lX\n", kiat.load_library_a);
    printf("GetProcAddress IAT: 0x%08lX\n\n", kiat.get_proc_address);

    HookTargets tg = find_hook_targets(pe, image_base);
    if (force_hook1) { tg.hook1 = force_hook1; printf("[Manual override] Hook1 = 0x%08lX\n", (unsigned long)tg.hook1); }
    if (force_hook2) { tg.hook2 = force_hook2; printf("[Manual override] Hook2 = 0x%08lX\n", (unsigned long)tg.hook2); }
    if (force_hook3) { tg.hook3 = force_hook3; printf("[Manual override] Hook3 = 0x%08lX\n", (unsigned long)tg.hook3); }
    printf("\nResolved: Hook1(DrawFormattedText)=0x%08lX (WC1: 0x%08lX, offset %+ld)\n",
           (unsigned long)tg.hook1, (unsigned long)WC1_VA_HOOK1, (long)tg.hook1 - (long)WC1_VA_HOOK1);
    printf("          Hook2(DrawTextString)   =0x%08lX (WC1: 0x%08lX, offset %+ld)\n",
           (unsigned long)tg.hook2, (unsigned long)WC1_VA_HOOK2, (long)tg.hook2 - (long)WC1_VA_HOOK2);
    printf("          Hook3(DrawBigText)      =0x%08lX (WC1: 0x%08lX, offset %+ld)\n\n",
           (unsigned long)tg.hook3, (unsigned long)WC1_VA_HOOK3, (long)tg.hook3 - (long)WC1_VA_HOOK3);

    // ── Hook 1: DrawFormattedText（星圖標籤等單行格式化文字）──
    HookConfig cfg1;
    cfg1.label              = "Hook1(DrawFormattedText)";
    cfg1.target_va_base     = tg.hook1 ? tg.hook1 - (image_base - IMAGE_BASE) : 0;
    cfg1.known_prologue     = HOOK1_PROLOGUE;
    cfg1.known_prologue_len = sizeof(HOOK1_PROLOGUE);
    cfg1.hook_fn_name       = "hook_draw_string";
    cfg1.setter_fn_name     = "wc1cht_set_trampoline";
    cfg1.section_name       = ".wc1cht1";

    HookInstallResult r1;
    if (tg.hook1) r1 = install_hook(pe, image_base, kiat, cfg1);
    else fprintf(stderr, "[Hook1] DrawFormattedText not found (specify it manually with --hook1=0xVA)\n");

    // ── Hook 2: DrawTextString（人物對話，需要自動換行）──
    HookConfig cfg2;
    cfg2.label              = "Hook2(DrawTextString)";
    cfg2.target_va_base     = tg.hook2 ? tg.hook2 - (image_base - IMAGE_BASE) : 0;
    cfg2.known_prologue     = HOOK2_PROLOGUE;     // 三個 exe 都是 55 8B EC 83 EC 20
    cfg2.known_prologue_len = sizeof(HOOK2_PROLOGUE);
    cfg2.hook_fn_name       = "hook_draw_dialogue";
    cfg2.setter_fn_name     = "wc1cht_set_trampoline2";
    cfg2.section_name       = ".wc1cht2";

    HookInstallResult r2;
    if (tg.hook2) r2 = install_hook(pe, image_base, kiat, cfg2);
    else fprintf(stderr, "[Hook2] DrawTextString not found (specify it manually with --hook2=0xVA)\n");

    // ── Hook 3: DrawBigText（開場字幕／製作名單；Transfer 沒有，找不到就略過）──
    HookConfig cfg3;
    cfg3.label              = "Hook3(DrawBigText)";
    cfg3.target_va_base     = tg.hook3 ? tg.hook3 - (image_base - IMAGE_BASE) : 0;
    cfg3.known_prologue     = HOOK3_PROLOGUE;
    cfg3.known_prologue_len = sizeof(HOOK3_PROLOGUE);
    cfg3.hook_fn_name       = "hook_draw_bigtext";
    cfg3.setter_fn_name     = "wc1cht_set_trampoline3";
    cfg3.section_name       = ".wc1cht3";

    HookInstallResult r3;
    if (tg.hook3) r3 = install_hook(pe, image_base, kiat, cfg3);
    else printf("[Hook3] This exe has no DrawBigText (normal for the Transfer tool; for other versions use --hook3=0xVA). Skipped.\n");

    if (!r1.ok && !r2.ok && !r3.ok) {
        fprintf(stderr, "All hooks failed to install; no file written.\n");
        return 1;
    }
    if (tg.hook3 && !r3.ok) printf("Hook3 (intro captions) failed to install; they will stay in English. The other hooks are not affected.\n");
    if (!r1.ok) printf("Hook1 (star-map labels) failed to install; they will not be translated, but Hook2 is still written.\n");
    if (!r2.ok) printf("Hook2 (character dialogue) failed to install; it will not be translated, but Hook1 is still written.\n");

    char out_path[MAX_PATH];
    strncpy(out_path, argv[1], MAX_PATH - 12);
    out_path[MAX_PATH - 12] = 0;
    char* dot = strrchr(out_path, '.');
    if (dot) {
        memmove(dot + 4, dot, strlen(dot) + 1);
        memcpy(dot, "_cht", 4);
    } else {
        strncat(out_path, "_cht", MAX_PATH - strlen(out_path) - 1);
    }

    FILE* out = fopen(out_path, "wb");
    if (!out) { fprintf(stderr, "Cannot write %s\n", out_path); return 1; }
    fwrite(pe.data(), 1, pe.size(), out);
    fclose(out);

    printf("\nSuccess! Output: %s\n", out_path);
    if (r1.ok) {
        printf("\nHook1 technical summary:\n");
        printf("  Trampoline VA: 0x%08lX (jumps back to 0x%08lX)\n", r1.trampoline_va, r1.continue_va);
        printf("  Hook entry:    0x%08lX -> JMP 0x%08lX\n", r1.hook_va, r1.dispatcher_va);
    }
    if (r2.ok) {
        printf("\nHook2 technical summary:\n");
        printf("  Trampoline VA: 0x%08lX (jumps back to 0x%08lX)\n", r2.trampoline_va, r2.continue_va);
        printf("  Hook entry:    0x%08lX -> JMP 0x%08lX\n", r2.hook_va, r2.dispatcher_va);
    }
    if (r3.ok) {
        printf("\nHook3 technical summary:\n");
        printf("  Trampoline VA: 0x%08lX (jumps back to 0x%08lX)\n", r3.trampoline_va, r3.continue_va);
        printf("  Hook entry:    0x%08lX -> JMP 0x%08lX\n", r3.hook_va, r3.dispatcher_va);
    }
    printf("\nDeploy: put wc1cht.dll / wc1cht.ini / the translation file and font files named in wc1cht.ini\n        in the same folder as %s,\n", out_path);
    printf("then run %s instead of the original exe.\n", out_path);

    return 0;
}
