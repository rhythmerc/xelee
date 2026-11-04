/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Where a slow game frame's time goes (frame_probe.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* dladdr */
#endif
#include "pc/frame_probe.h"

#include "pc/pc.h"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool pc_frame_probe_on;

/* Netplay's running totals (net_snapshot.c), for what each frame spent on
 * rollback: frames are grouped by the ticks they re-ran, and every 10 s a
 * line gives each group's count, sim time and snapshot cost. */
extern unsigned long long pc_snap_takes_total, pc_snap_restores_total, pc_snap_take_ns_total,
    pc_snap_restore_ns_total, pc_net_resim_ticks_total;
enum { RB_GROUPS = 5 }; /* re-ran 0, 1-2, 3-4, 5-6, 7+ ticks */
static struct {
    unsigned long long takes, restores, take_ns, restore_ns, resims; /* at the frame's start */
    unsigned n[RB_GROUPS], over6[RB_GROUPS], over9[RB_GROUPS];
    double sim[RB_GROUPS], snap[RB_GROUPS], takes_n[RB_GROUPS];
    unsigned long long windowStart;
} R;

static int rb_group(unsigned long long resims)
{
    return resims == 0 ? 0 : resims <= 2 ? 1 : resims <= 4 ? 2 : resims <= 6 ? 3 : 4;
}

typedef struct Slowest {
    const void* fn;
    int link;
    unsigned long long ns;
} Slowest;

static struct {
    bool init;
    unsigned long long thresholdNs;
    unsigned long long start, ready, simEnd, audioNs;
    int ticks;
    Slowest proc, render;
    unsigned long long procNs, renderNs; /* all of them, this frame */
    int logged;                          /* this window's lines */
    unsigned long long windowStart;
    /* Totals over the second so far, for the once-a-second split: the
     * boundary is from one frame's drawn to the next one's begin (Aurora's
     * end and begin frame, the pacing). */
    unsigned long long lastDrawn, sumStart, sumPad, sumSim, sumDraw, sumBoundary;
    int sumFrames;
} P;

unsigned long long pc_frame_probe_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000000000ull + (unsigned long long) ts.tv_nsec;
}

void pc_frame_probe_begin(void)
{
    if (!P.init) {
        const char* on = getenv("MELEE_FRAME_PROBE");
        const char* ms = getenv("MELEE_FRAME_PROBE_MS");
        P.init = true;
        pc_frame_probe_on = on != NULL && on[0] == '1';
        P.thresholdNs = (unsigned long long) ((ms != NULL ? atof(ms) : 11.0) * 1e6);
        if (pc_frame_probe_on) {
            pc_log_line("frame probe: on, frames over %.1f ms logged", P.thresholdNs / 1e6);
        }
    }
    if (!pc_frame_probe_on) {
        return;
    }
    P.start = pc_frame_probe_now();
    R.takes = pc_snap_takes_total;
    R.restores = pc_snap_restores_total;
    R.take_ns = pc_snap_take_ns_total;
    R.restore_ns = pc_snap_restore_ns_total;
    R.resims = pc_net_resim_ticks_total;
    if (P.lastDrawn != 0) {
        P.sumBoundary += P.start - P.lastDrawn;
    }
    P.ready = P.simEnd = P.audioNs = 0;
    P.ticks = 0;
    P.proc = (Slowest) { 0 };
    P.render = (Slowest) { 0 };
    P.procNs = P.renderNs = 0;
}

void pc_frame_probe_ready(void)
{
    if (pc_frame_probe_on) {
        P.ready = pc_frame_probe_now();
    }
}

void pc_frame_probe_audio(unsigned long long start)
{
    P.audioNs += pc_frame_probe_now() - start;
}

void pc_frame_probe_sim_end(int ticks)
{
    if (pc_frame_probe_on) {
        P.simEnd = pc_frame_probe_now();
        P.ticks = ticks;
    }
}

static void note(Slowest* s, unsigned long long* total, const void* fn, int link, unsigned long long start)
{
    const unsigned long long ns = pc_frame_probe_now() - start;
    *total += ns;
    if (ns > s->ns) {
        s->fn = fn;
        s->link = link;
        s->ns = ns;
    }
}

void pc_frame_probe_proc(const void* fn, int link, unsigned long long start)
{
    note(&P.proc, &P.procNs, fn, link, start);
}

void pc_frame_probe_render(const void* fn, int link, unsigned long long start)
{
    note(&P.render, &P.renderNs, fn, link, start);
}

