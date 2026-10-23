#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
wc1_extract_text.py  --  Wing Commander 1 中文化文字工具（單一程式）
適用 WC1 / Secret Missions 1 / SM2 / SM1.5 / Transfer。只用 Python 3.8+ 標準函式庫。

四個指令（完整說明見「操作說明.md」）：
    export   匯出遊戲用到的全部英文文字（劇情對話、UI 標籤、空戰對話、空戰電腦 UI、字幕、系統訊息），
             並產生翻譯表樣板 wc1zh_template.txt
    check    檢核既有翻譯表與遊戲原文的差異（key 對不上、被換行切斷、版本變體、尚未翻譯）
    missed   查 DLL 未命中字串（wc1cht_missed.tsv）在遊戲資料裡的位置，並產出可直接翻譯的條目
    sync     把已翻譯的譯文自動帶入「同一句、只差開頭 . 空白 大小寫」的空白條目

範例：
    python wc1_extract_text.py export  D:\WC1  -o out --table wc1zh.txt
    python wc1_extract_text.py check   D:\WC1  --table wc1zh.txt -o chk
    python wc1_extract_text.py missed  D:\WC1  --missed wc1cht_missed.tsv --table wc1zh.txt -o ms
    python wc1_extract_text.py sync    wc1zh.txt
    （省略指令名稱＝export，相容舊用法）
