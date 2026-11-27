#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wc1spr_tool.py — 把「中文版圖片（PCX）」編成 wc1cht.dll 能在執行階段替換的精靈包 wc1cht_spr.bin

背景
  遊戲裡有些英文是烘進圖資的（例如進入遊戲前選單的 Start Vega Campaign / Continue Campaign），
  不是文字繪製，沒有文字可以翻。wc1cht.dll 的 Hook4 會攔截遊戲的「精靈繪製」函式，
  遇到跟包裡某個「原始精靈」內容相同的精靈，就改畫包裡的中文版。原始 GAMEDAT 一個位元組都不會動。

用法
  1) 列出可替換的精靈（找出要替換哪張）：
       wc1spr_tool.py scan  <GAMEDAT 目錄> [--size 236x38] [--files TITLE.VGA,TITLE1.VGA]
  2) 轉換並打包：
       wc1spr_tool.py build <GAMEDAT 目錄> <對照表.txt> <輸出 wc1cht_spr.bin> [--palette original|full] [--preview-dir 目錄]

對照表（manifest）格式：每行「來源精靈」「圖檔」，; 或 # 開頭為註解
       TITLE.VGA:4:0    TITLE.VGA-IMAGE_001.pcx     ; 進入織女星戰役
  來源精靈 = 檔名:資源編號:精靈序號（scan 會列出）。圖檔相對於對照表所在目錄。

圖檔需求
  - 8 位元 256 色 PCX，尺寸必須跟原精靈完全相同（寬×高）。
  - 調色盤不必跟遊戲一樣：工具會把每個顏色對回遊戲調色盤（GAMEDAT\\GAME.PAL）。
      --palette original（預設）：只用「原精靈用過的那幾個調色盤索引」。顏色較少、風格跟原版一致，
                                   而且遊戲若對這些索引做調色盤動畫（反白、閃爍），中文圖也會一起動。
      --palette full           ：用整個 256 色找最接近的顏色，細節較多。
  - 透明：沿用原精靈的透明範圍（原本沒畫的像素維持沒畫）。

輸出 wc1cht_spr.bin 格式（little-endian）
  'WSP1' | u32 版本=1 | u32 筆數 | 每筆：u32 key | u32 原長 | u32 新長 | 新精靈位元組
  key = 原精靈（含標頭到結束記錄為止）的 FNV-1a 32 位元雜湊；DLL 用同一個算法比對。

精靈格式（逆向自遊戲的繪製函式，WC1 / SM1 / SM2 共用）
  標頭 8 位元組：u16 右、左、上、下（寬 = 右+左+1、高 = 上+下+1）
  之後多筆記錄 [u16 code][u16 x][u16 y][資料]，code = 0 結束。
  code >> 1 = 這段的像素數；code & 1 = 1 → 資料是 RLE 控制碼序列，0 → 直接是像素。
  RLE 控制碼 c：n = c >> 1；c & 1 → 重複下一個位元組 n 次；否則逐字複製接下來 n 個位元組。
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


# ───────────────────────── 精靈 ─────────────────────────
def _s16(v):
    return v - 65536 if v >= 32768 else v


def sprite_walk_len(s, start=0):
    """從 s[start:] 的精靈標頭開始，走完所有記錄，回傳到「結束記錄」之後的長度；格式不對回傳 0。"""
    i = start + 8
    n_s = len(s)
    while True:
        if i + 2 > n_s:
            return 0
        code = struct.unpack_from('<H', s, i)[0]
        i += 2
        if code == 0:
            return i - start
        if i + 4 > n_s:
            return 0
        i += 4
        n = code >> 1
        if code & 1:
            got = 0
            while got < n:
                if i >= n_s:
                    return 0
                c = s[i]
                i += 1
                k = c >> 1
                if k == 0:
                    return 0
                if c & 1:
                    i += 1
                else:
                    i += k
                got += k
            if got != n:
                return 0
        else:
            i += n
        if i > n_s:
            return 0


