// 控制口（dm_ctl.c）：按行的 JSON 命令 → 「[dm] RE {json}」回执；协议见 dm/verify/control_port.md
// Control port (dm_ctl.c): JSON commands one per line -> "[dm] RE {json}" replies; protocol in dm/verify/control_port.md
#pragma once
#include <stddef.h>

typedef struct { char key[32]; char val[512]; int str; } ctl_kv;
typedef struct { ctl_kv kv[16]; int n; } ctl_args;
typedef void (*dm_ctl_reply)(void *ctx, const char *line, size_t n);   // 收一整行回执（含换行） | receives one whole reply line (with the newline)
typedef int (*dm_ctl_fn)(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap);   // 成功返回 1，结果写成「,"名":值」进 out；失败返回 0、原因进 err | returns 1 on success with results written into out as ",\"name\":value"; returns 0 on failure with the reason in err

void dm_ctl_init(void);                                          // 登记命令；设了 DM_CTL_PORT 就起监听（Mac） | registers the commands; starts the listener when DM_CTL_PORT is set (Mac)
void dm_ctl_handle(const char *line, dm_ctl_reply reply, void *ctx);   // 处理一行命令（任一线程；会阻塞到命令做完） | handles one command line (any thread; blocks until the command is done)
void dm_ctl_register(const char *name, dm_ctl_fn fn);
void dm_ctl_commands_init(void);                                 // dm_ctl_cmds.c：登记游戏相关的命令 | dm_ctl_cmds.c: registers the game-facing commands
int dm_ctl_parse(const char *line, ctl_args *a);                  // 平面 JSON 对象；返回字段数，失败 -1 | flat JSON object; returns the field count, -1 on failure
const char *dm_ctl_get(const ctl_args *a, const char *k);
double dm_ctl_num(const ctl_args *a, const char *k, double def);
unsigned dm_ctl_dropped(void);                                   // 队列满丢掉的事件数 | events dropped because the queue was full
unsigned dm_ctl_commands(void);
// 前端生命周期钩子（front_mac.m / front_ios.m 设；控制口 app 命令调）：what = background / foreground / resign / active / minimize / restore / terminate，
// 在主线程上走与系统事件、DM_TEST_BG / DM_TEST_MINI_AT 同一个处理函数；做完返回 0，这个前端不支持返回 -1
// Front-end lifecycle hook (set by front_mac.m / front_ios.m; called by the control port's app command): what = background / foreground / resign / active / minimize / restore / terminate,
// run on the main thread through the same handler as the system events and DM_TEST_BG / DM_TEST_MINI_AT; returns 0 when done, -1 when this front end does not support it
__attribute__((weak)) int (*g_dm_ctl_app_fn)(const char *what) = 0;
// 前端测试输入（front_mac.m 设；控制口 fe 命令调）：一行同 DM_TEST_NSMOUSE 的「秒 动作 参数」（pad / pads / navpad / nav / skey / sheet / key / type / down / move / up / hover / dragto / dbl / unit），秒自此刻起算；
// 返回 1 = 排好、0 = 不认识这一行、-1 = 窗口还没建
// Front-end test input (set by front_mac.m; called by the control port's fe command): one line in DM_TEST_NSMOUSE's "seconds action arguments" form (pad / pads / navpad / nav / skey / sheet / key / type / down / move / up / hover / dragto / dbl / unit), seconds counted from now;
// returns 1 = scheduled, 0 = the line was not understood, -1 = the window is not built yet
__attribute__((weak)) int (*g_dm_ctl_fe_fn)(const char *line) = 0;
