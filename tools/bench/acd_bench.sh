#!/bin/bash
# Builds and runs acd_bench: Mesa's RADV Darwin backend (the tree with Cobalt64's and our patches) compiled for Linux
# against phxmock, with the compile flags of the tree's own Darwin build (build-darwin/compile_commands.json, from
# tools/cross/build-radv.sh) minus the macOS target bits.
#   tools/bench/acd_bench.sh <mesa tree> [out dir]
set -eu
M=$(cd "${1:?usage: acd_bench.sh <mesa tree> [out dir]}" && pwd)
R=$(cd "$(dirname "$0")/../.." && pwd)
O=${2:-$R/build/tests/acd}
[ -f "$M/build-darwin/compile_commands.json" ] || { echo "acd_bench: no $M/build-darwin (run tools/cross/build-radv.sh first)"; exit 1; }
mkdir -p "$O"
FLAGS=$(python3 -I - "$M" <<'PY'
import json, shlex, sys
M = sys.argv[1]
e = [x for x in json.load(open(M + '/build-darwin/compile_commands.json')) if x['file'].endswith('darwin/ac_darwin_drm.c')][0]
a, out, skip = shlex.split(e['command']), [], False
for t in a[1:]:
    if skip: skip = False; continue
    if t in ('-target', '-isysroot', '-o', '-MQ', '-MF', '-c'): skip = True; continue
    if t.startswith(('-D', '-I', '-std')):
        if t.startswith('-I') and not t.startswith('-I/'): t = '-I' + M + '/build-darwin/' + t[2:]
        out.append(t)
print(' '.join(shlex.quote(x) for x in out))
PY
)
INC="-I$M/src/amd/common/darwin -I$R/tools/bench/include -I$R/src/amd -I$R/src/phxbringup -D_GNU_SOURCE"
D=$M/src/amd/common/darwin
for f in "$D"/ac_darwin_drm.c "$D"/ac_darwin_kext.c "$D"/ac_darwin_iokit.c "$D"/ac_darwin_sync.c "$D"/ac_darwin_mock.c \
         "$D"/amdgpu_vamgr.c "$M"/src/amd/common/amdgpu_devices.c "$R"/tools/bench/acd_bench.c; do
    eval gcc $FLAGS $INC -w -c "$f" -o "$O/$(basename "${f%.c}").o"
done
gcc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -I"$R/tools/bench/include" -I"$R/src/phxbringup" -I"$R/src/amd" \
    -c "$R/tools/bench/phxmock.c" -o "$O/phxmock.o"
gcc "$O"/*.o -o "$O/acd_bench" -lpthread
PHXMOCK_FREE_DELAY_US=20 "$O/acd_bench"   # the delay opens the window a preempted BO free leaves (2-thread stress)
PHXMOCK_FREE_DELAY_US=20 PHXMOCK_NOVCN=1 "$O/acd_bench"
