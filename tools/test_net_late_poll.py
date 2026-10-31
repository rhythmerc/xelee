#!/usr/bin/env python3
"""A fresh tick polls current hardware; replay/advance reuse the sent sample."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
s = (ROOT / 'src/pc/net.c').read_text()
a = s.index('static void capture_local_sample(')
b = s.index('/* A fresh frame:', a)
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define PORTS 4
typedef struct { int button; } PADStatus;
static struct { unsigned local_mask; } net;
static PADStatus s_raw_last[PORTS];
static PADStatus s_phys_last[PORTS];
static int s_local_phys[PORTS];
static uint8_t s_hold_mask, s_hold_rest;
static int polls, steps;
static void pc_keyboard_sim_frame(void) { ++steps; }
static void PADRead(PADStatus* pads) {
    ++polls;
    for (int i=0;i<4;++i) pads[i].button=100+i;
}
static void hold_neutral(int q, PADStatus* pad) { (void)q; pad->button = 0; }
''' + s[a:b] + r'''
int main(void) {
    /* One seat on this machine: port 1 (index 0) reads pad 0. */
    net.local_mask = 1;
    s_raw_last[0].button=7;
    capture_local_sample(false);
    assert(polls==0 && steps==0 && s_raw_last[0].button==7);
    capture_local_sample(true);
    assert(polls==1 && steps==1 && s_raw_last[0].button==100);
    capture_local_sample(false);
    assert(polls==1 && s_raw_last[0].button==100);
    /* Two seats (lan multipad): port 3 plays from pad 1, and a held seat
     * reads neutral; ports not on this machine are left alone. */
    net.local_mask = 1u | 1u << 2;
    s_local_phys[2] = 1;
    s_raw_last[1].button = 9;
    s_hold_mask = 1u << 2;
    capture_local_sample(true);
    assert(polls==2 && s_raw_last[0].button==100 && s_raw_last[2].button==0 && s_raw_last[1].button==9);
    s_hold_mask = 0;
    capture_local_sample(true);
    assert(s_raw_last[2].button==101 && s_phys_last[3].button==103);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.c'; p.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Werror',str(p),'-o',d+'/test'],check=True)
    subprocess.run([d+'/test'],check=True)
print('PASS: late fresh input and preserved replay/advance sample')