def sprite_decode(s):
    """→ (寬, 高, (右,左,上,下), img[y][x]，沒畫的像素是 None)"""
    r, l, u, d = (_s16(v) for v in struct.unpack_from('<4H', s, 0))
    w, h = r + l + 1, u + d + 1
    img = [[None] * w for _ in range(h)]
    i = 8
    while True:
        code = struct.unpack_from('<H', s, i)[0]
        i += 2
        if code == 0:
            break
        x, y = struct.unpack_from('<2H', s, i)
        i += 4
        n = code >> 1
        px = []
        if code & 1:
            while len(px) < n:
                c = s[i]
                i += 1
                k = c >> 1
                if c & 1:
                    px += [s[i]] * k
                    i += 1
                else:
                    px += list(s[i:i + k])
                    i += k
        else:
            px = list(s[i:i + n])
            i += n
        for j, v in enumerate(px[:n]):
            if 0 <= y < h and 0 <= x + j < w:
                img[y][x + j] = v
    return w, h, (r, l, u, d), img


def _encode_rle(px):
    out = bytearray()
    i, n = 0, len(px)
    while i < n:
        j = i
        while j < n and px[j] == px[i] and j - i < 127:
            j += 1
        if j - i >= 3:                                  # 連續 3 個以上相同 → 重複碼
            out += bytes([((j - i) << 1) | 1, px[i]])
            i = j
            continue
        j = i                                           # 否則收集逐字複製，遇到下一段長重複就停
        while j < n and j - i < 127:
            k = j
            while k < n and px[k] == px[j] and k - j < 3:
                k += 1
            if k - j >= 3:
                break
            j += 1
        if j == i:
            j = i + 1
        out += bytes([(j - i) << 1]) + bytes(px[i:j])
        i = j
    return bytes(out)


def sprite_encode(header8, img):
    """header8 = 原精靈前 8 位元組；img 的 None 代表不畫。每一列把連續的有畫範圍各編成一筆記錄。"""
    out = bytearray(header8)
    for y, row in enumerate(img):
        x, w = 0, len(row)
        while x < w:
            if row[x] is None:
                x += 1
                continue
            e = x
            while e < w and row[e] is not None:
                e += 1
            seg = row[x:e]
            n = len(seg)
            out += struct.pack('<HHH', (n << 1) | 1, x, y) + _encode_rle(seg)
            x = e
    out += b'\x00\x00'
    return bytes(out)


def fnv1a32(b):
    h = 0x811C9DC5
    for c in b:
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


# ───────────────────────── 資源 / 調色盤 / PCX ─────────────────────────
def find_file(folder, name):
    for f in os.listdir(folder):
        if f.lower() == name.lower():
            return os.path.join(folder, f)
    raise FileNotFoundError('%s 底下找不到 %s' % (folder, name))


def parse_set(b):
    first = struct.unpack_from('<I', b, 4)[0]
    n = (first - 4) // 4
    offs = [struct.unpack_from('<I', b, 4 + 4 * i)[0] for i in range(n)]
    offs.append(len(b))
    return offs


def load_resource(gamedat, fname, res_idx, cache={}):
    import wcres
    key = fname.lower()
    if key not in cache:
        cache[key] = open(find_file(gamedat, fname), 'rb').read()
    return wcres.extract_resource(cache[key], res_idx)


def load_game_palette(gamedat):
    d = open(find_file(gamedat, 'GAME.PAL'), 'rb').read()
    i = d.find(b'CMAP')
    if i >= 0:
        cm = d[i + 8:i + 8 + 768]
    elif len(d) == 768:
        cm = d
    else:
        raise ValueError('GAME.PAL 格式不認得')
    return [tuple(cm[j * 3:j * 3 + 3]) for j in range(256)]


def pcx_load(path):
    d = open(path, 'rb').read()
    if d[0] != 10 or d[2] != 1 or d[3] != 8 or d[65] != 1:
        raise ValueError('%s 不是 8 位元單平面 PCX' % path)
    x0, y0, x1, y1 = struct.unpack('<4H', d[4:12])
    w, h = x1 - x0 + 1, y1 - y0 + 1
    bpl = struct.unpack('<H', d[66:68])[0]
    if d[-769] != 12:
        raise ValueError('%s 沒有 256 色調色盤' % path)
    pal = [tuple(d[len(d) - 768 + i * 3:len(d) - 768 + i * 3 + 3]) for i in range(256)]
    out, i, need = bytearray(), 128, bpl * h
    while len(out) < need:
        b = d[i]
        i += 1
        if b >= 0xC0:
            out += bytes([d[i]]) * (b & 0x3F)
            i += 1
        else:
            out.append(b)
    rows = [list(out[r * bpl:r * bpl + w]) for r in range(h)]
    return w, h, rows, pal