"""
import argparse, collections, difflib, os, re, struct, sys

# ───────────────────────────── 基本工具 ─────────────────────────────
def esc(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t').replace('\r', '\\r')

def unesc(s):
    m = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\'}
    return re.sub(r'\\(.)', lambda x: m.get(x.group(1), x.group(0)), s)

def wsflags(s):
    f = []
    if '  ' in s: f.append('dbl_space')
    if s != s.lstrip(' '): f.append('lead_space')
    if s != s.rstrip(' '): f.append('trail_space')
    if re.search(r' \n|\n ', s): f.append('space_at_LF')
    return ','.join(f)

def norm(s):
    """同一句不同寫法的正規化：去開頭 .、小寫、只留英數與 $ %、空白收成一格。"""
    s = s.lstrip('.').lower()
    s = re.sub(r"[^a-z0-9$%\s]", '', s)
    return re.sub(r'\s+', ' ', s).strip()

def ci_find(folder, name):
    """大小寫不分找檔。"""
    low = name.lower()
    try:
        for f in os.listdir(folder):
            if f.lower() == low: return os.path.join(folder, f)
    except OSError:
        pass
    return None

# ───────────────────────────── 資源包格式 ─────────────────────────────
def parse_pack(path):
    """WC1 資源包：u32 大小，其後每項 24-bit 偏移 + 1 byte 旗標。回傳 [(idx, off, flag, bytes)]。"""
    d = open(path, 'rb').read()
    size = struct.unpack_from('<I', d, 0)[0] & 0xFFFFFF
    first = struct.unpack_from('<I', d, 4)[0] & 0xFFFFFF
    n = first // 4 - 1
    if n <= 0 or first > len(d): raise ValueError('不是 WC1 資源包')
    offs, flags = [], []
    for i in range(n):
        v = struct.unpack_from('<I', d, 4 + 4 * i)[0]
        offs.append(v & 0xFFFFFF); flags.append(v >> 24)
    out = []
    for i in range(n):
        end = offs[i + 1] if i + 1 < n else size
        out.append((i, offs[i], flags[i], d[offs[i]:end] if end >= offs[i] else b''))
    return out

# ───────────────────────────── 劇情腳本解析 ─────────────────────────────
# 腳本以 NUL 分隔：  文字 \0 口型(lipsync) \0 動畫指令 \0    文字內可含 LF（折行）與少數 TAB。
PRINT = set(range(0x20, 0x7f)) | {0x0a, 0x09}
ANIM = re.compile(r'^[A-Z]{0,2}\d[\dA-Z,]*\Z')
CTL = re.compile(r'^(\d{1,3},)+\d{0,3}\Z')
PHON_ANY = re.compile(r'^(?P<ph>[A-Za-z0-9$]{3,})(?P<an>R[\dA-Z,]+)?\Z')
PHON_SP = re.compile(r'^[a-z0-9$ ]+\Z')

def segments(b):
    out, pos = [], 0
    for part in b.split(b'\x00'):
        if len(part) >= 2 and all(c in PRINT for c in part):
            out.append((pos, part))
        pos += len(part) + 1
    return out

def texty(t):
    if len(re.findall(r'[A-Za-z]', t)) < 2: return False
    if len(t) >= 4 and re.match(r"^[A-Z0-9$.\"'(*<\[\-\u2026 \n]", t): return True
    if len(t) >= 3 and re.match(r'^[A-Z].*[.!?]$', t): return True
    if ' ' in t and len(re.findall(r'[A-Za-z]{2,}', t)) >= 2: return True
    return False

def script_rows(b):
    segs = [(off, part.decode('latin1')) for off, part in segments(b)]
    rows, n, k = [], len(segs), 0
    while k < n:
        off, t = segs[k]
        if CTL.match(t): k += 1; continue
        if ANIM.match(t): k += 1; continue
        if ' ' not in t and not re.search(r"[.,!?'\"\-:;\u2026]", t) and \
           (len(t) >= 12 or re.search(r'\d', t) or t.islower()) and PHON_ANY.match(t):
            k += 1; continue                                  # 只有口型字串
        if texty(t) or (len(t) >= 4 and len(re.findall(r'[A-Za-z]', t)) >= 2):
            kind = 'text' if texty(t) else 'unclassified'
            r = dict(off=off, text=t, phoneme='', anim='', kind=kind)
            if k + 1 < n:                                      # 向後看口型與動畫
                o2, t2 = segs[k + 1]
                m = PHON_ANY.match(t2)
                nxt_anim = (k + 2 < n and ANIM.match(segs[k + 2][1]) is not None)
                lower_ph = (PHON_SP.match(t2) is not None and (' ' not in t2 or re.search(r'\d', t2)) and len(t2) >= 6)
                if m and (nxt_anim or m.group('an')):
                    r['phoneme'] = m.group('ph'); k += 1
                    if m.group('an'): r['anim'] = m.group('an')
                    elif nxt_anim: r['anim'] = segs[k + 1][1]; k += 1
                elif lower_ph and kind == 'text':
                    r['phoneme'] = t2; k += 1
                    if k + 1 < n and ANIM.match(segs[k + 1][1]): r['anim'] = segs[k + 1][1]; k += 1
                elif not nxt_anim and k + 1 < n and ANIM.match(t2):
                    r['anim'] = t2; k += 1
            if kind == 'unclassified' and not r['phoneme'] and len(t) < 4:
                k += 1; continue
            rows.append(r)
        k += 1
    return rows

# ───────────────────────────── PE / exe ─────────────────────────────
class PE:
    def __init__(self, path):
        d = open(path, 'rb').read(); self.d = d
        if d[:2] != b'MZ' or len(d) < 0x40: raise ValueError('非 PE')
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\0\0': raise ValueError('非 PE')
        ns = struct.unpack_from('<H', d, pe + 6)[0]; osz = struct.unpack_from('<H', d, pe + 20)[0]
        self.base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
        self.ts = struct.unpack_from('<I', d, pe + 8)[0]
        self.secs = []
        for i in range(ns):
            o = pe + 24 + osz + 40 * i
            nm = d[o:o + 8].rstrip(b'\0').decode('latin1')
            vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
            self.secs.append((nm, va, vs, ro, rs))
    def strings(self, minlen=3, secs=('.rdata', '.data')):
        out = []
        for nm, va, vs, ro, rs in self.secs:
            if nm not in secs: continue
            blob = self.d[ro:ro + rs]
            for m in re.finditer(rb'[\x20-\x7e\t\r\n]{%d,}' % minlen, blob):
                out.append((self.base + va + m.start(), nm, m.group()))
        return out

EXE_VERSIONS = {0x32821571: 'WC1', 0x3282101C: 'SM1', 0x3294AA25: 'SM2', 0x325D4FED: 'TRF'}

# 排除：系統／CRT／除錯／檔名等非遊戲文字
_BL = re.compile(r"((?i:\.drv)|SetMus|skipping for QA|^%d %d|^F\(-|^error$|\.c$|\.cpp|\.cfg|\.dat$|\.wav|\.pal|\.drv|\.str$|\.dll|\.exe|\.cal|DDERR_|^DIB|Streamer|stream|\[SYSTEM\]|^qq |^!|\bFMem\b|\bNMem\b|%p|Warning \[|Fatal \[|D:\\\\|\\\\wc|gamedat|vga$|ega$|cga$|tga$|alloc|MONODEBG|MonoDebug|direct\.err|^LP|^sin$|^cos$|^tan$|^log|^exp$|^pow$|^sqrt|^a(sin|cos|tan)|SAVEGAME|WINGCMDR|ctrl\.del|^PST$|^PDT$|FORM|AIFF|RIFF|WAVE|STRM|^j\.cal|^\$#|not MCGA|display_mode|^1\.0|^Software\\|^jeff$|bad |not jeff|__|triangle|shadow_draw|fizzle|snow_viewport|^SNOW|being tailed|ofx music|queue_|track_|flight_|SetMusic|StopMusic|FadeMusic|FlushSound|soundFX|sound_effect|sfx%|MusicVolume|SFXVolume|playWAVE|^Notice$|sleep_for|reg (not )?cheating|^Client|^Ignore|^Normal$|^Free$|^MCGA|^VSCREEN|free_viewport|reading from disk|allocating memory|\(OVR\)|^%X%Y%F%s%P$|^%X%Y%s%P$|^FORCE$|^\(null\)|^\(\w+\)$|^push |Sizeof|^%0ld$|^%ld$|^%02d$)")
_SHORT_OK = ('None', 'Flee', 'Mines', 'Full', 'KIA', 'Ace', 'Hit', 'CHEN', 'Easy', 'Hard', 'Free')

def is_game_string(s):
    t = s.decode('latin1')
    if _BL.search(t): return False
    if len(re.findall(r'[A-Za-z]', t)) < 3 and not re.search(r'%[A-Za-z]', t): return False
    if len(t) < 6 and not re.fullmatch(r"\s?[A-Za-z][A-Za-z .'\-]{2,}", t): return False
    if len(t) <= 4 and not (re.fullmatch(r'[A-Z]{3,4}', t) and re.search(r'[AEIOU]', t) and t[-1] not in 'JK') and t not in _SHORT_OK: return False
    if len(t) <= 5 and (t[-1] in 'JK' or not re.search(r'[AEIOUaeiou]', t)): return False
    return True

# exe 字串分類 → (category, group)
_CATS = [
 ('comp_ui', 'combat_ui', r"^(MISSILE LOCKED|Already Near|Enemy Near|Hazard Near|%s HIT|WEAPON DISPLAY|Full Guns|\nWeapon|\nGun|CALCULATING|\s?NONE$|UNKNOWN|COMP NAVIGATION|\n\nDESTINATION|\n\nRANGE|\n\n\(N\)ew|Already Visited|Objective Reached|Wait for|\s+LOCKED TARGET|\s+AUTO TARGETTING|\nTarget:|\nRange|TOO FAR|Weapon destroyed|Fuel tanks hit|%s FIXD|Need Lock|DAMAGE REPORT|NO INTERNAL|%d Unit|%s\nDamage|Light$|Moderate$|Heavy$|Destroyed$|Ion drive|Power plant|Shield gen|Computer sys|InterCom|Target track|Accel abs|Ejector sys|Repair sys|GAME PAUSED|SPACE BUFFER|MISSILE CAMERA|VIDEO IMAGES|MESSAGES SPEED|%d FRAMES|SFX VOLUME|MUSIC VOLUME|Setting SFX|\s?None$|%F%s)"),
 ('comm_menu', 'combat_ui', r"^(Never mind|Attack my target|Help me out here|Return to base|Die furball|Slag off|Bite it cat face|Break and attack|Keep formation|Form on my wing|Keep radio silence|Broadcast freely|Request Landing|Here, kitty|Get fixed|Eat this|SELECT$|VID-COM|ENEMY TARGET|CHOOSE|\n%d %s|%s: %Fs|SELECT\nENEMY)"),
 ('ship_weapon_names', 'ui_label', r"^(Hornet|Rapier|Scimitar|Raptor|Venture|Dilligent|Drayman|Exeter|Tiger's Claw$|Salthi|Dralthi|Krant|Gratha|Jalthi|Spikeri|Hriss|Dorkir|Lumbari|Ralari|Fralthi|Snakeir|Sivar|Star post|Laser cannon|Neutron gun|Mass driver|Dart DF|Javelin HS|Pilum FF|Spiculum IR|Porcupine|Drakhai|Vega XR|Courier)"),
 ('nav_map_briefing', 'ui_label', r"^(Patrol|Escort$|Strike$|Defend$|Wingman$|Flee$|Goto Warp|Rendezvous|err$|Asteroids|Mines|%X%YStandard time|MISSION FLIGHT PATH|HOME BASE|Sector:|System:|\* %s \*|\nNotes|%X%Y +Location|Briefing Nav Map|ConFed Nav Scan|OBJECTIVES|%s\n\n$)"),
 ('intro_ending_credits', 'story_text', r"^(\nWith your carrier|THE END|The End|For Now|Meanwhile|Houston, we have|In the distant future|Design\nby|Software Engineers|Dogfight|3Space|OriginFX|Artwork|Screenplay|Soundtrack|Sound Effects|Produced by|Directed by|Windows 95 Team|Combat Programmers|Sound System|Head Whiner|Whiners|Richard Zinser|Special Thanks|Destroying the remains|The Tiger's Claw (closes|manages)|And the last Kilrathi|Fleeing from the overwelming|There'll be other|You step out of the airlock|%X%Y(%F)?Company|%X%YAtten|%X%YPrepare arms|%X%Y%FFire|Animation demo|View Templates|The SECRET MISSIONS)"),
 ('landing_dialogue', 'dialogue', r"^(You got away pretty clean|Looks like it got a little hot|You sure got yourself shot up|Glad to see you made it back)"),
 ('medal_award', 'ui_label', r"^(Bronze Star|Silver Star|Gold Star|Golden Sun|Terran Medal|\$R \$N, aka)"),
 ('pilot_rank_names', 'ui_label', r"^(TANAKA|SPIRIT|ST\.JOHN|HUNTER|CHEN|BOSSMAN|CASEY|ICEMAN|DEVEREAUX|ANGEL|TAGGART|PALADIN|MARSHALL|MANIAC|KHUMALO|KNIGHT|PELLEY|GOBLIN|MONTCLAIR|DOOMSDAY|COLSON|JAZZ|2ND LT|1ST LT|CAPTAIN|MAJOR|LT\. COL|Bhurak|Dakhath|Khajja|Bakhtosh)"),
 ('bar_ui', 'ui_label', r"^(Talk to|Check pilot scores|Enter barracks|Fly training mission|%X%YCARRIER|%X%YPILOT|%X%YSORTIES|%X%Y%s %s|%X%Y%d%X%d|KIA|%X%Y     %s|Check poster|Save this campaign|Mission Hangar|Return to the Bar|Quit Wing Commander|View your medals|game %d|Awaken|Load a game|Game Name|Error: Game|This saved game|MONTCLAIR|Loading Game|Error: data|FAULTY DATA|Replace %s|Transfer (to|%)|SQUADRON: TRAINSIM|CHEATER)"),
 ('arcade_trainsim', 'ui_label', r"^(BISHOP|GOBLIN|JEFFTEP|MANGLER|THE MAN|MONGO|CONGRATULATIONS|Blair|LAST NAME|Maverick|CALLSIGN|\*{7}|> SORRY|%JHIGH|%X%Y%d\. |SELECT\nENEMY|Get Ready|Victory|Game Over|Wave %d|Mission %d complete|%X%YScore)"),
 ('system_dialog', 'system', r"^(Turn AUTO FIRE|Move stick|Center Joystick|FAILED! Center|Answer:|Press any key|Loading INSTALL|Unable to|ATTENTION|NOTICE|Only one instance|You must|Insert Correct CD|Please place|Please [Ii]nsert|Sorry, an error|Error loading|ERROR:|Requested graphics|Expanded Memory|Failed to load|EMStartUp|You do not have enough|Loading WING|Version|Pre animation|Post animation|WING COMMANDER VER|Origin$|Current|Memory Info|EXTREME|SEVERE|BAD$|NAUGHTY|over$|under$|Wing Commander|Space Flight|DirectDraw|\s+Unable|\d+x\d+ achieved|DISK\.000|Full$|Limited$|No $| music will play|Joystick:|keyboard almost|FATAL|\[SYSTEM\]|Quit Wing Commander\?|\s*INFO DISPLAY|Series %d|PC Kills|Series Score|Miss Score|Promotion Score|Wing Commander \d|<anydisc>|%s\\|\n\n|\n)"),
]
_CATS = [(c, g, re.compile(p)) for c, g, p in _CATS]
def categorize_exe(t):
    for c, g, rx in _CATS:
        if rx.search(t): return c, g
    return 'other_ui', 'ui_label'

_START = re.compile(rb'(\nWith your carrier|Hornet$|Transfer to SM1|View Templates|Bronze Star|THE END$|Never mind)')
_END = re.compile(rb'(Space Flight Max FPS|DirectDraw Error|ERROR: %s - \(%s\))')

# ───────────────────────────── 蒐集 ─────────────────────────────
OCC = []   # 每次出現
CUR_SET = ['']
def add(group, cat, ver, src, loc, text, kind='text', lip='', anim='', order=0):
    OCC.append(dict(group=group, category=cat, version=ver, source=src, loc=loc, text=text, set=CUR_SET[0],
                    kind=kind, lipsync=lip, anim=anim, order=len(OCC)))

PACK_VER = {'000': 'WC1', '001': 'SM1', '002': 'SM2', '015': 'SM15'}
def ver_of_ext(ext): return PACK_VER.get(ext, 'V' + ext)

def collect(gd, root, log, done_exes):
    names = {f.lower(): f for f in os.listdir(gd)}
    # --- BRIEFING.0xx / MIDGAME.Vxx 劇情對話
    for low in sorted(names):
        m = re.fullmatch(r'briefing\.(\d{3})', low)
        mg = re.fullmatch(r'midgame\.v(\d{2})', low)
        if not (m or mg): continue
        fn = names[low]
        try: res = parse_pack(os.path.join(gd, fn))
        except Exception as e: log.append('略過 %s：%s' % (fn, e)); continue
        ver = ver_of_ext(m.group(1)) if m else 'MIDGAME'
        cat = 'briefing' if m else 'midgame_cutscene'
        for i, o, fl, b in res:
            if not b: continue
            if mg and len(b) > 4096: continue           # 過場的大型資源是圖像／動畫，不是腳本
            for r in script_rows(b):
                add('dialogue', cat, ver, fn, 'res%d @0x%X' % (i, o + r['off']), r['text'], r['kind'], r['phoneme'], r['anim'])
    # --- 空戰對話
    for low, ver in (('communic.dat', 'WC1/SM1'), ('commsm2.dat', 'SM2')):
        if low not in names: continue
        d = open(os.path.join(gd, names[low]), 'rb').read()
        for i in range(len(d) // 80):
            t = d[i * 80:(i + 1) * 80].split(b'\0')[0].decode('latin1')
            if t.strip(): add('combat_dialogue', 'combat_comms', ver, names[low], '#%d @0x%X' % (i, i * 80), t)
    # --- MODULE.0xx：任務／目標／導航標籤（res1 任務、res2 目標、res4/5 標籤與星系）
    CATN = {1: 'module_res1_mission', 2: 'module_res2_objective', 4: 'module_res4_label', 5: 'module_res5_system'}
    for low in sorted(names):
        m = re.fullmatch(r'module\.(\d{3})', low)
        if not m: continue
        fn = names[low]
        try: res = parse_pack(os.path.join(gd, fn))
        except Exception as e: log.append('略過 %s：%s' % (fn, e)); continue
        for i, o, fl, b in res:
            if i not in CATN: continue
            for mm in re.finditer(rb'[\x20-\x7e\n]{2,}', b):
                t = mm.group().decode('latin1')
                if not re.search(r'[A-Za-z]{2,}', t): continue
                add('ui_label', CATN[i], ver_of_ext(m.group(1)), fn, 'res%d @0x%X' % (i, o + mm.start()), t,
                    kind='text' if (len(t.lstrip('.')) >= 4 and re.search(r'[AEIOUaeiou]', t)) else 'unclassified')
    # --- exe
    exes = [f for f in sorted(os.listdir(root)) if f.lower().endswith('.exe')]
    seen_sig = set()
    for fn in exes:
        p = os.path.join(root, fn)
        if os.path.abspath(p) in done_exes: continue
        done_exes.add(os.path.abspath(p))
        try: pe = PE(p)
        except Exception: log.append('略過 %s（不是 32 位元 PE，例如 DOS 版）' % fn); continue
        ver = EXE_VERSIONS.get(pe.ts)
        if not ver:
            low = fn.lower()
            ver = 'SM1' if 'sm1' in low else 'SM2' if 'sm2' in low else 'TRF' if 'transfer' in low else 'WC1' if ('wing1' in low or 'wc1' in low or 'wc_' in low) else os.path.splitext(fn)[0]
            log.append('%s：時間戳 0x%08X 不在已知清單，依檔名視為 %s' % (fn, pe.ts, ver))
        allstr = pe.strings(3, ('.data', '.rdata'))
        starts = [va for va, sec, s in allstr if sec == '.data' and _START.match(s)]
        ends = [va for va, sec, s in allstr if sec == '.data' and _END.match(s)]
        if starts: lo, hi = min(starts), (min(ends) if ends else 1 << 31)
        else: log.append('略過 %s：找不到遊戲字串區段（不是 WC1 系列主程式，或版本太特殊）' % fn); continue
        sig = (ver, tuple(s for va, sec, s in allstr if sec == '.data' and lo <= va < hi))
        if sig in seen_sig: log.append('%s：與同版本的另一個 exe 字串完全相同，位置合併列出' % fn)
        seen_sig.add(sig)
        for va, sec, s in allstr:
            if sec != '.data' or not (lo <= va < hi) or not is_game_string(s): continue
            t = s.decode('latin1'); cat, grp = categorize_exe(t)
            add(grp, 'exe:' + cat, ver, fn, '0x%X' % va, t)

# ───────────────────────────── 整理 ─────────────────────────────
VORDER = ['WC1', 'WC1/SM1', 'SM1', 'SM2', 'SM15', 'TRF', 'MIDGAME']
def vsort(vs):
    flat = set()
    for v in vs: flat.update(v.split('/'))
    return sorted(flat, key=lambda v: (VORDER.index(v) if v in VORDER else 99, v))

GROUPS = ['dialogue', 'combat_dialogue', 'combat_ui', 'ui_label', 'story_text', 'system']

def relevance(text, group):
    if not re.search(r'[A-Za-z]{2,}', text): return 'nontext'
    return 'ok'

def unique_rows():
    U = collections.OrderedDict()
    for o in OCC:
        k = (o['group'], o['text'])
        u = U.setdefault(k, dict(group=o['group'], text=o['text'], cats=collections.OrderedDict(), vers=set(), sets=[], locs=[],
                                  kinds=set(), n=0, order=o['order']))
        u['cats'][o['category']] = 1; u['vers'].add(o['version'])
        if o['set'] not in u['sets']: u['sets'].append(o['set'])
        u['n'] += 1; u['kinds'].add(o['kind'])
        u['locs'].append('%s %s' % (o['source'], o['loc']))
    return list(U.values())

def tsv_write(path, header, rows):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write('\t'.join(header) + '\n')
        for r in rows: f.write('\t'.join(str(x) for x in r) + '\n')

def load_table(path):
    T = {}
    for l in open(path, encoding='utf-8-sig').read().split('\n'):
        l = l.rstrip('\r')
        if not l.strip() or l.lstrip().startswith('#') or '\t' not in l: continue
        k, t = l.split('\t', 1)
        if t.strip(): T[unesc(k).rstrip()] = t
    return T

# ───────────────────────────── 掃描資料夾 ─────────────────────────────
def find_game_roots(path):
    """回傳 [(標籤, GAMEDAT 路徑, exe 所在資料夾)]。path 本身是遊戲資料夾或 GAMEDAT 就直接用；否則往下找兩層。"""
    path = os.path.abspath(path)
    def check(d):
        gd = ci_find(d, 'GAMEDAT')
        if gd and os.path.isdir(gd): return gd, d
        if ci_find(d, 'BRIEFING.000') or ci_find(d, 'COMMUNIC.DAT'): return d, os.path.dirname(d)
        return None
    found = []
    def walk(d, depth):
        c = check(d)
        if c: found.append((d, c[0], c[1])); return
        if depth >= 2: return
        try: subs = sorted(x for x in os.listdir(d) if os.path.isdir(os.path.join(d, x)))
        except OSError: return
        for x in subs: walk(os.path.join(d, x), depth + 1)
    walk(path, 0)
    return [(os.path.basename(d.rstrip(os.sep)) or d, gd, er) for d, gd, er in found]

def text_files(gd, exe_root):
    """會影響文字的檔案：名稱（小寫）→ 路徑。"""
    out = {}
    pat = re.compile(r'(briefing\.\d{3}|midgame\.v\d{2}|communic\.dat|commsm2\.dat|module\.\d{3})$')
    for f in os.listdir(gd):
        if pat.match(f.lower()): out[f.lower()] = os.path.join(gd, f)
    for f in os.listdir(exe_root):
        if f.lower().endswith('.exe'): out[f.lower()] = os.path.join(exe_root, f)
    return out

def md5(path):
    import hashlib
    return hashlib.md5(open(path, 'rb').read()).hexdigest()

def scan(roots, log):
    """掃描所有資料夾，結果累積在 OCC。回傳 (sets, multi, hashes)。"""
    sets = []
    for r in roots:
        for lab, gd, er in find_game_roots(r):
            n = 2; base = lab
            while any(x[0] == lab for x in sets): lab = '%s#%d' % (base, n); n += 1
            sets.append((lab, gd, er))
    if not sets: sys.exit('找不到 GAMEDAT 資料夾（或含 BRIEFING.000 的資料夾）：' + ' '.join(roots))
    done_exes = set(); hashes = {}
    for lab, gd, er in sets:
        CUR_SET[0] = lab
        collect(gd, er, log, done_exes)
        for fn, pth in text_files(gd, er).items(): hashes.setdefault(fn, {})[lab] = md5(pth)
    if not OCC: sys.exit('沒有抓到任何文字，請確認資料夾內容。')
    return sets, len(sets) > 1, hashes

# ───────────────────────────── 比對工具（check / missed 共用）─────────────────────────────
def vis(s): return s.replace(' ', '·')
def collapse(s): return re.sub(r'\s+', ' ', s).strip()
def _fmt(s): return re.sub(r'%[0-9]*l?[A-Za-z]+0?|\$[A-Z]', '§', s)
def _vsp(s): return re.sub(r'(\$[A-Z])\s+(?=\$[A-Z])', r'\1', s)

def inline(a, b):
    """逐字差異：[-只在A-]{+只在B+}，空白以 · 顯示。"""
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False); out = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal': out.append(a[i1:i2])
        else:
            if i2 > i1: out.append('[-' + vis(a[i1:i2]) + '-]')
            if j2 > j1: out.append('{+' + vis(b[j1:j2]) + '+}')
    return esc(''.join(out))

def classify(a, b):
    """兩句差異類型：whitespace / case / punctuation / var_spacing / placeholder_notation / spelling/minor_edit / reworded / different。"""
    if a == b: return 'same'
    if _fmt(a) == _fmt(b) and _vsp(collapse(a)) != _vsp(collapse(b)): return 'placeholder_notation($X vs %fmt)'
    if _vsp(a) == _vsp(b): return 'var_spacing($R$N vs $R $N)'
    ca, cb = collapse(a), collapse(b)
    if ca == cb:
        subs = []
        if ('  ' in a) != ('  ' in b) or a.count('  ') != b.count('  '): subs.append('space_count')
        if (a != a.rstrip(' ')) != (b != b.rstrip(' ')): subs.append('trailing_space')
        if (a != a.lstrip(' ')) != (b != b.lstrip(' ')): subs.append('leading_space')
        if a.count('\n') != b.count('\n'): subs.append('linebreak')
        return 'whitespace' + ('(' + '+'.join(subs) + ')' if subs else '')
    if a.replace(' ', '').replace('\n', '') == b.replace(' ', '').replace('\n', ''): return 'whitespace(space_added_or_removed)'
    if ca.lower() == cb.lower(): return 'case'
    pa = re.sub(r"[^\w\s]", '', ca.lower()); pb = re.sub(r"[^\w\s]", '', cb.lower())
    if pa == pb or pa.replace(' ', '') == pb.replace(' ', ''): return 'punctuation'
    r = difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()
    return 'spelling/minor_edit' if r >= 0.85 else 'reworded' if r >= 0.6 else 'different'

def n_rstrip(s): return s.rstrip(' \n\t')
def n_ws(s): return collapse(s)
def n_case(s): return collapse(s).lower()
def n_var(s): return _vsp(collapse(s)).lower()
def n_punct(s): return re.sub(r'[^\w\s$%]', '', n_var(s)).replace(' ', '')
def n_fmt(s): return re.sub(r'%[0-9]*l?[A-Za-z]+0?|\$[A-Z]', '§', n_var(s))
NORMS = [('exact_after_trailing_ws', n_rstrip), ('whitespace', n_ws), ('case', n_case), ('var_spacing', n_var),
         ('placeholder', n_fmt), ('punctuation', n_punct)]

class Matcher:
    """由一批字串建索引：精確 → 結尾空白 → 空白 → 大小寫 → 變數間距 → 佔位符 → 標點，逐級放寬找同一句。"""
    def __init__(self, texts):
        self.texts = list(texts); self.set = set(self.texts)
        self.idx = [{} for _ in NORMS]
        for t in self.texts:
            for j, (nm, f) in enumerate(NORMS): self.idx[j].setdefault(f(t), []).append(t)
        self.bylen = collections.defaultdict(list)
        for t in self.texts: self.bylen[len(t)].append(t)
    def find(self, k):
        if k in self.set: return 'EXACT', [k]
        for j, (nm, f) in enumerate(NORMS):
            c = self.idx[j].get(f(k))
            if c: return nm, c
        return None, []
    def nearest(self, k, cutoff=0.8, slack=5, frac=5):
        best = (0, None); L = len(k)
        for l in range(max(1, L - max(slack, L // frac)), L + max(slack, L // frac) + 1):
            for t in self.bylen.get(l, ()):
                sm = difflib.SequenceMatcher(None, k, t, autojunk=False)
                if sm.real_quick_ratio() < max(cutoff, best[0]) or sm.quick_ratio() < max(cutoff, best[0]): continue
                r = sm.ratio()
                if r > best[0]: best = (r, t)
        return best if best[0] >= cutoff else (0, None)

def build_src(multi):
    """OCC → {文字: {vers, cats, sets, locs}}"""
    src = {}
    for o in OCC:
        t = o['text']
        if not t.strip(): continue
        d = src.setdefault(t, {'vers': set(), 'cats': set(), 'locs': [], 'sets': []})
        d['vers'].update(o['version'].split('/')); d['cats'].add(o['category'])
        if o['set'] not in d['sets']: d['sets'].append(o['set'])
        if len(d['locs']) < 4: d['locs'].append(('{%s} ' % o['set'] if multi else '') + '%s %s' % (o['source'], o['loc']))
    return src

def vers_of(src, t): return '+'.join(vsort(src[t]['vers']))

def load_entries(path):
    """翻譯表每一行 → [(行號, key, 譯文, 有無TAB)]；空行與 # 開頭略過。"""
    keys = []
    for i, l in enumerate(open(path, encoding='utf-8-sig').read().split('\n'), 1):
        l = l.rstrip('\r')
        if not l.strip() or l.lstrip().startswith('#'): continue
        if '\t' in l: k, t = l.split('\t', 1); keys.append((i, unesc(k), t, True))
        else: keys.append((i, unesc(l), '', False))
    return keys

