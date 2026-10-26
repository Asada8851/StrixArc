#!/usr/bin/env python3
"""Ordered timeline of one phase of a trace_modeswitch.sh capture: DMUB commands (raw bytes), SMU and DC kprobes, AUX
transactions decoded into DPCD reads/writes (with the reply), and the register writes that matter for a mode switch,
by name (the surface, cursor, colour, VPG/GPIO/AUX data ports and DMCUB ring pointers are left out).

usage: dc_timeline.py <trace.txt (zstd -dc the capture first)> <phase> <dcn_3_5_0_offset.h> <mp_14_0_0_offset.h>
Used to work out the 880M's 60 <-> 120 Hz switch (2026-10-27).
"""
import re, sys, collections
DMU_BASES = [0x12, 0xC0, 0x34C0, 0x9000, 0x2403C00]
def names(header, bases):
    txt = open(header).read(); out = {}
    for m in re.finditer(r'#define (reg\w+)\s+0x([0-9a-fA-F]+)\s*\n#define \1_BASE_IDX\s+(\d)', txt):
        i = int(m.group(3))
        if i < len(bases): out[bases[i] + int(m.group(2), 16)] = m.group(1)[3:]
    return out
reg = names(sys.argv[3], DMU_BASES)
mp = names(sys.argv[4], [0x16000, 0x0243FC00, 0, 0, 0])   # MP1 bases (Phoenix-like)
for k, v in mp.items(): reg.setdefault(k, 'MP:' + v)
SKIP = re.compile(r'^(HUBPREQ0_DCSURF|HUBP0_DCSURF|HUBPREQ0_VMID|CURSOR|CNVC|CM0|MPCC|MPC_|DSCL|DPP_TOP|VPG|DMCUB_|DC_GPIO|DP_AUX|OPP_PIPE|HUBPRET)')
want = sys.argv[2]
on = False; aux = None; out = []
def emit(ts, s): out.append('%s %s' % (ts, s))
def aux_done(ts):
    global aux
    if not aux: return
    req = aux['req']; rep = aux['rep']
    if len(req) >= 4:
        cmd = req[0] >> 4; addr = ((req[0] & 0xf) << 16) | (req[1] << 8) | req[2]; ln = req[3] + 1
        kind = {8: 'DPCD-W', 9: 'DPCD-R', 0: 'I2C-W', 1: 'I2C-R', 4: 'I2C-W-MOT', 5: 'I2C-R-MOT'}.get(cmd, 'cmd%x' % cmd)
        data = req[4:] if cmd in (8, 0, 4) else rep[1:]
        if cmd == 8: data = req[4:4 + ln]
        if not kind.startswith('I2C'):
            emit(ts, 'AUX %s 0x%05x len %d: %s  (reply %s)' % (kind, addr, ln, ' '.join('%02x' % x for x in data[:16]), '%02x' % rep[0] if rep else '-'))
    elif req:
        emit(ts, 'AUX short req %s' % req)
    aux = None
for line in open(sys.argv[1]):
    if 'nullarc phase' in line:
        p = line.split('nullarc phase ')[1].split(':')[0].strip()
        if p == want: on = True; continue
        if on: break
    if not on: continue
    ts = re.search(r'\s(\d+\.\d+):', line); ts = ts.group(1) if ts else ''
    if 'amdgpu_device_wreg:' in line or 'amdgpu_device_rreg:' in line:
        rw = 'W' if 'wreg' in line else 'R'
        f = line.split(':')[-1].split(','); a, v = int(f[1], 16), int(f[2], 16)
        n = reg.get(a, '?%05x' % a)
        if n == 'DP_AUX0_AUX_SW_CONTROL' and rw == 'W':
            if v & 1:
                if aux is not None: aux['go'] = True
            else:
                aux_done(ts); aux = {'req': [], 'rep': [], 'go': False, 'rd': False}
            continue
        if n == 'DP_AUX0_AUX_SW_DATA' and aux is not None:
            if rw == 'W':
                if not aux['go'] and not (v & 1): aux['req'].append((v >> 8) & 0xff)
                if aux['go'] and (v & 1): aux['rd'] = True
            elif aux['rd']:
                aux['rep'].append((v >> 8) & 0xff)
            continue
        if n.startswith('DP_AUX0') or n.startswith('DC_GPIO'):
            continue
        if rw == 'W' and not SKIP.match(n):
            emit(ts, 'W %-44s %05x %08x' % (n, a, v))
    elif 'dmub_cmds:' in line:
        words = [int(x, 16) for x in re.findall(r'c\d=0x([0-9a-f]+)', line)]
        b = b''.join(w.to_bytes(8, 'little') for w in words)
        if b[0] in (69, 72, 67, 68): continue      # HW_LOCK, IDLE_OPT, dirty rect, cursor
        emit(ts, 'DMUB t%d s%d: %s' % (b[0], b[1], b[:24].hex()))
    elif any(k in line for k in ('smu_msg:', 'set_dispclk:', 'set_dppclk:', 'link_train:', 'commit_streams:', 'link_dpms_on:')):
        emit(ts, 'KPROBE ' + re.sub(r'^.*?:\s+\S+:\s+\(([^+]+)\+\S+\)', r'\1', line.strip()))
aux_done('')
# collapse identical consecutive lines
prev = None; n = 0
for l in out:
    k = l.split(' ', 1)[1]
    if k == prev: n += 1; continue
    if n > 1: print('      (x%d)' % n)
    print(l); prev = k; n = 1
