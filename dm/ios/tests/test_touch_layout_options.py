"""现有自定义页中两位置换组开关的跨语言契约。 | Cross-language contract for the two-position switch in the existing Custom page. | Cross-language contract for the two-position switch in the existing Custom page."""
import importlib
import json
import os
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'ios/tools'))
os.environ.pop('MOVIE_FFMPEG', None)   # 缺省包的自定义页（「配音」行只在 MOVIE_FFMPEG=1 时有，下面另核）
import touch_layout as layout

out, _ = layout._custom_page('"name": "VideoOptionsPanel", "name": "OptionsTable"')
page = json.loads(re.sub(r',\s*([}\]])', r'\1', out))
nodes = {}
def walk(node):
    if isinstance(node, dict):
        if 'name' in node:
            assert node['name'] not in nodes, node['name']
            nodes[node['name']] = node
        for child in node.values():
            walk(child)
    elif isinstance(node, list):
        for child in node:
            walk(child)
walk(page)
for name in ('TouchCustom_touch_swap_above', 'TouchCustom_touch_swap_above_near'):
    assert nodes[name]['type'] == 'ToggleButtonWidget'
    assert nodes[name]['fields']['onClickMessage'].endswith(name)
assert nodes['TouchCustom_touch_swap_above_Text']['fields']['rect']['height'] == 360
for suffix in ('Near', 'Far'):
    assert any(n.get('fields', {}).get('text') == '@dmCustomTouchSwap' + suffix for n in nodes.values())
host_src = (root / 'host/dm_custom.c').read_text()
def host_variant(voice):   # 宿主按 VOICE_ROW（DM_MOVIE_FFMPEG）条件编译的两份键表：缺省去掉 #if VOICE_ROW 块，开关包只去掉预处理行
    keep = re.sub(r'#if VOICE_ROW\n(.*?)#endif\n', (lambda m: m[1]) if voice else '', host_src, flags=re.S)
    return re.sub(r'(?m)^#.*\n', '', keep)
for voice in (False, True):
    if voice:
        os.environ['MOVIE_FFMPEG'] = '1'; layout = importlib.reload(layout)
    host = host_variant(voice)
    keys = re.findall(r'"([a-z_0-9]+)"', re.search(r'k_keys\[\].*?=\s*\{(.*?)\}', host, re.S)[1])
    assert keys == [key for key, _ in layout._CUSTOM_ALL], (voice, keys)
    assert ('voice' in keys) == voice
    defaults = [x.strip() for x in re.search(r'k_def\[NK\].*?=\s*\{(.*?)\}', host, re.S)[1].split(',')]   # 缺省值可以是平台式子（q3d：!DM_PHONE = Mac 画质优先、手机流畅优先） | a default may be a platform expression (q3d: !DM_PHONE = quality first on the Mac, smoothness first on phones)
    assert all(re.fullmatch(r'-?\d+|!?DM_PHONE', x) for x in defaults), defaults
    defaults = [int(x) if re.fullmatch(r'-?\d+', x) else x for x in defaults]
    assert len(defaults) == len(keys) and defaults[keys.index('touch_swap_above')] == 0
os.environ.pop('MOVIE_FFMPEG'); layout = importlib.reload(layout)
host = host_src
assert 'TouchCustom_touch_swap_above_near' in host
# Mac / iOS：「帧率」「掉帧时」是「视频」页签上的切换框（宿主 dm_video.c 读写同一对键 eco_fps / q3d），「附加」页上没有这两行；安卓的自定义页（不设覆盖层开关）照旧是勾选行
# Mac / iOS: Frame Rate and Frame Drops are selectors on the Video tab (the host's dm_video.c reads and writes the same keys eco_fps / q3d) and not rows on the Extras page; Android's custom page (no overlay flag) keeps the checkbox rows
FAKE = '"name": "VideoOptionsPanel", "name": "OptionsTable"'
def page_nodes(text):
    found = {}
    def visit(node):
        if isinstance(node, dict):
            if 'name' in node: found[node['name']] = node
            for child in node.values(): visit(child)
        elif isinstance(node, list):
            for child in node: visit(child)
    visit(json.loads(re.sub(r',\s*([}\]])', r'\1', text)))
    return found
all_keys = [k for k, _ in layout._CUSTOM_ROWS]
assert 'TouchCustom_eco_fps' in nodes and 'TouchCustom_q3d' in nodes and [k for k, _ in layout._CUSTOM_ALL][1:] == all_keys
page_rows = {}
for flag in ('DM_OVERLAY_MAC', 'DM_OVERLAY_IOS'):
    os.environ[flag] = '1'; layout = importlib.reload(layout)
    video = page_nodes(layout._custom_page(FAKE, 'video')[0]); extras = page_nodes(layout._custom_page(FAKE, 'extras')[0])
    for row, states in (('fps', ['dmVideoFps60', 'dmVideoFps30']), ('drops', ['dmVideoDropsLower', 'dmVideoDropsKeep'])):
        w = video['TouchVideo_' + row]
        assert w['type'] == 'CarouselWidget' and w['fields']['states'] == states, (flag, row, w)
    assert not {'TouchCustom_eco_fps', 'TouchCustom_q3d', 'TouchCustom_q3d_smooth'} & set(extras), flag
    assert [k for k, _ in layout._CUSTOM_ALL][1:] == [k for k in all_keys if k not in ('eco_fps', 'q3d')], flag
    page_rows[flag] = [r[0] for r in layout._VIDEO if r[0] != 'hdr']
    os.environ.pop(flag); layout = importlib.reload(layout)
# 宿主按 dm_video.c k_rows 的顺序认页上的行（手柄上下、第几行）：每个平台页上的行必须是 k_rows 按原顺序挑出的一部分；fps / drops 是自定义设置的键
# The host walks the page's rows in dm_video.c k_rows order (controller up / down, row numbers): each platform's page rows must be k_rows in the same order with some left out; fps / drops are custom settings keys
video_src = (root / 'host/dm_video.c').read_text()
k_rows = re.findall(r'"(\w+)"', re.search(r'k_rows\[NR\] = \{([^}]*)\}', video_src).group(1))
ckey = re.findall(r'(0|"\w+")', re.search(r'k_row_ckey\[NR\] = \{([^}]*)\}', video_src).group(1))
assert len(ckey) == len(k_rows) and dict(zip(k_rows, ckey))['fps'] == '"eco_fps"' and dict(zip(k_rows, ckey))['drops'] == '"q3d"', (k_rows, ckey)
for flag, rows in page_rows.items():
    it = iter(k_rows); assert all(r in it for r in rows), (flag, rows, k_rows)
assert page_rows['DM_OVERLAY_MAC'] == k_rows, page_rows['DM_OVERLAY_MAC']   # Mac 页上全有 | the Mac page has every row
print('PASS touch layout options: generated radio pair, labels, defaults, host order (default and MOVIE_FFMPEG=1), scrolling row extent, and Frame Rate / Frame Drops on the Mac and iOS Video tab')