# ───────────────────────────── 子指令：export ─────────────────────────────
def cmd_export(a):
    log = []
    sets, multi, hashes = scan(a.roots, log)
    root = '；'.join('%s ＝ %s' % (lab, gd) for lab, gd, er in sets)
    os.makedirs(a.out, exist_ok=True)

    # 1) 每次出現
    tsv_write(os.path.join(a.out, 'wc1_all_text_occurrences.tsv'),
              ['group', 'category', 'version', 'set', 'source', 'location', 'kind', 'text', 'lipsync', 'anim', 'ws_flags'],
              [(o['group'], o['category'], o['version'], o['set'], o['source'], o['loc'], o['kind'], esc(o['text']), esc(o['lipsync']), esc(o['anim']), wsflags(o['text'])) for o in OCC])

    # 2) 不同字串
    U = unique_rows()
    table = load_table(a.table) if a.table else None
    skipped, keep = [], []
    for u in U:
        (skipped if relevance(u['text'], u['group']) == 'nontext' else keep).append(u)
    def in_table(u):
        if table is None: return ''
        return 'exact' if u['text'].rstrip() in table else ''
    hdr = ['group', 'category', 'text', 'versions', 'n_occurrences', 'confidence', 'ws_flags', 'first_locations']
    if multi: hdr.insert(4, 'sets')
    if table is not None: hdr.insert(3, 'in_table')
    def row(u):
        conf = 'low' if u['kinds'] == {'unclassified'} else 'high'
        r = [u['group'], ';'.join(u['cats']), esc(u['text']), '/'.join(vsort(u['vers'])), u['n'], conf, wsflags(u['text']), ' | '.join(u['locs'][:3])]
        if multi: r.insert(4, '/'.join(u['sets']))
        if table is not None: r.insert(3, in_table(u) or 'MISSING')
        return r
    keep_sorted = sorted(keep, key=lambda u: (GROUPS.index(u['group']), u['order'] if a.order == 'source' else 0, norm(u['text']), u['text']))
    tsv_write(os.path.join(a.out, 'wc1_all_text.tsv'), hdr, [row(u) for u in keep_sorted])
    for g in GROUPS:
        rs = [row(u) for u in keep_sorted if u['group'] == g]
        if rs: tsv_write(os.path.join(a.out, 'by_group', g + '.tsv'), hdr, rs)
    tsv_write(os.path.join(a.out, 'wc1_skipped_nontext.tsv'), ['group', 'text', 'versions', 'first_locations'],
              [(u['group'], esc(u['text']), '/'.join(vsort(u['vers'])), ' | '.join(u['locs'][:2])) for u in skipped])

    # 3) 翻譯表樣板：每個「去掉結尾空白後不同」的句子一行；開頭 . 的任務標籤同時列有點與無點兩種
    ent = collections.OrderedDict()
    for u in keep_sorted:
        t = u['text']; forms = [t.rstrip()]
        if t.startswith('.') and u['group'] == 'ui_label': forms = [t[1:].rstrip(), t.rstrip()]
        for f in forms:
            if not f.strip(): continue
            e = ent.setdefault((u['group'], f), dict(vers=set(), sets=[], loc=u['locs'][0], n=0, order=u['order']))
            e['vers'].update(u['vers']); e['n'] += u['n']
            for x in u['sets']:
                if x not in e['sets']: e['sets'].append(x)
    out = ['# wc1zh.txt 樣板 — 由 wc1_extract_text.py 產生', '# 格式：英文<TAB>中文；\\n \\t 為跳脫；空譯文載入時略過；行首 # 為註解',
           '# 同一句的不同寫法（空白／標點／大小寫）以「# ▼」標記並緊接在上方原句之後；版本不同＝不同句，各自一行。']
    filled = 0; empty = 0; untr = []
    if table is not None:
        tn = collections.defaultdict(set)
        for k, t in table.items(): tn[norm(k)].add(t)
    for g in GROUPS:
        es = [(k, e) for (gg, k), e in ent.items() if gg == g]
        if not es: continue
        if a.order == 'normalized': es.sort(key=lambda x: (norm(x[0]), x[0]))
        else: es.sort(key=lambda x: x[1]['order'])
        out += ['', '# ══════════ %s（%d 句）══════════' % (g, len(es))]
        prev = None
        for k, e in es:
            nk = norm(k); mark = '▼' if (a.order == 'normalized' and nk == prev and nk) else '●'
            prev = nk
            out.append('# %s [%s]%s %s' % (mark, '/'.join(vsort(e['vers'])), (' {%s}' % ','.join(e['sets'])) if multi and len(e['sets']) < len(sets) else '', e['loc']))
            tr = ''
            if table is not None:
                if k in table: tr = table[k]
                elif len(tn.get(nk, ())) == 1 and nk: tr = next(iter(tn[nk])); out[-1] += '  〔譯文由近似寫法帶入，請確認〕'
            if tr: filled += 1
            else:
                empty += 1; untr.append((g, '/'.join(vsort(e['vers'])), ','.join(e['sets']), esc(k), e['loc']))
            out.append(esc(k) + '\t' + tr)
    open(os.path.join(a.out, 'wc1zh_template.txt'), 'w', encoding='utf-8', newline='').write('\n'.join(out) + '\n')
    if table is not None:
        tsv_write(os.path.join(a.out, 'wc1_untranslated.tsv'), ['group', 'versions', 'sets', 'text', 'first_location'], untr)

    # 4) 摘要
    S = ['Wing Commander 1 文字匯出摘要', '來源資料夾：'] + ['  %s ＝ %s' % (lab, gd) for lab, gd, er in sets] + ['']
    S.append('出現次數 %d；不同字串 %d（另有 %d 條只含格式碼／亂碼，列在 wc1_skipped_nontext.tsv）' % (len(OCC), len(keep), len(skipped)))
    S.append('翻譯表樣板：%d 句' % len(ent))
    if table is not None: S.append('既有譯文帶入 %d 句；尚未翻譯 %d 句（wc1_untranslated.tsv）' % (filled, empty))
    S += ['', '各群組不同字串數：'] + ['  %-16s %d' % (g, sum(1 for u in keep if u['group'] == g)) for g in GROUPS]
    byver = collections.Counter()
    for u in keep:
        for v in vsort(u['vers']): byver[v] += 1
    S += ['', '各版本涵蓋的不同字串數：'] + ['  %-8s %d' % (v, n) for v, n in sorted(byver.items(), key=lambda x: VORDER.index(x[0]) if x[0] in VORDER else 99)]
    if multi:
        S += ['', '資料夾之間的差異：']
        diff = [(fn, d) for fn, d in sorted(hashes.items()) if len(d) > 1 and len(set(d.values())) > 1]
        if diff:
            S.append('  同名但內容不同的檔案：')
            for fn, d in diff: S.append('    %s：%s' % (fn, ' ≠ '.join('%s(%s)' % (k, v[:8]) for k, v in d.items())))
        else: S.append('  所有同名文字檔內容都相同。')
        only = collections.Counter()
        for u in keep:
            if len(u['sets']) < len(sets): only[','.join(u['sets'])] += 1
        S.append('  只出現在部分資料夾的不同字串：' + ('；'.join('%s → %d 句' % (k, n) for k, n in sorted(only.items())) if only else '無'))
        S.append('  （這些句子在樣板中以「{資料夾}」標示；寫法不同＝不同句，各自一行）')
    S += ['', '注意事項：',
          '  * confidence=low 是無法確定是對話的腳本片段或很短的標籤，請目視確認（例如口型字串殘留）。',
          '  * MODULE 標籤開頭的「.」疑為旗標位元組；樣板同時列有點／無點兩種寫法。',
          '  * INTRO.DAT / INTRO1.DAT 的開場字幕未解碼；開場字幕目前由 exe 內建字串涵蓋。',
          '  * exe 字串區段以固定錨點字串界定；找不到錨點的 exe 會略過並在「訊息」列出。']
    if log: S += ['', '訊息：'] + ['  - ' + x for x in log]
    open(os.path.join(a.out, 'export_summary.txt'), 'w', encoding='utf-8').write('\n'.join(S) + '\n')
    print('\n'.join(S)); print('\n已輸出到：', os.path.abspath(a.out))

