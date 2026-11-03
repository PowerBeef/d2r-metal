#!/usr/bin/env python3
"""The PC interface set's console panels, drawn smaller and higher (Mac App, DM_OVERLAY_MAC; gen_romfs_overlay.py pc_set).

The PC set keeps the console item panels (the Switch build's PC item grids take no clicks), but they are laid out for the console HUD: scaled by the
console profile's ConsoleUIScale, the panel group's frame reaches the bottom of the screen and runs behind the PC HUD (orbs, statues, life and mana).
Every in-game console panel gets its place from a rectangle in the console profile (GROUP: ControllerOverlayRect, ConsoleLeftPanelRect,
PauseLayoutRect, ...), each scaled by ConsoleUIScale, set on the panel or on a child, with an anchor of its own or inherited from the layout it is
based on. This module shrinks the whole group by one factor about one point (the top centre of the screen), so the frame and its pages stay lined
up and end above the PC HUD: for each use of such a rectangle it works out the anchor in force and the rectangle's origin on the 3840 x 2160
interface, and writes a literal rectangle at the moved origin with the reduced scale. Width and height stay (they are in the panel's own units).

build(names, read) -> {path under layouts/: text}: for each layout name the game looks up (by its controller-mode path: console/switch, console,
controller, then the root), the PC copy, where anything in its basedOn chain uses a GROUP rectangle: the top-level rectangle the panel ends up with
is written into its own fields; children that use one are moved where they are written, and a base holding such a child gets its own copy in
BASE, with the copy's basedOn pointing there. read(path) gives a layout's text as controller mode loads it (the overlay's copy where it replaces
one), or None. Only key names and arithmetic live here; the layouts come from the player's game files. Standard library only.
"""
import re

SCALE = 0.82                      # the group's size against the console layout (ConsoleUIScale 1.16 -> about 0.95)
PIVOT = (1920.0, 0.0)             # top centre of the 3840 x 2160 interface
W, H = 3840.0, 2160.0
PROFILES = ('controller/_profileconsolehd.json', 'controller/_profilehd.json', '_profilehd.json')   # the console profile and what it is based on
GROUP = ('ControllerOverlayRect', 'UISwitcherRect', 'ConsoleLeftPanelRect', 'ConsoleRightPanelRect', 'ConsoleCenterPanelRect', 'CharacterStatsPanelRect',
         'QuestLogPanelOriginalRect', 'QuestLogSpeechPanelRect', 'PauseLayoutRect', 'MessageLogPanelRect', 'AdvancedStatsPanelRect', 'SettingsPanelRect',
         'SettingsPanelAnchorRect', 'SettingsPanelBackgroundRect', 'SettingsPanelTabsRect', 'OptionsPanelRect', 'ChronicleRewardsControllerRect')
ALSO = ('ActionButtonsTable', 'TouchClose')   # widgets an Options-style panel places on the screen itself (Apply / Default / Discard, our close X): moved with it
BASE = '5pc_base_lay/'            # copies of base layouts whose children move (a new folder under layouts/, as casc_overlay.py can add)
_USE = re.compile(r'"rect"\s*:\s*"\$(%s)"' % '|'.join(GROUP))
_ANY = re.compile(r'"\$(%s)"' % '|'.join(GROUP))


def _pairs(s):   # brace pairs {open: close}, skipping strings and // or /* */ comments
    out, st, i, n = {}, [], 0, len(s)
    while i < n:
        c = s[i]
        if c == '"':
            i += 1
            while i < n and s[i] != '"': i += 2 if s[i] == '\\' else 1
        elif c == '/' and i + 1 < n and s[i + 1] == '/':
            i = s.find('\n', i); i = n if i < 0 else i
        elif c == '/' and i + 1 < n and s[i + 1] == '*':
            i = s.find('*/', i + 2); i = n if i < 0 else i + 1
        elif c == '{': st.append(i)
        elif c == '}' and st: out[st.pop()] = i
        i += 1
    return out


def _enclosing(pairs, i):   # the innermost {...} holding position i
    best = None
    for a, b in pairs.items():
        if a < i < b and (best is None or a > best[0]): best = (a, b)
    return best


def _top_fields(s, pairs):   # (open, close) of the root object's "fields" object
    if not pairs: return None
    root = min(pairs)
    for m in re.finditer(r'"fields"\s*:\s*\{', s):
        o = m.end() - 1
        if _enclosing(pairs, o) == (root, pairs[root]): return o, pairs[o]
    return None


