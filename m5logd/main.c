/* m5logd -- M5 Pro telemetry logger
 * Binary delta log: CPU cluster util, memory, thermal, temps, fans.
 * Only records deltas. ~100 records/day at idle vs 43k for text logs.
 * Reader: m5log (separate tool).
 *
 * Build: clang -std=c99 -Wall -Wextra -O2 -o m5logd main.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>

#define REC_CPU   1
#define REC_MEM   2
#define REC_THERM 3
#define REC_TEMP  4   /* CPU/GPU die temp (°C), fan RPM — two uint16_t each */
#define REC_FAN   5

static uint64_t base_us = 0, last_us = 0;
static FILE *lfp = NULL;

/* ── SMC path: runtime detection ────────────────────────── */
static char smc_buf[256];
static const char *smc_path(void) {
    if (smc_buf[0] != '\0') return smc_buf;
    const char *home = getenv("HOME");
    if (home) {
        snprintf(smc_buf, sizeof smc_buf, "%s/.local/bin/smc", home);
        if (access(smc_buf, X_OK) == 0) return smc_buf;
    }
    FILE *fp = popen("which smc 2>/dev/null", "r");
    if (fp) {
        if (fgets(smc_buf, sizeof smc_buf, fp)) {
            size_t len = strlen(smc_buf);
            if (len > 0 && smc_buf[len - 1] == '\n') smc_buf[len - 1] = '\0';
            pclose(fp);
            if (smc_buf[0] != '\0' && access(smc_buf, X_OK) == 0) return smc_buf;
        } else { pclose(fp); }
    }
    snprintf(smc_buf, sizeof smc_buf, "/usr/local/bin/smc");
    return smc_buf;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int log_open(void) {
    const char *h = getenv("HOME");
    if (!h) {
        fprintf(stderr, "m5logd: HOME not set, cannot determine log path\n");
        return -1;
    }
    char p[256]; snprintf(p, sizeof p, "%s/Library/Logs/m5logd.m5l", h);
    lfp = fopen(p, "wb"); if (!lfp) return -1;
    uint8_t hdr[32] = {0}; memcpy(hdr, "M5LOGv2", 8);
    uint64_t bt = 0; size_t sz = sizeof(bt);
    sysctlbyname("kern.boottime", &bt, &sz, NULL, 0);
    memcpy(hdr+8, &bt, 8);
    fwrite(hdr, 32, 1, lfp);
    base_us = now_us(); last_us = base_us;
    return 0;
}

static void wr(uint8_t t, const void *d, uint16_t l) {
    if (!lfp) return;
    uint64_t n = now_us(), dt = n - last_us; last_us = n;
    uint8_t h[10]; memcpy(h, &dt, 4); h[4] = t; memcpy(h+5, &l, 2);
    fwrite(h, 7, 1, lfp);
    if (d && l) fwrite(d, l, 1, lfp);
    fflush(lfp);
}

/* ── CPU cluster util via sysctl ─────────────────────────── */
static int cpu_read(float *p, float *e) {
    int v = 0; size_t sz = sizeof v;
    if (sysctlbyname("hw.perflevel0.cpu_usage", &v, &sz, NULL, 0) == 0) *p = (float)v;
    sz = sizeof v;
    if (sysctlbyname("hw.perflevel1.cpu_usage", &v, &sz, NULL, 0) == 0) *e = (float)v;
    return 0;
}

/* ── Memory via mach VM stats ───────────────────────────── */
static int mem_read(float *u, float *w, int *pr) {
    vm_statistics64_data_t vm;
    mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_task_self(), HOST_VM_INFO64, (host_info_t)&vm, &c) != KERN_SUCCESS)
        return -1;
    uint64_t pg = vm_page_size;
    *u = (vm.active_count + vm.wire_count) * pg / 1048576.0f;
    *w = vm.wire_count * pg / 1048576.0f;
    uint64_t fp = 0; size_t sz = sizeof fp;
    sysctlbyname("vm.page_free_count", &fp, &sz, NULL, 0);
    *pr = fp > 50000 ? 0 : fp > 10000 ? 1 : fp > 5000 ? 2 : 3;
    struct xsw_usage swap;
    sz = sizeof swap;
    if (sysctlbyname("vm.swapusage", &swap, &sz, NULL, 0) == 0)
        *u += swap.xsu_used / 1048576.0f;
    return 0;
}

/* ── Thermal pressure via sysctl ────────────────────────── */
static int therm_read(int *level) {
    int v = 0; size_t sz = sizeof v;
    if (sysctlbyname("kern.temperature.thermal_pressure", &v, &sz, NULL, 0) < 0) return -1;
    *level = v;
    return 0;
}

