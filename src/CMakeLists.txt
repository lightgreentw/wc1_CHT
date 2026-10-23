cmake_minimum_required(VERSION 3.16)
project(wc1cht CXX)

# ── 必須用 32-bit 工具鏈 ──
# WC1 是 32-bit PE，DLL 和 patcher 都必須是 32-bit。
#
# MSVC：
#   cmake -A Win32 -B build
#   cmake --build build --config Release
#
# MSYS2（在 MINGW32 shell 裡，不是 MINGW64！）：
#   cmake -G "MinGW Makefiles" -B build
#   cmake --build build

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "必須以 32-bit 模式編譯！\n"
        "MSVC：  cmake -A Win32 -B build\n"
        "MSYS2： 請從「MSYS2 MINGW32」捷徑開啟終端機（不是 MINGW64 或 UCRT64），\n"
        "        並確認已安裝：pacman -S mingw-w64-i686-toolchain mingw-w64-i686-cmake")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 依編譯器分別設定選項 ──
if(MSVC)
    add_compile_options(
        /O2 /W3 /MT /EHsc
        /DWIN32 /D_WINDOWS /DNDEBUG
    )
else()
    # MinGW / GCC（含 MSYS2）
    add_compile_options(
        -O2 -Wall
        -static-libgcc -static-libstdc++
        -DWIN32 -DNDEBUG
    )
    add_link_options(-static)
endif()

# ════════════════════════════════════════════════
# wc1cht.dll — 中文字幕 Hook DLL
# ════════════════════════════════════════════════
add_library(wc1cht SHARED wc1cht.cpp)

target_link_libraries(wc1cht PRIVATE kernel32 user32)

set_target_properties(wc1cht PROPERTIES
    OUTPUT_NAME "wc1cht"
    PREFIX      ""
    SUFFIX      ".dll"
)

target_compile_definitions(wc1cht PRIVATE _USRDLL WC1CHT_EXPORTS)

if(NOT MSVC)
    # MinGW 預設會把匯出符號改名成 _hook_draw_string@4 這種 stdcall 修飾格式，
    # 但我們的 extern "C" __declspec(dllexport) 搭配 __cdecl 已經避免大部分問題；
    # 為保險起見，__stdcall 的 wc1cht_set_trampoline 仍需要 --kill-at
    # 移除 @N 修飾，確保 wc1cht_patch.exe 用 GetProcAddress("wc1cht_set_trampoline")
    # 能找到，不需要帶 @4 後綴。
    target_link_options(wc1cht PRIVATE -Wl,--kill-at)
endif()

# ════════════════════════════════════════════════
# wc1cht_patch.exe — Detour Hook 安裝工具
# ════════════════════════════════════════════════
add_executable(wc1cht_patch wc1cht_patch.cpp)

target_link_libraries(wc1cht_patch PRIVATE kernel32)

set_target_properties(wc1cht_patch PROPERTIES
    OUTPUT_NAME "wc1cht_patch"
    SUFFIX      ".exe"
)

# ════════════════════════════════════════════════
# 安裝目標（可選）
# cmake --install build --prefix <遊戲目錄>
# ════════════════════════════════════════════════
install(TARGETS wc1cht wc1cht_patch
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
)

add_custom_command(TARGET wc1cht POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "===================================================="
    COMMAND ${CMAKE_COMMAND} -E echo "wc1cht.dll 建置完成（工具鏈: ${CMAKE_CXX_COMPILER_ID}）"
    COMMAND ${CMAKE_COMMAND} -E echo "部署順序："
    COMMAND ${CMAKE_COMMAND} -E echo "  1. wc1cht_patch.exe Wing1_wcdx.exe"
    COMMAND ${CMAKE_COMMAND} -E echo "  2. 複製 wc1cht.dll + wc1cht.ini + ini 指定的翻譯對照檔及字型檔 到遊戲目錄"
    COMMAND ${CMAKE_COMMAND} -E echo "  3. 執行 Wing1_wcdx_cht.exe"
    COMMAND ${CMAKE_COMMAND} -E echo "===================================================="
)