# ───────────────────────────── 子指令：check ─────────────────────────────
def cmd_check(a):
    log = []
    sets, multi, hashes = scan(a.roots, log)
    os.makedirs(a.out, exist_ok=True)
    src = build_src(multi); keys = load_entries(a.table)
    print('原文不同字串 %d；翻譯表條目 %d（比對中，需要一點時間…）' % (len(src), len(keys)))
    SM = Matcher(src)
    vers = lambda t: vers_of(src, t)
    note = lambda tab: '' if tab else 'no_tab'
    # --- 1. 翻譯表 key → 原文
    res = {}; near = {}
    for ln, k, tr, tab in keys:
        res[ln] = SM.find(k)
        if res[ln][0] is None: near[ln] = SM.nearest(k, 0.7, 5, 4)
    segidx = {}
    for t in src:
        if '\n' in t:
            for seg in t.split('\n'):
                if seg.strip(): segidx.setdefault(seg.strip(), []).append(t)
    LIP = re.compile(r'^[A-Za-z0-9$]{6,}(R[\dA-Z,]+)?$')
    rows = []; cnt = collections.Counter()
    for ln, k, tr, tab in keys:
        st, c = res[ln]
        if st is None and ' ' not in k and LIP.match(k) and (re.search(r'[\d$]', k) or k.islower()):
            cnt['JUNK_KEY'] += 1
            rows.append((ln, 'JUNK_KEY', 'lipsync/phoneme string', esc(k), esc(tr), '', '', '', '', note(tab))); continue
        if st is None and k.strip() in segidx:
            t = segidx[k.strip()][0]; cnt['KEY_SPLIT_AT_LF'] += 1
            rows.append((ln, 'KEY_SPLIT_AT_LF', 'key is one line of a multi-line source', esc(k), esc(tr), esc(t), vers(t), '; '.join(src[t]['locs'][:2]), '', note(tab))); continue
        if st is None:
            r, t = near[ln]
            if t is None:
                cnt['NO_SOURCE_FOUND'] += 1
                rows.append((ln, 'NO_SOURCE_FOUND', '', esc(k), esc(tr), '', '', '', '', note(tab)))
            else:
                dt = classify(k, t); cnt['CHANGED'] += 1
                rows.append((ln, 'CHANGED(key≠source)', dt, esc(k), esc(tr), esc(t), vers(t), '; '.join(src[t]['locs'][:2]), inline(k, t), note(tab)))
        else:
            t = c[0]; exact = (st == 'EXACT')
            dt = '' if exact else (classify(k, t) if classify(k, t) != 'same' else st)
            cnt['MATCH' if exact else 'MATCH_LENIENT_ONLY'] += 1
            rows.append((ln, 'MATCH' if exact else 'MATCH_LENIENT_ONLY', dt, esc(k), esc(tr), '' if exact else esc(t),
                         '+'.join(sorted({v for x in c for v in src[x]['vers']})), '; '.join(src[t]['locs'][:2]), '' if exact else inline(k, t), note(tab)))
    tsv_write(os.path.join(a.out, 'check_1_table_keys_vs_source.tsv'),
              ['table_line', 'status', 'diff_type', 'table_key', 'table_translation', 'source_text(nearest/lenient match)', 'source_versions', 'source_location', 'inline_diff(key→source, ·=space)', 'note'], rows)
    # --- 2. 原文 → 翻譯表
    keyinfo = {}
    for ln, k, tr, tab in keys: keyinfo.setdefault(k, (ln, tr))
    KM = Matcher(keyinfo)
    def worth(t, d):
        if all(c == 'exe:system_dialog' for c in d['cats']): return 'system'
        if not re.search(r'[A-Za-z]{2,}', t): return 'nontext'
        return 'ok'
    rows2 = []; cnt2 = collections.Counter()
    for t, d in src.items():
        if t in keyinfo: cnt2['covered_exact'] += 1; continue
        st, c = KM.find(t); w = worth(t, d)
        if st:
            k = c[0]; ln, tr = keyinfo[k]; cnt2['COVERED_LENIENT_ONLY'] += 1
            rows2.append(('COVERED_LENIENT_ONLY', st, esc(t), vers(t), ';'.join(sorted(d['cats'])), '; '.join(d['locs'][:2]), ln, esc(k), vers(k) if k in src else '', esc(tr), inline(k, t), w)); continue
        r, k = KM.nearest(t, 0.8, 5, 5)
        if k:
            ln, tr = keyinfo[k]; dt = classify(k, t); cnt2['VARIANT_OF_KEY'] += 1
            rows2.append(('VARIANT_OF_KEY', dt, esc(t), vers(t), ';'.join(sorted(d['cats'])), '; '.join(d['locs'][:2]), ln, esc(k), vers(k) if k in src else '(key not in any source)', esc(tr), inline(k, t), w))
        else:
            cnt2['NOT_IN_TABLE'] += 1
            rows2.append(('NOT_IN_TABLE', '', esc(t), vers(t), ';'.join(sorted(d['cats'])), '; '.join(d['locs'][:2]), '', '', '', '', '', w))
    rows2.sort(key=lambda r: (r[0], r[11], r[4], r[2]))
    tsv_write(os.path.join(a.out, 'check_2_source_not_exactly_in_table.tsv'),
              ['status', 'diff_type', 'source_text', 'source_versions', 'source_category', 'source_location', 'nearest_table_line', 'nearest_table_key', 'versions_where_that_key_matches_exactly', 'table_translation', 'inline_diff(key→source, ·=space)', 'relevance'], rows2)
    # --- 3. 版本矩陣
    VERS = [v for v in ('WC1', 'SM1', 'SM2', 'SM15', 'TRF') if any(v in d['vers'] for d in src.values())]
    vm = {v: Matcher([t for t, d in src.items() if v in d['vers']]) for v in VERS}
    rows3 = []
    for ln, k, tr, tab in keys:
        has = {v: k in vm[v].set for v in VERS}
        if not any(has.values()) and res[ln][0] is None and near.get(ln, (0, None))[1] is None: continue
        cells = {}; anyvar = False
        for v in VERS:
            if has[v]: cells[v] = 'OK'
            else:
                r, t = vm[v].nearest(k, 0.8, 4, 6)
                if t: cells[v] = 'VARIANT'; anyvar = True; cells[v + '_t'] = t
                else: cells[v] = '-'
        if not anyvar: continue
        rows3.append((ln, esc(k), esc(tr)) + tuple(cells[v] for v in VERS) + tuple(
            (esc(cells[v + '_t']) + '  ⟦' + classify(k, cells[v + '_t']) + '⟧  ' + inline(k, cells[v + '_t'])) if cells[v] == 'VARIANT' else '' for v in VERS))
    tsv_write(os.path.join(a.out, 'check_3_version_matrix.tsv'), ['table_line', 'table_key', 'table_translation'] + VERS + [v + '_variant(diff)' for v in VERS], rows3)
    # --- 摘要
    S = ['翻譯表檢核摘要', '翻譯表：' + a.table, '原文不同字串 %d；翻譯表條目 %d' % (len(src), len(keys)), '',
         '【翻譯表 key → 原文】check_1_table_keys_vs_source.tsv',
         '  MATCH               key 與原文逐字相同：%d' % cnt['MATCH'],
         '  MATCH_LENIENT_ONLY  只差結尾空白／空白／大小寫／標點才對上：%d' % cnt['MATCH_LENIENT_ONLY'],
         '  CHANGED             找到相近原文但寫法或用字不同：%d' % cnt['CHANGED'],
         '  KEY_SPLIT_AT_LF     key 只是多行原文的其中一行（舊表在換行處切斷）：%d' % cnt['KEY_SPLIT_AT_LF'],
         '  JUNK_KEY            key 是口型字串，不是顯示文字：%d' % cnt['JUNK_KEY'],
         '  NO_SOURCE_FOUND     原文裡找不到（詞彙表條目、黏了中文、或原文已不存在）：%d' % cnt['NO_SOURCE_FOUND'], '',
         '【原文 → 翻譯表】check_2_source_not_exactly_in_table.tsv',
         '  逐字已在表內：%d' % cnt2['covered_exact'],
         '  COVERED_LENIENT_ONLY 只差寫法，DLL 的寬鬆比對可能命中：%d' % cnt2['COVERED_LENIENT_ONLY'],
         '  VARIANT_OF_KEY       與表內某句相近但不同（需新增一行並確認譯文）：%d' % cnt2['VARIANT_OF_KEY'],
         '  NOT_IN_TABLE         表內完全沒有（需要翻譯）：%d' % cnt2['NOT_IN_TABLE'], '',
         '【版本矩陣】check_3_version_matrix.tsv：%d 個 key 在某些版本只有變體寫法（欄位 OK／VARIANT／-）' % len(rows3)]
    if log: S += ['', '訊息：'] + ['  - ' + x for x in log]
    open(os.path.join(a.out, 'check_summary.txt'), 'w', encoding='utf-8').write('\n'.join(S) + '\n')
    print('\n'.join(S)); print('\n已輸出到：', os.path.abspath(a.out))