class Profile:
    def __init__(self, read):
        self.texts = [read(p) or '' for p in PROFILES]

    def raw(self, name):   # the profile value's text for $name, nearest profile first
        for t in self.texts:
            m = re.search(r'"%s"\s*:\s*' % re.escape(name), t)
            if not m: continue
            j = m.end()
            if t[j] == '{': return t[j:j + _pairs(t[j:])[0] + 1]
            if t[j] == '"': return t[j:t.index('"', j + 1) + 1]
            n = re.match(r'[-0-9.]+', t[j:])
            return n.group(0) if n else None
        return None

    def obj(self, raw):   # '{"x": 1, "scale": "$ConsoleUIScale"}' or '"$Name"' -> dict, $ values resolved where they are numbers
        out = {}
        if raw is None: return out
        raw = raw.strip()
        if raw.startswith('"$'): raw = self.raw(raw[2:-1])
        if raw is None or not raw.startswith('{'): return out
        for k, v in re.findall(r'"(\w+)"\s*:\s*("[^"]*"|[-0-9.]+)', raw):
            if v.startswith('"$'):
                r = self.raw(v[2:-1])
                try: out[k] = float(r) if r is not None else v
                except ValueError: out[k] = v
            elif v.startswith('"'): out[k] = v
            else: out[k] = float(v)
        return out


def _move(rect, anchor):
    ax, ay = anchor.get('x', 0.0), anchor.get('y', 0.0)
    ox, oy = ax * W + rect.get('x', 0.0), ay * H + rect.get('y', 0.0)
    nx, ny = PIVOT[0] + (ox - PIVOT[0]) * SCALE, PIVOT[1] + (oy - PIVOT[1]) * SCALE
    parts = ['"x": %d' % round(nx - ax * W), '"y": %d' % round(ny - ay * H)]
    for k in ('width', 'height'):
        if k in rect: parts.append('"%s": %s' % (k, rect[k] if isinstance(rect[k], str) else '%d' % round(rect[k])))
    parts.append('"scale": %.4f' % (rect.get('scale', 1.0) * SCALE))
    return '{ ' + ', '.join(parts) + ' }'


def _base(s):   # the basedOn value and its path under layouts/ ("Controller/X.json" -> controller/x.json)
    m = re.search(r'"basedOn"\s*:\s*"([^"]*)"', s[:800])
    return (m, m.group(1).replace('\\', '/').lower()) if m else (None, None)


def _chain_uses(path, read, depth=0):   # does this layout or a layout it is based on name a GROUP rectangle anywhere
    s = read(path)
    if s is None or depth > 8: return False
    if _ANY.search(s): return True
    m, b = _base(s)
    return bool(m) and _chain_uses(b, read, depth + 1)


def _effective(path, read, prof, depth=0):   # (GROUP name or None, anchor dict) of the root panel's rectangle, following basedOn
    s = read(path)
    if s is None or depth > 8: return None, {}
    pairs = _pairs(s); tf = _top_fields(s, pairs); name, anchor, has_rect = None, None, False
    if tf:
        body = s[tf[0]:tf[1] + 1]
        r = re.search(r'"rect"\s*:\s*("\$\w+"|\{[^}]*\})', body)
        if r: has_rect = True; u = _USE.match(r.group(0)); name = u.group(1) if u else None
        a = re.search(r'"anchor"\s*:\s*("\$\w+"|\{[^}]*\})', body); anchor = prof.obj(a.group(1)) if a else None
    m, b = _base(s)
    if (not has_rect or anchor is None) and m:
        bn, ba = _effective(b, read, prof, depth + 1)
        if not has_rect: name = bn
        if anchor is None: anchor = ba
    return name, anchor or {}


