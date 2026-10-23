// dm_ctl.c 的单测：平面 JSON 解析、回执格式（id 原样回、错误原因）、DM_CTL_PORT 的 TCP 传输（回执与推送的事件、换连接、纯文本 quit 不在这里测）。游戏命令用桩代替。
// Unit test for dm_ctl.c: flat JSON parsing, the reply format (id echoed as given, error reasons), the DM_CTL_PORT TCP transport (replies and pushed events, replacing the connection; the plain-text quit is not tested here). Game commands are replaced by stubs.
#include "dm.h"
#include "dm_ctl.h"
#include <mach/mach_time.h>
#include <unistd.h>

uint64_t now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int cmd_emit(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) { (void)out; (void)cap; (void)err; (void)ecap; DM_EV("hello", "k:d", (int)dm_ctl_num(a, "k", 0)); return 1; }
static int cmd_echo(const ctl_args *a, char *out, size_t cap, char *err, size_t ecap) {
    const char *v = dm_ctl_get(a, "v"); if (!v) { snprintf(err, ecap, "missing v"); return 0; }
    dm_json_fields(out, cap, "v:s n:f", v, dm_ctl_num(a, "n", -1)); return 1;
}
void dm_ctl_commands_init(void) { dm_ctl_register("echo", cmd_echo); dm_ctl_register("emit", cmd_emit); }

static char g_got[4096];
static void collect(void *ctx, const char *line, size_t n) { (void)ctx; strncat(g_got, line, n < sizeof g_got - strlen(g_got) - 1 ? n : 0); }
static const char *run(const char *line) { g_got[0] = 0; dm_ctl_handle(line, collect, NULL); return g_got; }

int main(int argc, char **argv) {
    ctl_args a;
    // --serve：当「宿主」跑 DM_CTL_PORT 的传输，由 test_ctl_transport.py 当控制端来驱动（测试里不放 listen，iOS 出包核对只许一处） | --serve: run the DM_CTL_PORT transport as the "host", driven by test_ctl_transport.py as the controller (no listen in the tests: the iOS package check allows only one)
    if (argc > 1 && !strcmp(argv[1], "--serve")) { dm_events_init(); dm_ctl_init(); for (int i = 0; i < 300; i++) usleep(100000); return 0; }
    CHECK(dm_ctl_parse("{\"id\":7,\"cmd\":\"press\",\"keys\":\"A+B\",\"frames\":12,\"on\":true}", &a) == 5, "解析字段数 | parsed field count");
    CHECK(!strcmp(dm_ctl_get(&a, "keys"), "A+B") && dm_ctl_num(&a, "frames", 0) == 12 && !strcmp(dm_ctl_get(&a, "on"), "true"), "取值 | values");
    CHECK(dm_ctl_parse(" { \"s\" : \"a\\\"b\\\\c\\n\" , \"x\" : -1.5 } ", &a) == 2 && !strcmp(dm_ctl_get(&a, "s"), "a\"b\\c\n") && dm_ctl_num(&a, "x", 0) == -1.5, "空白与转义 | whitespace and escapes");
    CHECK(dm_ctl_parse("{}", &a) == 0, "空对象 | empty object");
    const char *bad[] = {"", "[1]", "{\"a\":1", "{\"a\" 1}", "{\"a\":[1]}", "{\"a\":{\"b\":1}}", "{a:1}", "{\"a\":}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(dm_ctl_parse(bad[i], &a) < 0, "应拒绝 | should reject: %s", bad[i]);

    const char *r = run("{\"id\":3,\"cmd\":\"echo\",\"v\":\"中文\\\"q\",\"n\":2}");
    CHECK(!strcmp(r, "[dm] RE {\"id\":3,\"cmd\":\"echo\",\"ok\":true,\"v\":\"中文\\\"q\",\"n\":2}\n"), "回执 | reply: %s", r);
    r = run("{\"id\":\"abc\",\"cmd\":\"echo\"}");
    CHECK(!strcmp(r, "[dm] RE {\"id\":\"abc\",\"cmd\":\"echo\",\"ok\":false,\"err\":\"missing v\"}\n"), "串 id 与错误 | string id and error: %s", r);
    r = run("{\"cmd\":\"nope\"}");
    CHECK(strstr(r, "\"id\":null") && strstr(r, "\"err\":\"unknown cmd\""), "未知命令 | unknown command: %s", r);
    r = run("{bad");
    CHECK(strstr(r, "\"ok\":false,\"err\":\"bad json\""), "坏 JSON | bad JSON: %s", r);
    r = run("{\"id\":1,\"cmd\":\"help\"}");
    CHECK(strstr(r, "ping help echo"), "help 列出命令 | help lists the commands: %s", r);

    printf("t_ctl: %s\n", fails ? "FAIL" : "OK");
    return fails != 0;
}