# ───────────────────────────── 子指令：missed ─────────────────────────────
def template_regex(t):
    """含 $X 變數的原文 → 萬用字元 regex（$C/$N 等由 DLL 還原，其餘遊戲展開後的值以 .+? 代替）。"""
    parts = re.split(r'(\$[A-Z])', t); rx = ''
    for p in parts:
        rx += '(.+?)' if re.fullmatch(r'\$[A-Z]', p) else re.escape(p)
    return re.compile(rx + r'\Z', re.S)

def cmd_missed(a):
    log = []
    sets, multi, hashes = scan(a.roots, log)
    os.makedirs(a.out, exist_ok=True)
    src = build_src(multi); SM = Matcher(src)
    tmpl = [(template_regex(t), t) for t in src if re.search(r'\$[A-Z]', t)]
    table = None
    if a.table:
        table = {}
        for ln, k, tr, tab in load_entries(a.table):
            if tr.strip(): table.setdefault(k.rstrip(), tr)
        TM = Matcher(table)
    lines = open(a.missed, encoding='utf-8-sig', errors='replace').read().split('\n')
    cands = []     # (行號, 欄位們)
    for i, l in enumerate(lines, 1):
        l = l.rstrip('\r')
        if not l.strip() or l.lstrip().startswith('#'): continue
        cols = [unesc(c) for c in l.split('\t')]
        if i == 1 and any(c.strip().lower() in ('text', 'string', 'key', 'count') for c in cols): continue   # 標題列
        cands.append((i, cols))
    rows = []; frag = []; cnt = collections.Counter(); seen_frag = set()
    def lookup(s):
        s1 = s.rstrip()
        if s in src: return 'EXACT', s
        if s1 in src: return 'EXACT', s1
        st, c = SM.find(s)
        if st: return 'NORMALIZED:' + st, c[0]
        for rx, t in tmpl:
            if rx.match(s) or rx.match(s1): return 'TEMPLATE', t
        r, t = SM.nearest(s, 0.8, 5, 5)
        if t: return 'FUZZY', t
        return 'NOT_FOUND', ''
    for i, cols in cands:
        best = None
        for c in cols:                                  # 每個欄位都試，取第一個有對上原文的欄
            if not re.search(r'[A-Za-z]{2,}', c) or re.fullmatch(r'\s*(0x)?[0-9A-Fa-f]+\s*', c): continue
            r = lookup(c)
            if r[0] != 'NOT_FOUND': best = (c,) + r; break
        if best is None:
            txt = max((c for c in cols if re.search(r'[A-Za-z]{2,}', c)), key=len, default='')
            if not txt: continue
            best = (txt, 'NOT_FOUND', '')
        txt, st, t = best
        cnt[st.split(':')[0]] += 1
        d = src.get(t)
        in_tab = ''
        if table is not None and t:
            in_tab = 'yes' if t.rstrip() in table else ('lenient' if TM.find(t)[0] else 'no')
        hint = {'EXACT': '原文有此句', 'NORMALIZED': '只差空白／標點／大小寫 → 用原文寫法當 key',
                'TEMPLATE': '含 $ 變數的模板 → 表內 key 用模板原文（$C $N 由 DLL 還原，其餘遊戲先展開）',
                'FUZZY': '與原文某句相近但不同 → 可能是版本差異或執行期改寫，請人工確認',
                'NOT_FOUND': '遊戲資料找不到 → 可能是執行期組合（數字、名稱）或 exe 內的格式字串'}[st.split(':')[0]]
        if in_tab == 'yes': hint += '；表內已有該原文卻仍未命中 → 檢查折行 \\n、結尾空白、場景規則 Skip'
        elif in_tab in ('no', 'lenient') and st != 'NOT_FOUND': hint += '；表內尚無（已列入 missed_to_translate.txt）' if in_tab == 'no' else '；表內只有近似寫法'
        rows.append((i, esc(txt), st, esc(t), vers_of(src, t) if d else '', '/'.join(d['sets']) if d else '', ';'.join(sorted(d['cats'])) if d else '',
                     '; '.join(d['locs'][:2]) if d else '', in_tab, hint))
        if t and in_tab in ('no', '') and t.rstrip() not in seen_frag and st != 'NOT_FOUND' and (table is None or in_tab == 'no'):
            seen_frag.add(t.rstrip()); frag.append(esc(t.rstrip()) + '\t')
    tsv_write(os.path.join(a.out, 'missed_lookup.tsv'),
              ['line', 'missed_text', 'status', 'source_text', 'versions', 'sets', 'category', 'location', 'source_in_table' if table is not None else 'source_in_table(未給 --table)', 'hint'], rows)
    open(os.path.join(a.out, 'missed_to_translate.txt'), 'w', encoding='utf-8', newline='').write(
        '# 由 missed 查詢產生：原文有、翻譯表尚無的句子（空譯文，翻完貼進 wc1zh.txt）\n' + '\n'.join(frag) + ('\n' if frag else ''))
    S = ['未命中字串查詢摘要', '輸入：%s（%d 筆）' % (a.missed, len(rows)), ''] + ['  %-12s %d' % (k, v) for k, v in cnt.most_common()]
    S += ['', '輸出：missed_lookup.tsv（逐筆結果與處理建議）、missed_to_translate.txt（可直接翻譯的條目 %d 句）' % len(frag)]
    open(os.path.join(a.out, 'missed_summary.txt'), 'w', encoding='utf-8').write('\n'.join(S) + '\n')
    print('\n'.join(S)); print('\n已輸出到：', os.path.abspath(a.out))

