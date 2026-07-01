/* m5fand -- M5 Pro fan daemon
 * Reads all temps from one smc -l call per cycle, applies hysteresis-armed
 * fan curve, writes target speed. Logs to ~/Library/Logs/m5fand.log.
 *
 * Build: clang -std=c99 -Wall -Wextra -O2 -o m5fand main.c
 * Install: cp m5fand ~/.local/bin/m5fand
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SMC    "/Users/jwalinshah/.local/bin/smc"

/* ── Read max temp for sensor prefix from one smc -l call ── */
static float max_temp(const char *pfx) {
    FILE *fp = popen(SMC " -l 2>/dev/null", "r"); if (!fp) return 0;
    char line[128]; float mx = 0; size_t pl = strlen(pfx);
    while (fgets(line, sizeof line, fp)) {
        char *bracket = strrchr(line, ']');
        if (!bracket) continue;
        char *end = NULL;
        float v = strtof(bracket + 1, &end);
        if (end == bracket + 1) continue;
        char k[5] = {line[2], line[3], line[4], line[5], 0};
        if (strncmp(k, pfx, pl) == 0 && v > mx) mx = v;
    }
    pclose(fp); return mx;
}

static void w32(const char *k, uint32_t v) {
    char cmd[128], h[9];
    snprintf(h, sizeof h, "%08x", v);
    snprintf(cmd, sizeof cmd, SMC " -k %s -w %s 2>/dev/null", k, h);
    FILE *fp = popen(cmd, "r"); if (fp) pclose(fp);
}
static void w1(const char *k, uint8_t v) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, SMC " -k %s -w %02x 2>/dev/null", k, v);
    FILE *fp = popen(cmd, "r"); if (fp) pclose(fp);
}

/* ── Fan curve table (2-degree steps, sigmoid) ──────────── */
static const int cv_t[]  = {30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80,82,84,86,88};
static const uint32_t cv_h[] = {0x00c0ac44,0x0020ae44,0x0020b044,0x00a0b244,0x0000b644,0x0080ba44,0x0060c044,0x0020c844,0x0040d244,0x0040df44,0x00401c45,0x00803b45,0x00006145,0x00408345,0x00e09245,0x00409c45,0x00f0a045,0x0010a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445};
static const uint16_t cv_s[] = {1382,1393,1409,1429,1456,1492,1539,1601,1682,1786,2500,3000,3600,4200,4700,5000,5150,5250,5269,5269,5269,5269,5269,5269,5269,5269,5269,5269,5269,5269};
#define CLEN (sizeof cv_t / sizeof cv_t[0])

/* ── Main ──────────────────────────────────────────────── */
int main(void) {
    const char *hd = getenv("HOME"); if (!hd) hd = "/Users/jwalinshah";
    char lp[256]; snprintf(lp, sizeof lp, "%s/Library/Logs/m5fand.log", hd);
    FILE *lf = fopen(lp, "a"); if (lf) setlinebuf(lf);

    w1("F0md",1); w1("F1md",1);
    if (lf) fprintf(lf, "m5fand: SMC OK, fans forced.\n");
    fprintf(stdout, "m5fand: SMC OK, fans forced.\n");

    int last_idx = -1;
    while (1) {
        float cpu = max_temp("Tp"), gpu = max_temp("Tg"), pk = cpu > gpu ? cpu : gpu;
        int t = ((int)pk / 2) * 2; if (t < 30) t = 30; if (t > 88) t = 88;

        /* Hysteresis: damp oscillation at curve boundaries */
        if (last_idx >= 0) {
            int diff = last_idx - t;
            if (diff > 0 && diff < 3) t = last_idx;  /* cooling: hold 3 steps */
            if (diff < 0 && diff > -2) t = last_idx; /* heating: hold 2 steps */
        }

        int idx = -1;
        for (size_t i = 0; i < CLEN; i++) { if (cv_t[i] == t) { idx = (int)i; break; } }
        if (idx < 0) { sleep(2); continue; }

        w32("F0Tg", cv_h[idx]); w32("F1Tg", cv_h[idx]);
        last_idx = t;

        char li[128]; snprintf(li, sizeof li, "%.0f | %.0f | %u", cpu, gpu, cv_s[idx]);
        if (lf) { fprintf(lf, "%s\n", li); fflush(lf); }
        fprintf(stdout, "%s\n", li); fflush(stdout);
        sleep(2);
    }
}
