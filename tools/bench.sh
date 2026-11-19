#!/usr/bin/env bash
# Every benchmark in bench/, in Quark (quark run --release: in memory, as nothing has to be written and allowed to run)
# and, where there is one, in C (gcc -O2): the milliseconds each reports for its own work (not compiling, not the
# process's start), the best of N runs, and their checksums compared.
#   bash tools/bench.sh [N]        default 3; QUARK: the quark to measure (default build/quark.exe); CC: default gcc
set -e
cd "$(dirname "$0")/.."
quark=${QUARK:-build/quark.exe}
cc=${CC:-gcc}
runs=${1:-3}
out=build/bench
mkdir -p "$out"
# a new executable may be held a moment by a virus scanner: tried again for a few seconds
ready() {
    for _ in $(seq 50); do
        "$1" > /dev/null 2>&1 && return 0
        sleep 0.2
    done
    "$1"
}
best() { # best EXE: the least "ms" of $runs runs, and the checksum line
    local least= sum=
    [ "$1" = "$quark" ] || ready "$1"
    for _ in $(seq "$runs"); do
        local o
        o=$("$@")
        sum=$(echo "$o" | grep '^checksum')
        local ms
        ms=$(echo "$o" | awk '/^ms/ {printf "%d", $2}')
        if [ -z "$least" ] || [ "$ms" -lt "$least" ]; then least=$ms; fi
    done
    echo "$least|$sum"
}
printf "%-12s %10s %10s %8s\n" benchmark quark-ms c-ms ratio
for f in bench/*.qk; do
    b=$(basename "$f" .qk)
    q=$(best "$quark" run --release "$f")
    c="|"
    if [ -f "bench/$b.c" ] && command -v "$cc" > /dev/null; then
        # Windows' application control may refuse a new executable: built again under another name, a few times
        for k in 1 2 3 4 5; do
            "$cc" -O2 -o "$out/$b-c$k.exe" "bench/$b.c" -lm
            if "$out/$b-c$k.exe" > /dev/null 2>&1; then
                c=$(best "$out/$b-c$k.exe")
                break
            fi
        done
    fi
    qms=${q%%|*}
    cms=${c%%|*}
    ratio=-
    if [ -n "$cms" ] && [ -n "$qms" ] && [ "$cms" -gt 0 ]; then ratio=$(awk "BEGIN {printf \"%.2f\", $qms / $cms}"); fi
    if [ -n "$cms" ] && [ "${q#*|}" != "${c#*|}" ]; then ratio="$ratio (checksums differ)"; fi
    printf "%-12s %10s %10s %8s\n" "$b" "$qms" "${cms:--}" "$ratio"
done