# ───────────────────────────── 子指令：sync ─────────────────────────────
def cmd_sync(a):
    L = open(a.table, encoding='utf-8-sig').read().split('\n')
    def data(l): return l.strip() and not l.lstrip().startswith('#') and '\t' in l
    nm = lambda k: re.sub(r'\s+', ' ', unesc(k).lstrip('.').strip()).lower()
    tr = collections.defaultdict(set)
    for l in L:
        if data(l):
            k, t = l.rstrip('\r').split('\t', 1)
            if t.strip(): tr[nm(k)].add(t)
    n = 0; amb = []; out = []
    for l in L:
        if data(l):
            k, t = l.rstrip('\r').split('\t', 1)
            if not t.strip():
                s = tr.get(nm(k), set())
                if len(s) == 1: l = k + '\t' + next(iter(s)); n += 1
                elif len(s) > 1: amb.append(k)
        out.append(l)
    dst = a.out if a.out else os.path.splitext(a.table)[0] + '.synced.txt'
    open(dst, 'w', encoding='utf-8', newline='').write('\n'.join(out))
    print('帶入 %d 條空白譯文；同句有多種不同譯文、未帶入 %d 條' % (n, len(amb)))
    for k in amb[:20]: print('  ', k)
    print('已寫出：', os.path.abspath(dst))

