// 「自定义」设置：我们自己加的功能的启停开关，统一在这里读写。各模块只调 dm_custom_get，不各自读文件 / 环境变量当设置。
// 「自定义」(Custom) settings: on/off switches for the features we added ourselves, all read and written here. Each module only calls dm_custom_get and does not read files / environment variables as settings on its own.
#pragma once

// Effective value 0 / 1: changes only after the player taps Apply on the options page; Discard leaves it unchanged; Default = each key's default (power-saving mode off, the rest on). Returns 1 for an unknown key.
// Callable from any thread: the first call reads the file once, after that it is an atomic read plus a few string comparisons, lock-free. On iOS it can be read during startup (before the save is mounted).
// Keys: drop_light light beam on high-value drops / item_notes item notes and old translated names / icloud iCloud save sync (cloud_ios.m, read at startup) / pot_pick potion auto-pickup (dm_potpick.c) / drop_haptic haptics on high-value drops (dm_drophaptic.c) / eco_fps power-saving mode at 30 fps (nvn_mtl.m, off by default)
// / q3d Frame Drops (below) / touch_swap_above 0 = left of the portal (default), 1 = above the skill fan.
int dm_custom_get(const char *key);
int dm_custom_lang_explicit(void);   // 语言开关（键 lang_en）在设置文件里写着的值：-1 没写过（不动原版设置）/ 0 简中 / 1 英文；hle_fs.c 挂存档时读 | Value of the language switch (key lang_en) as written in the settings file: -1 never written (original settings left alone) / 0 Simplified Chinese / 1 English; read by hle_fs.c at save mount
int dm_custom_voice_explicit(void);   // 单独配音开关（键 voice）：-1 没写过 / 0 中文配音 / 1 英文配音；hle_fs.c 挂存档时读
// q3d Frame Drops (0 = lower the 3D resolution for a while when frames drop, 1 = keep it): the default is 0 on phones, the recommendation's choice in the Mac App and 1 on the CLI host;
// saved only when it differs from that default (custom_v 2); applied on load / Apply through hle_oe.c dm_3d_quality_mode_set
int dm_fps_setting(void);   // 省电模式（键 eco_fps，缺省关）：开 = 30 帧、关 = 60；nvn_mtl.m mtl_fps_target 读 | Power-saving mode (key eco_fps, off by default): on = 30 fps, off = 60; read by mtl_fps_target in nvn_mtl.m

void dm_custom_init(const char *dir);   // 设置文件所在目录（iOS 外壳启动时给 Documents）；不调则用存档提交目录的上一级（Mac 宿主） | Directory holding the settings file (the iOS shell passes Documents at startup); if not called, the parent of the save commit directory is used (Mac host)

// 以下只给自定义页的界面接线用
// The following are only for the UI wiring of the custom page
void dm_custom_on_uimsg(const char *cat, const char *act, const char *arg);   // 选项页应用 / 默认 / 放弃（dm_cursor.c 的界面消息回调转来） | Options page apply / default / discard (forwarded from the UI message callback in dm_cursor.c)
void dm_custom_ui_pump(unsigned long long root);                               // 游戏主线程每帧：选项页开着时读各行勾选框（控件名 TouchCustom_<键>） | Game main thread, every frame: while the options page is open, read the checkbox of each row (widget name TouchCustom_<key>)
