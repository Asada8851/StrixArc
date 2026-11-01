#!/usr/bin/env python3
"""sample_tree.py: what a macOS `sample` report says a process spent its time on, for StrixArc rounds.

  sample_tree.py <sample report> [--sym <image>=<unstripped binary> ...] [--top N] [--thread SUBSTR] [--chain N]

The call graph's every node has a count; its own samples are the count minus its children's. Those are summed per
(leaf frame, nearest caller in "our" code) - our code being the images that are not the system's (ffplay, FFmpeg's
libraries, RADV, the Metal plugin) - so a kernel trap such as mach_msg2_trap shows whether RADV's kext call, the
plugin or the run loop made it. Waits (cvwait, semaphore waits, workq) are counted apart: they are idle, not CPU.
Stripped images show as image+offset; --sym maps an image's offsets to names with the unstripped build (llvm-nm).
"""
import bisect
import re
import subprocess
import sys
from collections import Counter, defaultdict

IDLE = ('__psynch_cvwait', 'semaphore_wait_trap', '__workq_kernreturn', '__semwait_signal', 'start_wqthread',
        '__select', 'kevent', '__psynch_mutexwait', 'thread_start')
LINE = re.compile(r'^(?P<indent>[ +!:|]*?)(?P<count>\d+) (?P<rest>.*)$')
FRAME = re.compile(r'^(?P<sym>.*?)\s+\(in (?P<img>[^)]+)\)(?:\s+(?:\+ \d+|load address 0x[0-9a-f]+ \+ (?P<off>0x[0-9a-f]+)))?')


def load_syms(path):
    out = subprocess.run(['llvm-nm', '-n', '--defined-only', path], capture_output=True, text=True).stdout
    addrs, names = [], []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) >= 3 and p[1] in 'tT':
            addrs.append(int(p[0], 16)); names.append(p[2].lstrip('_'))
    return addrs, names


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); sys.exit(1)
    report, syms, top, thread_filter, chain = args[0], {}, 40, None, 1
    i = 1
    while i < len(args):
        if args[i] == '--sym':
            img, path = args[i + 1].split('=', 1); syms[img] = load_syms(path); i += 2
        elif args[i] == '--top':
            top = int(args[i + 1]); i += 2
        elif args[i] == '--chain':
            chain = int(args[i + 1]); i += 2
        elif args[i] == '--thread':
            thread_filter = args[i + 1]; i += 2
        else:
            sys.exit('unknown argument ' + args[i])
    system = lambda img: img.startswith(('lib', 'com.apple', 'CoreFoundation', 'AppKit', 'Foundation', 'Metal',
                                          'QuartzCore', 'IOKit', 'HIToolbox', 'SkyLight', 'AudioToolbox', 'CoreAudio',
                                          'dyld')) and not img.startswith(('libav', 'libsw', 'libvulkan', 'libnvmtl'))

    def name(sym, img, off):
        if sym != '???' or not off:
            return f'{sym} ({img})'
        if img in syms:
            a, n = syms[img]
            k = bisect.bisect_right(a, int(off, 16)) - 1
            if k >= 0:
                return f'{n[k]} ({img})'
        return f'{img}+{off}'

    lines = open(report, errors='replace').read().split('\n')
    try:
        start = lines.index('Call graph:') + 1
    except ValueError:
        sys.exit('no call graph')
    stack, thread, per_thread = [], None, Counter()
    selfc, idle = Counter(), Counter()
    nodes = []   # (depth, count, frame, thread)
    for ln in lines[start:]:
        if ln.startswith('Total number in stack'):
            break
        m = LINE.match(ln)
        if not m:
            continue
        depth = len(m.group('indent'))
        rest = m.group('rest')
        if re.match(r'Thread_\d+', rest):
            thread = rest
            nodes.append((depth, int(m.group('count')), None, thread))
            continue
        f = FRAME.match(rest)
        if f:
            nodes.append((depth, int(m.group('count')), (f.group('sym'), f.group('img'), f.group('off')), thread))
    # self counts: a node's count minus its children's (a child: the next deeper node under it)
    kids = [0] * len(nodes)
    parents = []
    for k, (depth, count, frame, thr) in enumerate(nodes):
        while parents and nodes[parents[-1]][0] >= depth:
            parents.pop()
        if parents:
            kids[parents[-1]] += count
        parents.append(k)
    for k, (depth, count, frame, thr) in enumerate(nodes):
        while stack and stack[-1][0] >= depth:
            stack.pop()
        stack.append((depth, frame))
        own = count - kids[k]
        if frame is None or own <= 0 or (thread_filter and thread_filter not in (thr or '')):
            continue
        sym, img, off = frame
        leaf = name(sym, img, off)
        if any(sym.startswith(x) for x in IDLE):
            idle[leaf] += own; continue
        ours = [fr for _, fr in reversed(stack[:-1]) if fr and not system(fr[1])][:chain]
        caller = ' <- '.join(name(*fr) for fr in ours) if ours and system(img) else ''
        selfc[(leaf, caller)] += own
        per_thread[thr] += own
    total = sum(selfc.values())
    print(f'busy samples {total}, idle {sum(idle.values())} (1 sample = 1 ms of one thread)')
    print('-- busy per thread')
    for thr, c in per_thread.most_common(12):
        print(f'{c:8d}  {thr}')
    print('-- busy per leaf (nearest caller in our code)')
    for (leaf, caller), c in selfc.most_common(top):
        print(f'{c:8d}  {leaf}' + (f'   <- {caller}' if caller else ''))
    by_img = defaultdict(int)
    for (leaf, caller), c in selfc.items():
        key = caller.split(' <- ')[0].rsplit('(', 1)[-1].rstrip(')') if caller else leaf.rsplit('(', 1)[-1].rstrip(')').split('+')[0]
        by_img[key] += c
    print('-- busy per image of our code (system leaves charged to their caller)')
    for img, c in sorted(by_img.items(), key=lambda x: -x[1])[:15]:
        print(f'{c:8d}  {img}')


if __name__ == '__main__':
    main()