# ───────────────────────────── 入口 ─────────────────────────────
def main():
    try: sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception: pass
    CMDS = ('export', 'check', 'missed', 'sync')
    argv = sys.argv[1:]
    if argv and argv[0] not in CMDS and argv[0] not in ('-h', '--help'): argv = ['export'] + argv     # 相容舊用法
    ap = argparse.ArgumentParser(prog='wc1_extract_text.py', description='Wing Commander 1 中文化文字工具（匯出／檢核／未命中查詢／譯文同步）',
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog='各指令說明：wc1_extract_text.py <指令> -h；完整操作請見「操作說明.md」')
    sub = ap.add_subparsers(dest='cmd', metavar='{export,check,missed,sync}')
    def roots(p): p.add_argument('roots', nargs='+', metavar='遊戲資料夾', help='含 GAMEDAT 與 exe 的資料夾（可多個）；也可直接指向 GAMEDAT')
    p = sub.add_parser('export', help='匯出全部遊戲文字與翻譯表樣板'); roots(p)
    p.add_argument('-o', '--out', default='wc1_text_export'); p.add_argument('--table', help='既有翻譯表：帶入譯文並列出未翻譯')
    p.add_argument('--order', choices=('normalized', 'source'), default='normalized')
    p = sub.add_parser('check', help='檢核翻譯表與遊戲原文的差異'); roots(p)
    p.add_argument('--table', required=True, help='要檢核的翻譯表 wc1zh.txt'); p.add_argument('-o', '--out', default='wc1_check')
    p = sub.add_parser('missed', help='查詢 DLL 未命中字串（wc1cht_missed.tsv）在遊戲資料中的位置'); roots(p)
    p.add_argument('--missed', required=True, help='wc1cht_missed.tsv（任何 TSV／純文字皆可）'); p.add_argument('--table', help='翻譯表（判斷是否已收錄）')
    p.add_argument('-o', '--out', default='wc1_missed')
    p = sub.add_parser('sync', help='把已翻譯的譯文帶入同一句的空白條目')
    p.add_argument('table', help='翻譯表'); p.add_argument('-o', '--out', help='輸出檔（預設：原檔名.synced.txt）')
    a = ap.parse_args(argv)
    if not a.cmd: ap.print_help(); return
    {'export': cmd_export, 'check': cmd_check, 'missed': cmd_missed, 'sync': cmd_sync}[a.cmd](a)

if __name__ == '__main__':
    main()