def pcx_save(path, rows, pal):
    h, w = len(rows), len(rows[0])
    bpl = w + (w & 1)
    hdr = bytearray(128)
    hdr[0:4] = bytes([10, 5, 1, 8])
    struct.pack_into('<4H', hdr, 4, 0, 0, w - 1, h - 1)
    struct.pack_into('<2H', hdr, 12, 72, 72)
    hdr[65] = 1
    struct.pack_into('<HH', hdr, 66, bpl, 1)
    body = bytearray()
    for r in rows:
        line = list(r) + [0] * (bpl - w)
        i = 0
        while i < len(line):
            j = i
            while j < len(line) and line[j] == line[i] and j - i < 63:
                j += 1
            n = j - i
            if n > 1 or line[i] >= 0xC0:
                body += bytes([0xC0 | n, line[i]])
            else:
                body.append(line[i])
            i = j
    body.append(12)
    for c in pal:
        body += bytes(c)
    open(path, 'wb').write(bytes(hdr) + bytes(body))


def png_save(path, img, pal):
    try:
        from PIL import Image
    except ImportError:
        return False
    h, w = len(img), len(img[0])
    im = Image.new('RGB', (w, h))
    im.putdata([pal[v] if v is not None else (255, 0, 255) for r in img for v in r])
    im.resize((w * 3, h * 3), Image.NEAREST).save(path)
    return True


# ───────────────────────── 指令 ─────────────────────────
def cmd_scan(a):
    import wcres
    gd = a.gamedat
    want = None
    if a.size:
        ww, hh = a.size.lower().split('x')
        want = (int(ww), int(hh))
    files = [f for f in os.listdir(gd) if f.upper().endswith('.VGA')]
    if a.files:
        keep = {x.strip().upper() for x in a.files.split(',')}
        files = [f for f in files if f.upper() in keep]
    n_hit = 0
    for f in sorted(files, key=str.upper):
        data = open(os.path.join(gd, f), 'rb').read()
        try:
            if not wcres.is_wc_archive(data):
                continue
            nres = len(wcres.read_directory(data))
        except Exception:
            continue
        for ri in range(nres):
            try:
                b = wcres.extract_resource(data, ri)
                offs = parse_set(b)
            except Exception:
                continue
            for k in range(len(offs) - 1):
                s = b[offs[k]:offs[k + 1]]
                try:
                    ln = sprite_walk_len(s)
                    if not ln:
                        continue
                    w, h, hd, _ = sprite_decode(s)
                except Exception:
                    continue
                if want and (w, h) != want:
                    continue
                n_hit += 1
                print('%s:%d:%d\t%dx%d\t長度 %d\t雜湊 %08X' % (f.upper(), ri, k, w, h, ln, fnv1a32(s[:ln])))
    print('共 %d 個精靈' % n_hit, file=sys.stderr)


def nearest(c, pal, cands):
    best, bd = None, 1 << 30
    for g in cands:
        p = pal[g]
        d = (c[0] - p[0]) ** 2 + (c[1] - p[1]) ** 2 + (c[2] - p[2]) ** 2
        if d < bd:
            best, bd = g, d
    return best, bd ** 0.5


