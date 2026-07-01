/* m5logd -- M5 Pro telemetry logger
 * Binary delta log: CPU cluster util, memory, thermal.
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

static uint64_t base_us = 0, last_us = 0;
static FILE *lfp = NULL;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int log_open(void) {
    const char *h = getenv("HOME"); if (!h) h = "/Users/jwalinshah";
    char p[256]; snprintf(p, sizeof p, "%s/Library/Logs/m5logd.m5l", h);
    lfp = fopen(p, "wb"); if (!lfp) return -1;
    uint8_t hdr[32] = {0}; memcpy(hdr, "M5LOGv1", 8);
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

    /* memory pressure: 0=low 1=mod 2=high 3=critical */
    uint64_t fp = 0; size_t sz = sizeof fp;
    sysctlbyname("vm.page_free_count", &fp, &sz, NULL, 0);
    *pr = fp > 50000 ? 0 : fp > 10000 ? 1 : fp > 5000 ? 2 : 3;

    /* Also get swap usage */
    struct xsw_usage swap;
    sz = sizeof swap;
    if (sysctlbyname("vm.swapusage", &swap, &sz, NULL, 0) == 0) {
        *u += swap.xsu_used / 1048576.0f;  /* include swap in "used" */
    }
    return 0;
}

/* ── Thermal pressure via sysctl ────────────────────────── */
static int therm_read(int *level) {
    int v = 0; size_t sz = sizeof v;
    if (sysctlbyname("kern.temperature.thermal_pressure", &v, &sz, NULL, 0) < 0) return -1;
    *level = v;
    return 0;
}

/* ── Main loop ──────────────────────────────────────────── */
int main(void) {
    if (log_open() < 0) return 1;
    fprintf(stderr, "m5logd: logging ~/Library/Logs/m5logd.m5l\n");

    float pp = -1, ep = -1, pu = -1, pw = -1;
    int ppr = -1, ptl = -1;

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

        sleep(2);
    }
}