/* `fn` as its library's offset, for addr2line. */
static void where(const void* fn, char* out, size_t size)
{
#if !defined(_WIN32)
    Dl_info info;
    if (fn != NULL && dladdr(fn, &info) != 0 && info.dli_fbase != NULL) {
        const char* lib = info.dli_fname != NULL ? info.dli_fname : "?";
        const char* slash = lib;
        for (const char* c = lib; *c != '\0'; c++) {
            if (*c == '/') {
                slash = c + 1;
            }
        }
        snprintf(out, size, "%s+0x%lx%s%s", slash, (unsigned long) ((uintptr_t) fn - (uintptr_t) info.dli_fbase),
                 info.dli_sname != NULL ? " " : "", info.dli_sname != NULL ? info.dli_sname : "");
        return;
    }
#endif
    snprintf(out, size, "%p", fn);
}

void pc_frame_probe_drawn(void)
{
    if (!pc_frame_probe_on || P.start == 0) {
        return;
    }
    const unsigned long long now = pc_frame_probe_now();
    {
        const unsigned long long rdy = P.ready != 0 ? P.ready : P.start;
        const unsigned long long end = P.simEnd != 0 ? P.simEnd : now;
        P.sumPad += rdy - P.start;
        P.sumSim += end - rdy;
        P.sumDraw += now - end;
        P.sumFrames++;
        P.lastDrawn = now;
        if (P.sumStart == 0) {
            P.sumStart = now;
        } else if (now - P.sumStart >= 1000000000ull) {
            pc_log_line("frame probe: second of %d frames: pad wait %.0f ms, sim %.0f, draw %.0f, boundary %.0f",
                        P.sumFrames, P.sumPad / 1e6, P.sumSim / 1e6, P.sumDraw / 1e6, P.sumBoundary / 1e6);
            P.sumStart = now;
            P.sumPad = P.sumSim = P.sumDraw = P.sumBoundary = 0;
            P.sumFrames = 0;
        }
    }
    {
        const unsigned long long rdy = P.ready != 0 ? P.ready : P.start;
        const unsigned long long end = P.simEnd != 0 ? P.simEnd : now;
        const unsigned long long resims = pc_net_resim_ticks_total - R.resims;
        const int g = rb_group(resims);
        const double sim = (end - rdy) / 1e6;
        R.n[g]++;
        R.sim[g] += sim;
        R.snap[g] += (pc_snap_take_ns_total - R.take_ns + pc_snap_restore_ns_total - R.restore_ns) / 1e6;
        R.takes_n[g] += (double) (pc_snap_takes_total - R.takes);
        R.over6[g] += sim > 6.0;
        R.over9[g] += sim > 9.0;
        if (R.windowStart == 0) {
            R.windowStart = now;
        } else if (now - R.windowStart >= 10000000000ull) {
            static const char* names[RB_GROUPS] = { "0", "1-2", "3-4", "5-6", "7+" };
            char line[600];
            int o = 0;
            for (int k = 0; k < RB_GROUPS; k++) {
                if (R.n[k] == 0) {
                    continue;
                }
                o += snprintf(line + o, sizeof line - o, " | re-ran %s: %u frames, sim %.1f ms, snapshots %.1f ms (%.1f takes), over 6 ms %u, over 9 ms %u",
                              names[k], R.n[k], R.sim[k] / R.n[k], R.snap[k] / R.n[k], R.takes_n[k] / R.n[k], R.over6[k], R.over9[k]);
            }
            pc_log_line("frame probe: rollback frames (10 s)%s", line);
            memset(&R.n, 0, sizeof R.n);
            memset(&R.over6, 0, sizeof R.over6);
            memset(&R.over9, 0, sizeof R.over9);
            memset(&R.sim, 0, sizeof R.sim);
            memset(&R.snap, 0, sizeof R.snap);
            memset(&R.takes_n, 0, sizeof R.takes_n);
            R.windowStart = now;
        }
    }
    if (now - P.windowStart > 10000000000ull) {
        P.windowStart = now;
        P.logged = 0;
    }
    if (now - P.start < P.thresholdNs || P.logged >= 12) {
        return;
    }
    ++P.logged;
    char proc[160], render[160];
    where(P.proc.fn, proc, sizeof proc);
    where(P.render.fn, render, sizeof render);
    const unsigned long long ready = P.ready != 0 ? P.ready : P.start;
    const unsigned long long simEnd = P.simEnd != 0 ? P.simEnd : now;
    pc_log_line("frame probe: %.1f ms (pad wait %.1f; sim %.1f in %d ticks: updates %.1f, audio %.1f; draw %.1f, "
                "renders %.1f); slowest update %.1f ms %s (p_link %d); slowest render %.1f ms %s (link %d)",
                (now - P.start) / 1e6, (ready - P.start) / 1e6, (simEnd - ready) / 1e6, P.ticks, P.procNs / 1e6,
                P.audioNs / 1e6, (now - simEnd) / 1e6, P.renderNs / 1e6, P.proc.ns / 1e6, proc, P.proc.link,
                P.render.ns / 1e6, render, P.render.link);
}
