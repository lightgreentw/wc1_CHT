#!/usr/bin/env bash
# build_msys2.sh — 在 MSYS2 MINGW32 shell 裡直接編譯，不需要 CMake
#
# 前置需求：
#   1. 安裝 MSYS2（https://www.msys2.org/）
#   2. 開啟「MSYS2 MINGW32」捷徑（開始選單裡，注意不是 MSYS2 MSYS
#      也不是 MSYS2 MINGW64 / UCRT64 —— WC1 是 32-bit，工具鏈也要 32-bit）
#   3. 安裝工具鏈：
#        pacman -S mingw-w64-i686-gcc make
#
# 使用方式：
#   cd wc1cht/
#   bash build_msys2.sh
#
# 產出：
#   build_msys2/wc1cht.dll
#   build_msys2/wc1cht_patch.exe

set -e

OUT_DIR="build_msys2"
mkdir -p "$OUT_DIR"

echo "== 檢查工具鏈 =="
if ! command -v g++ &> /dev/null; then
    echo "找不到 g++。請先在 MSYS2 MINGW32 shell 執行："
    echo "  pacman -S mingw-w64-i686-gcc make"
    exit 1
fi

# 確認是 32-bit 工具鏈（i686），不是 64-bit（x86_64）
GCC_TARGET=$(g++ -dumpmachine)
echo "偵測到工具鏈: $GCC_TARGET"
if [[ "$GCC_TARGET" != i686-* ]]; then
    echo ""
    echo "⚠  警告：目前的 g++ 是 $GCC_TARGET，不是 32-bit (i686-*)。"
    echo "   WC1 是 32-bit 遊戲，wc1cht.dll 和 wc1cht_patch.exe 都必須是 32-bit，"
    echo "   否則沒辦法注入 / 修補 32-bit 的 Wing1_wcdx.exe。"
    echo ""
    echo "   請確認你開啟的是「MSYS2 MINGW32」捷徑，不是 MINGW64 或 UCRT64，"
    echo "   並執行： pacman -S mingw-w64-i686-gcc"
    echo ""
    read -p "仍要繼續嗎？(y/N) " confirm
    [[ "$confirm" == "y" || "$confirm" == "Y" ]] || exit 1
fi

echo ""
echo "== 編譯 wc1cht.dll =="
g++ -O2 -Wall -shared -static -static-libgcc -static-libstdc++ -mstackrealign \
    -o "$OUT_DIR/wc1cht.dll" wc1cht.cpp \
    -lkernel32 -luser32 \
    -Wl,--kill-at

echo "== 編譯 wc1cht_patch.exe =="
g++ -O2 -Wall -static -static-libgcc -static-libstdc++ \
    -o "$OUT_DIR/wc1cht_patch.exe" wc1cht_patch.cpp \
    -lkernel32


echo ""
echo "== 完成 =="
ls -la "$OUT_DIR"
echo ""
echo "接下來："
echo "  1. cd $OUT_DIR"
echo "  2. ./wc1cht_patch.exe /path/to/Wing1_wcdx.exe"
echo "  3. 把 wc1cht.dll、wc1cht.ini、ini 指定的翻譯對照檔及字型檔 複製到遊戲目錄"
echo "  4. 執行 Wing1_wcdx_cht.exe（需要診斷 log 時，把 wc1cht.ini 的 [Log] Enable 設成 1，再查看 wc1cht_debug.log）"
