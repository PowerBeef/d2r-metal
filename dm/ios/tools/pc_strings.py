#!/usr/bin/env python3
"""The PC interface set's wording (Mac App, DM_OVERLAY_MAC): console prompts that name a controller button get the game's own PC sentence in the
same language ("Hold <A> to Use" -> the PC string of the same action, "Right Click to Use"), or are blanked where PC mode has no such action.
Nothing of the game's text is stored here: only key names. build_sidecar(romfs) -> [(key, checksum of the original, PC wording)], one row per key
and language the game has; gen_romfs_overlay.py writes them as pc_strings.tsv, which the host's dm_padmode.c (str_load / str_apply) swaps in while
the PC set is on screen, as touch_strings.tsv does in touch mode. Each row's checksum is the first 8 hex digits of the SHA-1 of the console string
in that language, so the host swaps only the rows of the language the game is in. Standard library + romfs_art.
"""
import hashlib, json
import romfs_art

STR = 'data/local/lng/strings/'
# console key -> (file, PC key): the PC string of the same action. Right Click uses an item in PC mode (dm_kbm.c), Shift compares (dm_cursor.c),
# Esc continues after death (dm_kbm.c sends the death screen's Continue message)
TWIN = {
    'UseItemController': ('ui.json', 'UseItem'),
    'OpenHoradricCubeController': ('ui.json', 'OpenHoradricCube'),
    'ReadScrollController': ('ui.json', 'ReadScroll'),
    'UsePotionOfLifeController': ('ui.json', 'UsePotionOfLife'),
    'UseBookOfSkillController': ('ui.json', 'UseBookOfSkill'),
    'UseScrollOfResistanceController': ('ui.json', 'UseScrollOfResistance'),
    'UseTokenOfAbsolutionController': ('item-modifiers.json', 'UseTokenOfAbsolution'),
    'UseTerrorTokenAct1Controller': ('item-modifiers.json', 'UseTerrorTokenAct1'),
    'UseTerrorTokenAct2Controller': ('item-modifiers.json', 'UseTerrorTokenAct2'),
    'UseTerrorTokenAct3Controller': ('item-modifiers.json', 'UseTerrorTokenAct3'),
    'UseTerrorTokenAct4Controller': ('item-modifiers.json', 'UseTerrorTokenAct4'),
    'UseTerrorTokenAct5Controller': ('item-modifiers.json', 'UseTerrorTokenAct5'),
    'comparisonTooltipController': ('ui.json', 'holdShift'),
    'hireComparisonTooltipController': ('ui.json', 'hireComparisonTooltip'),
    'StrYouDiedConsole': ('ui.json', 'StrYouDied'),
}
# console prompts with no PC action behind them: a click on the field / button does it (name entry), or PC mode has no such button
BLANK = ('ControllerCycleComparisonTooltips', 'ControllerInteractButton', 'EnterCharacterName', 'SelectButtonNoteText')
CONSOLE = 'ui-controller.json'


def _table(romfs, name):
    return {e['Key']: e for e in json.loads(romfs_art.read(romfs, STR + name).decode('utf-8-sig'))}


def build_sidecar(romfs):
    con = _table(romfs, CONSOLE); files = {}; out = []
    langs = [k for k in next(iter(con.values())) if k not in ('id', 'Key')]
    for key in list(TWIN) + list(BLANK):
        if key not in con: raise SystemExit('pc_strings: %s has no key %s (another game version?)' % (CONSOLE, key))
        twin = None
        if key in TWIN:
            f, pk = TWIN[key]; t = files[f] if f in files else files.setdefault(f, _table(romfs, f))
            if pk not in t: raise SystemExit('pc_strings: %s has no key %s (another game version?)' % (f, pk))
            twin = t[pk]
        for lang in langs:
            cur = con[key].get(lang)
            if not cur: continue
            new = twin.get(lang, '') if twin else ''
            if twin and not new: continue   # no PC sentence in this language: the console one stays
            row = (key, hashlib.sha1(cur.encode('utf-8')).hexdigest()[:8], new)
            if row not in out: out.append(row)   # languages sharing a sentence (esES / esMX) need one row
    return out


if __name__ == '__main__':   # preview: pc_strings.py <RomFS> prints the rows
    import sys
    for k, c, v in build_sidecar(sys.argv[1]): print(k, c, repr(v))
