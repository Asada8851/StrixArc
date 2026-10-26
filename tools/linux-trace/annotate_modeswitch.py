#!/usr/bin/env python3
"""Reads a trace_modeswitch.sh capture (~/strix_modeswitch_trace.txt.zst) and prints, per phase: the clock and link
kprobes (SMU messages, DISPCLK/DPPCLK, link training), the DMUB commands DC queued (decoded: VBIOS transmitter control
with its link rate, LVTMA panel power, clock-manager notifications) and, between two phases, the DCN registers whose
last written value differs, by name (dcn_3_5_0_offset.h with the 880M's DMU bases from IP discovery).

usage: annotate_modeswitch.py <trace.txt[.zst]> <dcn_3_5_0_offset.h> [phaseA phaseB]   (default: 1 3, native vs scaled)
"""
import collections
import re
import subprocess
import sys

DMU_BASES = [0x12, 0xC0, 0x34C0, 0x9000, 0x2403C00]   # Strix Point DMU 3.5.0, IP discovery of the 880M
TX_ACTION = {0: 'DISABLE', 1: 'ENABLE', 2: 'LCD_BLOFF', 3: 'LCD_BLON', 7: 'INIT', 8: 'DISABLE_OUTPUT',
             9: 'ENABLE_OUTPUT', 10: 'SETUP', 11: 'SETUP_VSEMPH', 12: 'POWER_ON', 13: 'POWER_OFF'}
DMUB_TYPE = {64: 'PSR', 66: 'ABM', 67: 'UPDATE_DIRTY_RECT', 68: 'UPDATE_CURSOR_INFO', 69: 'HW_LOCK', 72: 'IDLE_OPT',
             73: 'CLK_MGR', 74: 'PANEL_CNTL', 128: 'VBIOS'}
KPROBES = ('smu_msg:', 'set_dispclk:', 'set_dppclk:', 'link_train:', 'commit_streams:', 'link_dpms_on:')


def lines(path):
    if path.endswith('.zst'):
        out = subprocess.run(['zstd', '-dc', path], check=True, capture_output=True, text=True).stdout
        return out.splitlines()
    return open(path).read().splitlines()


def names(header):
    txt = open(header).read()
    out = {}
    for m in re.finditer(r'#define (reg\w+)\s+0x([0-9a-fA-F]+)\s*\n#define \1_BASE_IDX\s+(\d)', txt):
        out[DMU_BASES[int(m.group(3))] + int(m.group(2), 16)] = m.group(1)[3:]
    return out


def dmub(line):
    words = [int(x, 16) for x in re.findall(r'c\d=0x([0-9a-f]+)', line)]
    b = b''.join(w.to_bytes(8, 'little') for w in words)
    t, st = b[0], b[1]
    if t == 128 and st == 1:
        sym = int.from_bytes(b[8:12], 'little') * 10
        return 'VBIOS TRANSMITTER phy=%d %s laneset=0x%02x lanes=%d symclk=%d kHz' % (
            b[4], TX_ACTION.get(b[5], b[5]), b[6], b[7], sym)
    if t == 128 and st == 15:
        return 'VBIOS LVTMA_CONTROL action=%d' % b[4]
    return '%s sub %d' % (DMUB_TYPE.get(t, t), st)


def main():
    trace, header = sys.argv[1], sys.argv[2]
    pa, pb = (sys.argv[3], sys.argv[4]) if len(sys.argv) > 4 else ('1', '3')
    reg = names(header)
    phase = None
    last = collections.defaultdict(dict)
    order = collections.defaultdict(list)
    for line in lines(trace):
        if 'nullarc phase' in line:
            phase = line.split('nullarc phase ')[1].strip()
            print('== phase', phase)
            phase = phase.split(':')[0]
            continue
        if phase is None:
            continue
        if any(k in line for k in KPROBES):
            print('   ', re.sub(r'^.*?\]\s+\S+\s+', '', line).replace(' [amdgpu]', ''))
        elif 'dmub_cmds:' in line:
            d = dmub(line)
            if not d.endswith(('HW_LOCK sub 0', 'IDLE_OPT sub 0', 'IDLE_OPT sub 2', 'UPDATE_DIRTY_RECT sub 0',
                               'UPDATE_CURSOR_INFO sub 0')):
                print('    dmub:', d)
        elif 'amdgpu_device_wreg:' in line:
            f = line.split('amdgpu_device_wreg:')[1].split(',')
            a, v = int(f[1], 16), int(f[2], 16)
            if a not in last[phase]:
                order[phase].append(a)
            last[phase][a] = v
    A, B = last[pa], last[pb]
    print('== registers whose last write differs, phase %s -> %s (%d / %d registers written)' % (pa, pb, len(A), len(B)))
    for a in order[pb]:
        if a in A and A[a] == B[a]:
            continue
        print('    %-50s 0x%05x  %s -> 0x%08x' % (reg.get(a, '?'), a, ('0x%08x' % A[a]) if a in A else '     -    ', B[a]))


if __name__ == '__main__':
    main()
