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

/* ── SMC path: runtime detection, not compile-time ──────────── */
static char smc_buf[256];
static const char *smc_path(void) {
    if (smc_buf[0] != '\0') return smc_buf;
    /* Prefer $HOME/.local/bin/smc */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(smc_buf, sizeof smc_buf, "%s/.local/bin/smc", home);
        if (access(smc_buf, X_OK) == 0) return smc_buf;
    }
    /* Fall back to which(1) */
    FILE *fp = popen("which smc 2>/dev/null", "r");
    if (fp) {
        if (fgets(smc_buf, sizeof smc_buf, fp)) {
            size_t len = strlen(smc_buf);
            if (len > 0 && smc_buf[len - 1] == '\n')
                smc_buf[len - 1] = '\0';
            pclose(fp);
            if (smc_buf[0] != '\0' && access(smc_buf, X_OK) == 0) return smc_buf;
        } else {
            pclose(fp);
        }
    }
    /* Last resort */
    snprintf(smc_buf, sizeof smc_buf, "/usr/local/bin/smc");
    if (access(smc_buf, X_OK) == 0) return smc_buf;

    /* FAIL CLOSED: no smc binary means we can't read temps or control fans.
     * Running blind at minimum RPM will cook the machine. Exit with a loud
     * error so launchd restarts us. macOS falls back to its own fan management
     * when m5fand is not controlling the fans. */
    fprintf(stderr, "m5fand: FATAL — smc binary not found at any expected path. "
                    "Cannot read temperatures or control fans. Exiting.\n");
    exit(1);
}

/* ── Read max temp for sensor prefix from one smc -l call ── */
static float max_temp(const char *pfx) {
    char cmd[320];
    snprintf(cmd, sizeof cmd, "%s -l 2>/dev/null", smc_path());
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    char line[128]; float mx = 0; size_t pl = strlen(pfx);
    while (fgets(line, sizeof line, fp)) {
        char *bracket = strrchr(line, ']');
        if (!bracket) continue;
        char *end = NULL;
        float v = strtof(bracket + 1, &end);
        if (end == bracket + 1) continue;
        /* guard: ensure line is at least 6 chars before indexing 2..5 */
        if (strlen(line) < 6) continue;
        char k[5] = {line[2], line[3], line[4], line[5], 0};
        if (strncmp(k, pfx, pl) == 0 && v > mx) mx = v;
    }
    pclose(fp); return mx;
}

static void w32(const char *k, uint32_t v) {
    char cmd[320], h[9];
    snprintf(h, sizeof h, "%08x", v);
    snprintf(cmd, sizeof cmd, "%s -k %s -w %s 2>/dev/null", smc_path(), k, h);
    FILE *fp = popen(cmd, "r"); if (fp) pclose(fp);
}
static void w1(const char *k, uint8_t v) {
    char cmd[320];
    snprintf(cmd, sizeof cmd, "%s -k %s -w %02x 2>/dev/null", smc_path(), k, v);
    FILE *fp = popen(cmd, "r"); if (fp) pclose(fp);
}

/* ── Fan curve table (2-degree steps, sigmoid) ──────────── */
static const int cv_t[]  = {30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80,82,84,86,88};
static const uint32_t cv_h[] = {0x00c0ac44,0x0020ae44,0x0020b044,0x00a0b244,0x0000b644,0x0080ba44,0x0060c044,0x0020c844,0x0040d244,0x0040df44,0x00401c45,0x00803b45,0x00006145,0x00408345,0x00e09245,0x00409c45,0x00f0a045,0x0010a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445,0x00a8a445};
static const uint16_t cv_s[] = {1382,1393,1409,1429,1456,1492,1539,1601,1682,1786,2500,3000,3600,4200,4700,5000,5150,5250,5269,5269,5269,5269,5269,5269,5269,5269,5269,5269,5269,5269};
#define CLEN (sizeof cv_t / sizeof cv_t[0])

/* ── Main ──────────────────────────────────────────────── */
int main(void) {
    const char *hd = getenv("HOME");
    if (!hd) {
        fprintf(stderr, "m5fand: HOME not set, cannot determine log path\n");
        return 1;
    }
    char lp[256]; snprintf(lp, sizeof lp, "%s/Library/Logs/m5fand.log", hd);
    FILE *lf = fopen(lp, "a"); if (lf) setlinebuf(lf);

    w1("F0md",1); w1("F1md",1);
    if (lf) fprintf(lf, "m5fand: SMC OK, fans forced.\n");
    fprintf(stdout, "m5fand: SMC OK, fans forced.\n");

    int last_temp = -1;
    while (1) {
        float cpu = max_temp("Tp");
        float gpu = max_temp("Tg");
        float pk = (cpu > gpu) ? cpu : gpu;
        int t = ((int)pk / 2) * 2;

        /* Failsafe: if smc returns zero for both, something is wrong with
         * the sensor reads — force fans to max safe speed and exit rather
         * than cook the machine at minimum RPM. */
        if (t <= 0) {
            if (lf) fprintf(lf, "m5fand: FATAL — zero temp read from smc, forcing max fans and exiting\n");
            fprintf(stderr, "m5fand: FATAL — zero temperature read from smc, forcing max fans and exiting\n");
            w32("F0Tg", cv_h[CLEN - 1]); w32("F1Tg", cv_h[CLEN - 1]);
            return 1;
        }

        if (t < 30) { t = 30; }
        if (t > 88) { t = 88; }

        /* Hysteresis: damp oscillation at curve boundaries */
        if (last_temp >= 0) {
            int diff = last_temp - t;
            if (diff > 0 && diff < 3) { t = last_temp; }  /* cooling: hold 3 steps */
            if (diff < 0 && diff > -2) { t = last_temp; } /* heating: hold 2 steps */
        }

        int idx = -1;
        for (size_t i = 0; i < CLEN; i++) {
            if (cv_t[i] == t) { idx = (int)i; break; }
        }
        if (idx < 0) { sleep(2); continue; }

        w32("F0Tg", cv_h[idx]); w32("F1Tg", cv_h[idx]);
        last_temp = t;

        char li[128]; snprintf(li, sizeof li, "%.0f | %.0f | %u", cpu, gpu, cv_s[idx]);
        if (lf) { fprintf(lf, "%s\n", li); fflush(lf); }
        fprintf(stdout, "%s\n", li); fflush(stdout);
        sleep(2);
    }
}
