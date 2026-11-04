#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Compares two la_bench builds.  Both read the same archives, generated once by
# build A, so a difference in the writer can't look like one in the reader.
#
# The builds take turns, and swap who goes first on every other round, so that
# anything drifting on the machine lands on both.

set -eu

pairs=15
threshold=0.01
outdir=la-bench-results
only=

usage() {
    echo "usage: compare.sh [-n pairs] [-t threshold] [-o dir] [-c workload]" \
        "bench-a bench-b" >&2
    exit 2
}

while getopts n:t:o:c: opt; do
    case $opt in
    n) pairs=$OPTARG ;;
    t) threshold=$OPTARG ;;
    o) outdir=$OPTARG ;;
    c) only=$OPTARG ;;
    *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 2 ] || usage

a=$1
b=$2
here=$(cd "$(dirname "$0")" && pwd)
runs="$outdir/runs.tsv"
mkdir -p "$outdir"

{
    uname -a
    echo "A: $("$a" version)"
    echo "B: $("$b" version)"
} | tee "$outdir/info.txt"

cases=$("$a" cases)
if [ -n "$only" ]; then
    cases=$(echo "$cases" | awk -F'\t' -v w="$only" '$2 == w')
    [ -n "$cases" ] || { echo "compare.sh: no workload $only" >&2; exit 2; }
fi

echo "$cases" | while IFS='	' read -r format workload; do
    "$a" gen "$format" "$workload" "$outdir/$format-$workload.archive"
done

run() {
    # $1 pair, $2 build name, $3 binary, $4 op, $5 format, $6 workload
    if [ "$4" = write ]; then
        line=$("$3" "$4" "$5" "$6")
    else
        line=$("$3" "$4" "$5" "$6" "$outdir/$5-$6.archive")
    fi
    printf '%s\t%s\t%s\n' "$1" "$2" "$line" >>"$runs"
}

printf 'pair\tbuild\t%s\n' "$("$a" header)" >"$runs"

pair=1
while [ "$pair" -le "$pairs" ]; do
    echo "$cases" | while IFS='	' read -r format workload; do
        for op in list read write; do
            if [ $((pair % 2)) -eq 1 ]; then
                run "$pair" A "$a" "$op" "$format" "$workload"
                run "$pair" B "$b" "$op" "$format" "$workload"
            else
                run "$pair" B "$b" "$op" "$format" "$workload"
                run "$pair" A "$a" "$op" "$format" "$workload"
            fi
        done
    done
    pair=$((pair + 1))
done

awk -f "$here/compare.awk" -v threshold="$threshold" "$runs" |
    tee "$outdir/report.txt"
