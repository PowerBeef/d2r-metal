// 引擎快照（dm_engine.c）：游戏主线程每帧读一遍游戏自己的界面与存盘状态（面板栈、焦点、手柄光标模式、存盘忙 / 待提交 / 挂着的容器），
// 发布成一份快照给控制口 state / engine、事件（action、focus、save_stuck）与红时现场用。只读游戏内存，不写。
// Engine snapshot (dm_engine.c): once per frame the game main thread reads the game's own UI and save state (panel stack, focus, controller cursor mode, save busy / commit pending / mounted containers)
// and publishes it as a snapshot for the control port's state / engine, the events (action, focus, save_stuck) and the snapshot taken on red. Reads game memory only, never writes it.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { DM_ENGINE_PANELS = 24, DM_ENGINE_NAME = 48 };
typedef struct {
    uint64_t frame;                       // 出图帧号（dm_guest_frame） | presented frame number (dm_guest_frame)
    int npanels;                          // 可见面板数（下面按游戏的优先级顺序，最后一个在最上） | number of visible panels (in the game's priority order below, the last one on top)
    struct { char name[DM_ENGINE_NAME]; uint8_t active; int32_t priority; } panels[DM_ENGINE_PANELS];
    char focus_panel[DM_ENGINE_NAME], focus[DM_ENGINE_NAME], hover[DM_ENGINE_NAME];   // 焦点所在面板、焦点控件、悬停控件（空 = 没有） | the focus's panel, the focused widget, the hovered widget (empty = none)
    int cursor_mode;                      // 手柄光标模式字节（1 = 开着自由光标的面板） | controller cursor mode byte (1 = a free-cursor panel is open)
    int cursor_state;                     // 光标状态（物品在手 / 待用目标等） | cursor state (item in hand / waiting for a target and so on)
    int save_busy, save_pending;          // 存盘忙（刷新在跑）、提交被推迟（另一个容器还挂着） | save busy (a flush is running), commit deferred (another container is still mounted)
    int containers, mounted, dirty;       // 存档容器数、挂着的、脏的 | save containers, mounted ones, dirty ones
} dm_engine_snap;

void dm_engine_pump(uint64_t root);       // 游戏主线程，每帧（dm_cursor.c 每帧那一行） | game main thread, every frame (the per-frame line in dm_cursor.c)
void dm_engine_get(dm_engine_snap *out);  // 任意线程：拷一份最新快照 | any thread: copy the latest snapshot
void dm_engine_input_msg(uint64_t act_hash, uint64_t payload);   // diag.c 界面消息观测：InputMessage 类的每条消息 | diag.c UI-message observer: every message of the InputMessage category
void dm_engine_save_requested(void);      // 宿主请游戏存盘成功（dm_request_save）：存盘卡住的计时从这里起 | the host asked the game to save (dm_request_save): the stuck-save timer starts here
// 控制口：state 的引擎字段（「,"名":值…」）与 engine 命令的完整快照（再加最近的动作与界面消息）；返回写了多长
// Control port: the engine fields of state (",\"name\":value...") and the engine command's full snapshot (plus the latest actions and UI messages); returns the length written
size_t dm_engine_state_json(char *out, size_t cap);
size_t dm_engine_full_json(char *out, size_t cap);
