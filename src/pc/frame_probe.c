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
#include <time.h>

bool pc_frame_probe_on;

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
