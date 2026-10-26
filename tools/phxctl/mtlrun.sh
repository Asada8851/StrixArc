# sourced by try5.sh / mtl.sh: mtlprobe with a time limit. A hang gets a stack sample (which call in the plugin or RADV
# it waits in) and the kext's view of the fences, then the process is killed instead of waiting for the watchdog.
# The plugin log goes to ~/ (files in /private/tmp are wiped by a reboot).
mtlprobe_limited() {
    local LIM=${MTL_LIMIT:-45} PL=~/nullarc_nvmtl.log SM=~/nullarc_mtlprobe_sample.txt
    rm -f "$PL" "$SM"
    env NVMTL_LOG=1 NVMTL_LOG_PATH="$PL" ~/gpu/metal/mtlprobe > ~/nullarc_mtlprobe.out 2>&1 &
    local pid=$! t=0
    while kill -0 $pid 2>/dev/null && [ $t -lt $LIM ]; do sleep 1; t=$((t+1)); done
    cat ~/nullarc_mtlprobe.out
    if kill -0 $pid 2>/dev/null; then
        echo "!! mtlprobe still running after ${LIM}s: sampling its stacks, then killing it"
        sample $pid 3 -file "$SM" >/dev/null 2>&1
        # the main thread's call chain, deepest frames last; enough to see where it waits
        awk '/Call graph:/{p=1} p&&/Thread_/{n++} p&&n==1' "$SM" | grep -E '[A-Za-z_]' | sed 's/^ *[+!:|]* *//' | cut -c1-150 | head -60
        echo "-- other threads waiting in the plugin / RADV:"
        grep -E 'nvmtl|NVMTL|radv|vk_|amdgpu|acd_|IOConnect|semaphore_wait|__psynch_cvwait|mach_msg' "$SM" | sed 's/^ *[+!:|]* *//' | cut -c1-150 | sort | uniq -c | sort -rn | head -25
        kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
        echo "(full sample: $SM)"
    else
        wait $pid; echo "mtlprobe rc=$?"
    fi
    echo "== plugin log ($PL), last 80 lines"; tail -80 "$PL" 2>/dev/null || echo "(empty)"
    echo "== kernel: PhxBringup / NMXAccel, last 2 min"
    log show --last 2m --style compact --predicate 'sender == "PhxBringup" OR sender == "PhxAccel" OR eventMessage CONTAINS "PhxBringup" OR eventMessage CONTAINS "phxn"' 2>/dev/null | grep -v 'cfg+0x' | tail -50
    ioreg -rc PhxBringup -d1 | grep -iE 'seq|fence|retired|emitted|hang|fault' | head
}
