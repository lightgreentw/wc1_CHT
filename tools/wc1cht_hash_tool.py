#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wc1cht_hash_tool.py — 把純文字翻譯檔（英文<TAB>中文）轉成「雜湊格式」翻譯檔。

雜湊格式的檔案不含英文原文，wc1cht.dll 載入時自動辨識（檔頭 #wc1cht:hash=fnv1a64）。
轉換規則與 wc1cht_trans.inc 的載入邏輯完全一致，格式說明見該檔開頭。

用法：
    python wc1cht_hash_tool.py build  wc1zh.txt  wc1zh_hash.txt    轉出雜湊格式
    python wc1cht_hash_tool.py info   wc1zh_hash.txt               顯示雜湊格式檔的統計
    python wc1cht_hash_tool.py selftest                            檢查雜湊實作

注意：含英文的 wc1zh.txt 是翻譯用的原始檔，請自行保管，不要一起散布。
"""
import sys, re, io

MASK = (1 << 64) - 1
HEADER = "#wc1cht:hash=fnv1a64 v=1"

def fnv1a64(b: bytes) -> int:
    h = 14695981039346656037
    for x in b:
        h ^= x
        h = (h * 1099511628211) & MASK
    return h

def hx(h: int) -> str:
    return "%016x" % h

def lower_ascii(b: bytes) -> bytes:
    return bytes((c + 32) if 65 <= c <= 90 else c for c in b)

def unescape_field(s: bytes) -> bytes:
    o = bytearray(); i = 0
    while i < len(s):
        if s[i:i+1] == b'\\' and i + 1 < len(s):
            c = s[i+1:i+2]
            if c == b'n':  o += b'\n'; i += 2; continue
            if c == b't':  o += b'\t'; i += 2; continue
            if c == b'\\': o += b'\\'; i += 2; continue
        o.append(s[i]); i += 1
    return bytes(o)

def escape_field(b: bytes) -> str:
    return (b.replace(b'\\', b'\\\\').replace(b'\n', b'\\n').replace(b'\t', b'\\t')
             .replace(b'\r', b'')).decode('utf-8')

def is_pattern_var(c: int) -> bool:
    return 65 <= c <= 90 and c not in (ord('C'), ord('N'))

def build_pattern(key: bytes):
    """回傳 [('v', 變數字母) | ('l', 固定文字bytes)]，沒有變數時回傳 None（與 DLL 的 build_pattern 相同）。"""
    segs = []; lit = bytearray(); has_var = False; i = 0
    while i < len(key):
        if key[i] == ord('$') and i + 1 < len(key) and is_pattern_var(key[i+1]):
            if lit: segs.append(['l', bytes(lit)]); lit = bytearray()
            segs.append(['v', chr(key[i+1])]); has_var = True; i += 2
        else:
            lit.append(key[i]); i += 1
    if lit: segs.append(['l', bytes(lit)])
    if not has_var: return None
    for i, sg in enumerate(segs):
        if sg[0] == 'v': continue
        L = sg[1]
        if i > 0 and segs[i-1][0] == 'v':
            L = L.lstrip(b' \t')
        if i + 1 < len(segs) and segs[i+1][0] == 'v':
            L = L.rstrip(b' \t')
        sg[1] = L
    return [sg for sg in segs if sg[0] == 'v' or sg[1]]

def letter_count(b: bytes) -> int:
    return sum(1 for c in b if (65 <= c <= 90) or (97 <= c <= 122))

def term_ok(k: bytes) -> bool:
    if len(k) < 2 or len(k) > 32: return False
    if any(c in b"\n\r$,;:!?" for c in k): return False
    if k[-1:] in (b'.', b' '): return False
    if letter_count(k) < 2: return False
    return True

def parse_text(data: bytes):
    """與 wct_load_text 相同的解析與順序規則。回傳 (order, tmp, patterns, stats)。"""
    if data[:3] == b'\xef\xbb\xbf': data = data[3:]
    tmp = {}; order = []; patterns = []
    count = empty = 0
    def put(k, z):
        if k not in tmp: order.append(k)
        tmp[k] = z
    for raw in data.split(b'\n'):
        cr = raw.find(b'\r')
        if cr >= 0: raw = raw[:cr]
        if not raw or raw[:1] == b'#': continue
        tab = raw.find(b'\t')
        if tab < 0: continue
        eng = unescape_field(raw[:tab])
        zr = raw[tab+1:]
        t2 = zr.find(b'\t')
        if t2 >= 0: zr = zr[:t2]
        zh = unescape_field(zr)
        if not zh: empty += 1; continue
        segs = build_pattern(eng)
        if segs is not None:
            patterns.append((segs, zh, eng))
        else:
            put(eng, zh)
            trimmed = eng.rstrip(b' \t')
            if trimmed and trimmed != eng and trimmed not in tmp:
                put(trimmed, zh)
        count += 1
    return order, tmp, patterns, dict(count=count, empty=empty)

def build(src, dst):
    data = open(src, 'rb').read()
    order, tmp, patterns, st = parse_text(data)
    lines = []     # (排序鍵, 行文字)
    seen_ci = set(); seen_exact = {}
    n_ci = n_term = 0
    for k in order:
        zh = tmp[k]
        eh = fnv1a64(k)
        if eh in seen_exact and seen_exact[eh] != k:
            sys.exit("雜湊碰撞：兩個不同的原文有相同的雜湊，請回報（換雜湊演算法）")
        seen_exact[eh] = k
        low = lower_ascii(k); ch = fnv1a64(low)
        meta = []
        if ch not in seen_ci:
            seen_ci.add(ch); n_ci += 1
            meta.append('c' if ch == eh else 'c=' + hx(ch))
            if term_ok(k):
                meta.append('t=%d' % len(k)); n_term += 1
        zhs = escape_field(zh)
        line = hx(eh) + '\t' + zhs + (('\t' + ','.join(meta)) if meta else '')
        lines.append((0, hx(eh), line))
    for prio, (segs, zh, eng) in enumerate(patterns):
        toks = []
        for sg in segs:
            toks.append('$' + sg[1] if sg[0] == 'v' else '%d.%s' % (len(sg[1]), hx(fnv1a64(sg[1]))))
        key = '@' + '+'.join(toks)
        lines.append((1, key, key + '\t' + escape_field(zh) + '\tp=%d' % prio))
    lines.sort(key=lambda x: (x[0], x[1]))
    with open(dst, 'wb') as f:
        f.write((HEADER + '\n').encode('utf-8'))
        f.write(("# 本檔不含遊戲原文。格式說明見 wc1cht_trans.inc。由 wc1cht_hash_tool.py 產生。\n").encode('utf-8'))
        for _, _, l in lines:
            f.write((l + '\n').encode('utf-8'))
    print("輸入：%d 條有效翻譯（含 %d 條模板），略過 %d 條空翻譯" % (st['count'], len(patterns), st['empty']))
    print("輸出：%d 條一般條目 + %d 條模板；不分大小寫代表 %d 條；詞彙 %d 條" % (len(order), len(patterns), n_ci, n_term))
    print("寫入：" + dst)

def info(path):
    t = open(path, 'rb').read().decode('utf-8').split('\n')
    hdr = [l for l in t if l.startswith('#wc1cht:hash')]
    ent = [l for l in t if l and not l.startswith('#')]
    pats = [l for l in ent if l.startswith('@')]
    print("檔頭：", hdr[:1], "| 條目", len(ent), "| 模板", len(pats))
    ok_key = re.compile(r'^([0-9a-f]{16}|@(\$[A-Z]|[0-9]+\.[0-9a-f]{16})(\+(\$[A-Z]|[0-9]+\.[0-9a-f]{16}))*)$')
    bad = [i for i, l in enumerate(ent) if not ok_key.match(l.split('\t')[0])]
    print("第 1 欄不是「雜湊」或「模板鍵」的行數（應為 0）：", len(bad))

def selftest():
    vec = {b"": 0xcbf29ce484222325, b"a": 0xaf63dc4c8601ec8c, b"foobar": 0x85944171f73967e8}
    for k, v in vec.items():
        assert fnv1a64(k) == v, k
    print("FNV-1a 64 測試向量通過")

if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == 'build': build(a[1], a[2])
    elif len(a) == 2 and a[0] == 'info': info(a[1])
    elif len(a) == 1 and a[0] == 'selftest': selftest()
    else: print(__doc__); sys.exit(1)