def _move_uses(s, path, read, prof):   # every GROUP rectangle written in this layout, moved
    pairs = _pairs(s); tf = _top_fields(s, pairs); edits = []
    for m in _USE.finditer(s):
        blk = _enclosing(pairs, m.start())
        a = re.search(r'"anchor"\s*:\s*("\$\w+"|\{[^}]*\})', s[blk[0]:blk[1] + 1]) if blk else None
        if a: anchor = prof.obj(a.group(1))
        elif tf and blk == tf: anchor = _effective(path, read, prof)[1]
        else: anchor = {}
        edits.append((m.start(), m.end(), '"rect": ' + _move(prof.obj('"$%s"' % m.group(1)), anchor)))
    for m in re.finditer(r'"name"\s*:\s*"(%s)"' % '|'.join(ALSO), s):   # their own literal rectangle, with their anchor
        w = _enclosing(pairs, m.start())
        f = re.compile(r'"fields"\s*:\s*\{').search(s, m.end(), w[1]) if w else None
        if not f: continue
        fo = f.end() - 1; fc = pairs.get(fo)
        if fc is None: continue
        r = re.compile(r'"rect"\s*:\s*(\{[^}]*\})').search(s, fo, fc)
        if not r or _enclosing(pairs, r.start()) != (fo, fc): continue
        a = re.compile(r'"anchor"\s*:\s*("\$\w+"|\{[^}]*\})').search(s, fo, fc)
        edits.append((r.start(1), r.end(1), _move(prof.obj(r.group(1)), prof.obj(a.group(1)) if a else {})))
    for a, b, new in sorted(edits, reverse=True): s = s[:a] + new + s[b:]
    return s


def _pc_copy(path, read, prof, out, depth=0):   # the moved text of this layout; its base, where it holds moved children, copied into BASE
    s = read(path)
    if s is None or depth > 8: return None
    t = _move_uses(s, path, read, prof)
    m, b = _base(t)
    if m and _chain_uses(b, read):
        bt = _pc_copy(b, read, prof, out, depth + 1)
        if bt is not None:
            key = BASE + b.replace('/', '__'); out[key] = bt
            t = t[:m.start(1)] + BASE + m.group(1).replace('\\', '/').replace('/', '__') + t[m.end(1):]
    return t if t != s else None


def _leaf(path, read, prof, out):   # the PC copy of a layout the game loads by name (None = unchanged)
    orig = read(path)
    if orig is None or not _chain_uses(path, read): return None
    s = _pc_copy(path, read, prof, out) or orig
    gname, anchor = _effective(path, read, prof)
    pairs = _pairs(s); tf = _top_fields(s, pairs)
    if gname and tf and not re.search(r'"rect"\s*:', s[tf[0]:tf[1] + 1]):   # the panel inherits its rectangle: the moved one goes into its own fields
        s = s[:tf[0] + 1] + '\n        "rect": %s,' % _move(prof.obj('"$%s"' % gname), anchor) + s[tf[0] + 1:]
    return s if s != orig else None


def build(names, read, renamed=()):
    """names: [(lookup paths in the game's order, name)]; renamed: [(path, old panel name, new panel name)] for a page opened by a panel name that
    holds its folder (our Extras page, 2extras_lay/): its PC copy, named the new name, goes into BASE and the copies that open it are pointed there
    (dm_custom.c knows both names)."""
    prof = Profile(read); out = {}
    for lookup, name in names:
        found = next((p for p in lookup if read(p) is not None), None)
        s = _leaf(found, read, prof, out) if found else None
        if s is not None: out[(found, name)] = s
    for path, old, new in renamed:
        s = _leaf(path, read, prof, out)
        if s is None: continue
        out[BASE + path.rsplit('/', 1)[-1]] = s.replace('"name": "%s"' % old, '"name": "%s"' % new)   # its name too, or the game opens it under one name and keeps it under another (it never closes)
        for k, v in list(out.items()):
            if old in v and k != BASE + path.rsplit('/', 1)[-1]: out[k] = v.replace('CheckChanges:' + old, 'CheckChanges:' + new)
    return out


if __name__ == '__main__':   # preview: pc_panels.py <RomFS> lists what it would write, with each root rectangle
    import os, sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import romfs_art
    R = sys.argv[1]; LAY = 'data/global/ui/layouts/'
    man = {l.split('|')[9].lower().split(':', 1)[-1] for l in open(os.path.join(R, 'data', 'build_manifest.psv'), encoding='utf-8') if len(l.split('|')) >= 11}
    def rd(p):
        k = LAY + p
        return romfs_art.read(R, k).decode('utf-8', 'replace') if k in man else None
    names = sorted({k[len(LAY):].rsplit('/', 1)[-1] for k in man if k.startswith(LAY) and k.endswith('.json') and '/_profile' not in k and '/pc/' not in k and '/handheld/' not in k})
    dirs = ('controller/console/switch/', 'controller/console/', 'controller/', '')
    for k, t in sorted(build([([d + n for d in dirs], n) for n in names], rd).items(), key=str):
        m = re.search(r'"rect"\s*:\s*(\{[^}]*\})', t); print(k, m.group(1) if m else '')
