#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Tests the benchmark itself: that compare.awk reaches the right verdict on
# made-up timings, and that la_bench refuses input it shouldn't accept.
#
# usage: test.sh [la_bench-binary]
# Without a binary, only compare.awk is tested.

set -u

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0

fail() {
    echo "FAIL: $1"
    failures=$((failures + 1))
}

# Writes a runs.tsv of $1 rounds.  A's wall time is 1000 plus a little noise;
# B's is A's times $2, and every other round is further off by +-$3 percent.
make_runs() {
    awk -v n="$1" -v factor="$2" -v spread="${3:-0}" 'BEGIN {
        OFS = "\t"
        print "pair", "build", "format", "workload", "op", "wall_ns",
            "cpu_ns", "max_rss", "entries", "bytes", "out_bytes"
        for (i = 1; i <= n; i++) {
            a = 1000 + (i % 3)
            b = a * factor * (1 + (i % 2 ? spread : -spread) / 100)
            print i, "A", "tar.gz", "w", "read", a, a, 100, 5, 50, 0
            print i, "B", "tar.gz", "w", "read", b, b, 100, 5, 50, 0
        }
    }'
}

# Checks the verdict on the wall line of a runs.tsv read from stdin.
expect_wall() {
    got=$(awk -f "$here/compare.awk" -v threshold=0.01 |
        awk '$4 == "wall" { print $NF }')
    [ "$got" = "$2" ] || fail "$1: expected $2, got '$got'"
}

make_runs 15 1.00 | expect_wall "identical builds" same
make_runs 15 1.05 | expect_wall "5% slower" worse
make_runs 15 0.95 | expect_wall "5% faster" better
make_runs 15 1.002 | expect_wall "under the threshold" same
make_runs 15 1.00 10 | expect_wall "noise far above the threshold" unclear
make_runs 15 1.03 10 | expect_wall "a change hidden in noise" unclear

make_runs 5 1.05 | awk -f "$here/compare.awk" |
    grep -q 'too few rounds' || fail "5 rounds should be too few"

# A build that skips work can't be allowed to look fast.
make_runs 15 1.00 | awk -F'\t' -v OFS='\t' \
    '$2 == "B" && $1 == 7 { $10 = 49 } { print }' >"$work/differs.tsv"
awk -f "$here/compare.awk" "$work/differs.tsv" >/dev/null 2>"$work/err"
[ $? -eq 2 ] || fail "different work should exit 2"
grep -q 'different work' "$work/err" || fail "different work should be reported"

printf 'pair\tbuild\n' >"$work/empty.tsv"
awk -f "$here/compare.awk" "$work/empty.tsv" >/dev/null 2>&1
[ $? -eq 2 ] || fail "no runs should exit 2"

if [ $# -ge 1 ]; then
    bench=$1
    w=small-text
    "$bench" gen tar.gz "$w" "$work/1" || fail "gen"
    "$bench" gen tar.gz "$w" "$work/2" || fail "gen again"
    cmp -s "$work/1" "$work/2" || fail "generating twice should match"

    for op in list read; do
        line=$("$bench" "$op" tar.gz "$w" "$work/1") || fail "$op"
        entries=$(echo "$line" | cut -f7)
        [ "$entries" = 20000 ] || fail "$op entries: $entries"
    done
    bytes=$("$bench" read tar.gz "$w" "$work/1" | cut -f8)
    [ "$bytes" = 81920000 ] || fail "read bytes: $bytes"

    out=$("$bench" write tar.gz "$w" | cut -f9)
    size=$(wc -c <"$work/1" | tr -d ' ')
    [ "$out" = "$size" ] || fail "write made $out bytes, gen $size"

    # An archive that isn't the workload's is an error, not a fast result.
    "$bench" read tar.gz large-text "$work/1" >/dev/null 2>&1 &&
        fail "read should notice the wrong workload"
    head -c 100000 "$work/1" >"$work/cut"
    "$bench" read tar.gz "$w" "$work/cut" >/dev/null 2>&1 &&
        fail "read should notice a truncated archive"
    "$bench" read tar.gz "$w" "$work/missing" >/dev/null 2>&1 &&
        fail "read should notice a missing file"
    "$bench" read nope "$w" "$work/1" >/dev/null 2>&1 &&
        fail "unknown format should fail"
    "$bench" >/dev/null 2>&1
    [ $? -eq 2 ] || fail "no arguments should be a usage error"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures failed"
    exit 1
fi
echo "all passed"