def cmd_build(a):
    gd = a.gamedat
    gpal = load_game_palette(gd)
    man_dir = os.path.dirname(os.path.abspath(a.manifest))
    entries = []
    for ln, line in enumerate(open(a.manifest, encoding='utf-8-sig'), 1):
        t = line.split(';')[0].split('#')[0].strip()
        if not t:
            continue
        parts = t.split()
        if len(parts) < 2:
            sys.exit('對照表第 %d 行格式不對：%s' % (ln, line.strip()))
        spec, pic = parts[0], ' '.join(parts[1:])
        f, ri, k = spec.split(':')
        entries.append((f, int(ri), int(k), pic))
    if a.preview_dir:
        os.makedirs(a.preview_dir, exist_ok=True)

    out_items, seen = [], {}
    for f, ri, k, pic in entries:
        res = load_resource(gd, f, ri)
        offs = parse_set(res)
        s0 = res[offs[k]:offs[k + 1]]
        ln = sprite_walk_len(s0)
        if not ln:
            sys.exit('%s:%d:%d 不是合法精靈' % (f, ri, k))
        s0 = s0[:ln]
        w, h, hd, img0 = sprite_decode(s0)
        key = fnv1a32(s0)
        pw, ph, rows, ppal = pcx_load(os.path.join(man_dir, pic))
        if (pw, ph) != (w, h):
            sys.exit('%s 尺寸 %dx%d，但 %s:%d:%d 的原精靈是 %dx%d（必須相同）' % (pic, pw, ph, f, ri, k, w, h))
        # 允許的調色盤索引
        if a.palette == 'full':
            cands = list(range(256))
        else:
            cands = sorted({v for r in img0 for v in r if v is not None})
        cache, tot, mx = {}, 0.0, 0.0
        newimg = []
        for y in range(h):
            row = []
            for x in range(w):
                if img0[y][x] is None:
                    row.append(None)
                    continue
                v = rows[y][x]
                if v not in cache:
                    cache[v] = nearest(ppal[v], gpal, cands)
                g, d = cache[v]
                row.append(g)
                tot += d
                mx = max(mx, d)
            newimg.append(row)
        npx = sum(1 for r in img0 for v in r if v is not None)
        s1 = sprite_encode(s0[:8], newimg)
        # 驗證：解回來要跟預期一樣、長度走得完
        if sprite_walk_len(s1) != len(s1):
            sys.exit('內部錯誤：新精靈走訪長度不符')
        _, _, _, chk = sprite_decode(s1)
        if chk != newimg:
            sys.exit('內部錯誤：新精靈解回來跟預期不一致')
        # 驗證編碼器本身：把原精靈重編再解，要跟原圖一致
        _, _, _, chk0 = sprite_decode(sprite_encode(s0[:8], img0))
        if chk0 != img0:
            sys.exit('內部錯誤：編碼器無法無損重現原精靈')
        print('%s:%d:%d  %dx%d  key=%08X  原 %d → 新 %d 位元組  %s  顏色誤差 平均 %.1f / 最大 %.1f  用到 %d 個調色盤索引'
              % (f.upper(), ri, k, w, h, key, len(s0), len(s1), pic, tot / npx, mx, len({v for r in newimg for v in r if v is not None})))
        if key in seen:
            if seen[key] != s1:
                sys.exit('兩筆對照指到同一個原精靈（key=%08X）但圖不同' % key)
            continue
        seen[key] = s1
        out_items.append((key, len(s0), s1))
        if a.preview_dir:
            base = '%s_%d_%d' % (os.path.splitext(f)[0].upper(), ri, k)
            pcx_save(os.path.join(a.preview_dir, base + '_gamepal.pcx'),
                     [[(v if v is not None else 0) for v in r] for r in newimg], gpal)
            both = [r0 + [None] + r1 for r0, r1 in zip(img0, newimg)]
            png_save(os.path.join(a.preview_dir, base + '_before_after.png'), both, gpal)

    blob = b'WSP1' + struct.pack('<II', 1, len(out_items))
    for key, ol, s1 in out_items:
        blob += struct.pack('<III', key, ol, len(s1)) + s1
    open(a.out, 'wb').write(blob)
    print('寫出 %s：%d 筆、%d 位元組' % (a.out, len(out_items), len(blob)))


def main():
    ap = argparse.ArgumentParser(description='WC1 精靈替換包工具')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('scan')
    p.add_argument('gamedat')
    p.add_argument('--size')
    p.add_argument('--files')
    p.set_defaults(fn=cmd_scan)
    p = sub.add_parser('build')
    p.add_argument('gamedat')
    p.add_argument('manifest')
    p.add_argument('out')
    p.add_argument('--palette', choices=['original', 'full'], default='original')
    p.add_argument('--preview-dir')
    p.set_defaults(fn=cmd_build)
    a = ap.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