/* ── Temps via smc: max of sensor prefix ────────────────── */
static float smc_max_temp(const char *pfx) {
    char cmd[320]; int r = snprintf(cmd, sizeof cmd, "%s -l 2>/dev/null", smc_path());
    if (r < 0 || (size_t)r >= sizeof cmd) return -1;
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char line[128]; float mx = -1; size_t pl = strlen(pfx);
    while (fgets(line, sizeof line, fp)) {
        char *bracket = strrchr(line, ']');
        if (!bracket) continue;
        char *end = NULL;
        float v = strtof(bracket + 1, &end);
        if (end == bracket + 1) continue;
        if (strlen(line) < 6) continue;
        char k[5] = {line[2], line[3], line[4], line[5], 0};
        if (strncmp(k, pfx, pl) == 0 && v > mx) mx = v;
    }
    pclose(fp); return mx;
}

/* ── Fan RPM via smc -f ─────────────────────────────────── */
static int smc_read_fans(uint16_t *f0, uint16_t *f1) {
    char cmd[320]; int r = snprintf(cmd, sizeof cmd, "%s -f 2>/dev/null", smc_path());
    if (r < 0 || (size_t)r >= sizeof cmd) return -1;
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char line[128]; int found = 0;
    *f0 = 0; *f1 = 0;
    while (fgets(line, sizeof line, fp)) {
        if (strstr(line, "Fan #0")) {
            if (fgets(line, sizeof line, fp) && strstr(line, "Current speed"))
                *f0 = (uint16_t)atoi(strrchr(line, ':') ? strrchr(line, ':') + 2 : "0");
            found++;
        }
        if (strstr(line, "Fan #1")) {
            if (fgets(line, sizeof line, fp) && strstr(line, "Current speed"))
                *f1 = (uint16_t)atoi(strrchr(line, ':') ? strrchr(line, ':') + 2 : "0");
            found++;
        }
    }
    pclose(fp); return found >= 2 ? 0 : -1;
}

/* ── Main loop ──────────────────────────────────────────── */
int main(void) {
    if (log_open() < 0) return 1;
    fprintf(stderr, "m5logd: logging ~/Library/Logs/m5logd.m5l\n");

    float pp = -1, ep = -1, pu = -1, pw = -1;
    int ppr = -1, ptl = -1;
    uint16_t pcpu = 0, pgpu = 0, pf0 = 0, pf1 = 0;

    while (1) {
        /* CPU */
        float p = 0, e = 0;
        cpu_read(&p, &e);
        if (p != pp || e != ep) {
            float buf[2] = {p, e};
            wr(REC_CPU, buf, 8);
            pp = p; ep = e;
        }

        /* Memory */
        float u = 0, w = 0; int pr = 0;
        mem_read(&u, &w, &pr);
        if (fabsf(u - pu) > 50 || fabsf(w - pw) > 50 || pr != ppr) {
            uint8_t buf[12];
            memcpy(buf, &u, 4); memcpy(buf+4, &w, 4); buf[8] = (uint8_t)pr; memset(buf+9, 0, 3);
            wr(REC_MEM, buf, 12);
            pu = u; pw = w; ppr = pr;
        }

        /* Thermal */
        int tl = 0;
        if (therm_read(&tl) == 0 && tl != ptl) {
            uint8_t v = (uint8_t)tl;
            wr(REC_THERM, &v, 1);
            ptl = tl;
        }

        /* Temps: CPU/GPU die temp from smc */
        float cpu_t = smc_max_temp("Tp");
        float gpu_t = smc_max_temp("Tg");
        if (cpu_t >= 0 && gpu_t >= 0) {
            uint16_t ct = (uint16_t)cpu_t, gt = (uint16_t)gpu_t;
            if (abs((int)ct - (int)pcpu) >= 2 || abs((int)gt - (int)pgpu) >= 2) {
                uint16_t buf[2] = {ct, gt};
                wr(REC_TEMP, buf, 4);
                pcpu = ct; pgpu = gt;
            }
        }

        /* Fans: RPM */
        uint16_t f0 = 0, f1 = 0;
        if (smc_read_fans(&f0, &f1) == 0) {
            if (abs((int)f0 - (int)pf0) >= 100 || abs((int)f1 - (int)pf1) >= 100) {
                uint16_t buf[2] = {f0, f1};
                wr(REC_FAN, buf, 4);
                pf0 = f0; pf1 = f1;
            }
        }

        sleep(2);
    }
}
