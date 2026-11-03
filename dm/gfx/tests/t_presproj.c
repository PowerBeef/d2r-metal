// Unit test of nvn_presproj.h: a real presented time is kept; the link's target time stands in only when projection is on and the target is known; otherwise -1
#include "../nvn/nvn_presproj.h"
#include <stdio.h>

int main(void) {
    struct { double presented; int project; double target, want; const char *why; } c[] = {
        {12.5, 0, 0, 12.5, "real presented time, projection off: kept"},
        {12.5, 1, 12.6, 12.5, "real presented time, projection on: kept (never replaced by the target)"},
        {0, 1, 12.6, 12.6, "no presented time, projection on, target known: the target"},
        {0, 0, 12.6, -1, "no presented time, projection off: -1 (as before)"},
        {0, 1, 0, -1, "no presented time, projection on, no target (classic / rescue present): -1"},
        {-1, 1, 12.6, 12.6, "a negative reading counts as none"},
    };
    int fail = 0, n = sizeof c / sizeof *c;
    for (int i = 0; i < n; i++) {
        double got = nvn_presented_or_projected(c[i].presented, c[i].project, c[i].target); int ok = got == c[i].want;
        printf("%-80s want %6.2f got %6.2f %s\n", c[i].why, c[i].want, got, ok ? "✓" : "✗"); fail += !ok;
    }
    printf("PRESPROJ %s %d/%d\n", fail ? "RED" : "OK", n - fail, n);
    return fail != 0;
}
