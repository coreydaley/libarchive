# SPDX-License-Identifier: BSD-2-Clause
#
# Reads compare.sh's runs.tsv and reports, for each case and metric, how build B
# compares with build A.
#
# Each round gives one ratio, B over A.  The result is the median ratio and a
# confidence interval of at least 95% for it, taken from the sorted ratios by
# the sign test.  That assumes nothing about how the timings are distributed,
# and a stray slow run moves it very little.
#
#   worse    the interval leaves out 1, and the median is past the threshold
#   better   the same, in the other direction
#   same     the whole interval is within the threshold
#   unclear  none of these: more rounds, or a quieter machine, might settle it
#
# Exits 2 if the builds disagree on what they read or how much, because then
# their timings aren't measuring the same work.

BEGIN {
	FS = "\t"
	if (threshold == "")
		threshold = 0.01
	nmetrics = split("wall_ns cpu_ns max_rss out_bytes", metrics, " ")
	name["wall_ns"] = "wall"
	name["cpu_ns"] = "cpu"
	name["max_rss"] = "rss"
	name["out_bytes"] = "size"
	scale["wall_ns"] = 1e6
	scale["cpu_ns"] = 1e6
	scale["max_rss"] = 1048576
	scale["out_bytes"] = 1048576
	unit["wall_ns"] = "ms"
	unit["cpu_ns"] = "ms"
	unit["max_rss"] = "MiB"
	unit["out_bytes"] = "MiB"
}

NR == 1 {
	for (i = 1; i <= NF; i++)
		col[$i] = i
	next
}

{
	key = $col["format"] " " $col["workload"] " " $col["op"]
	if (!(key in seen)) {
		seen[key] = 1
		keys[++nkeys] = key
	}
	pair = $col["pair"]
	build = $col["build"]
	if (pair + 0 > npairs + 0)
		npairs = pair
	for (m = 1; m <= nmetrics; m++)
		value[key, pair, build, metrics[m]] = $col[metrics[m]]
	work[key, pair, build] = $col["entries"] " " $col["bytes"]
}

function sort(a, n,    i, j, t) {
	for (i = 2; i <= n; i++) {
		t = a[i]
		for (j = i - 1; j >= 1 && a[j] > t; j--)
			a[j + 1] = a[j]
		a[j + 1] = t
	}
}

function median(a, n) {
	if (n % 2 == 1)
		return (a[(n + 1) / 2])
	return ((a[n / 2] + a[n / 2 + 1]) / 2)
}

# The largest k such that the k-th smallest and k-th largest of n ratios bound
# the median with at least 95% confidence; 0 if n is too small for any k.
function interval_rank(n,    k, term, cumulative, best) {
	term = 2 ^ -n
	cumulative = 0
	best = 0
	for (k = 1; k <= n / 2; k++) {
		cumulative += term
		if (2 * cumulative > 0.05)
			break
		best = k
		term = term * (n - k + 1) / k
	}
	return (best)
}

function verdict(med, lo, hi) {
	if (lo > 1 && med >= 1 + threshold)
		return ("worse")
	if (hi < 1 && med <= 1 - threshold)
		return ("better")
	if (lo >= 1 - threshold && hi <= 1 + threshold)
		return ("same")
	return ("unclear")
}

END {
	if (nkeys == 0) {
		print "compare.awk: no runs" > "/dev/stderr"
		exit 2
	}
	for (k = 1; k <= nkeys; k++) {
		for (p = 1; p <= npairs; p++) {
			if (work[keys[k], p, "A"] != work[keys[k], p, "B"]) {
				printf("%s: the builds did different work in " \
				    "round %d: %s vs %s\n", keys[k], p,
				    work[keys[k], p, "A"],
				    work[keys[k], p, "B"]) > "/dev/stderr"
				exit 2
			}
		}
	}

	printf("%-7s %-13s %-6s %-5s %10s %10s %8s %17s  %s\n", "format",
	    "workload", "op", "what", "A", "B", "change", "95% interval",
	    "result")
	for (k = 1; k <= nkeys; k++) {
		split(keys[k], parts, " ")
		for (m = 1; m <= nmetrics; m++) {
			metric = metrics[m]
			n = 0
			for (p = 1; p <= npairs; p++) {
				va = value[keys[k], p, "A", metric] + 0
				vb = value[keys[k], p, "B", metric] + 0
				if (va <= 0)
					continue
				n++
				ratio[n] = vb / va
				as[n] = va
				bs[n] = vb
			}
			if (n == 0)
				continue
			sort(ratio, n)
			sort(as, n)
			sort(bs, n)
			med = median(ratio, n)
			rank = interval_rank(n)
			if (rank == 0) {
				result = "too few rounds"
				shown = "-"
			} else {
				lo = ratio[rank]
				hi = ratio[n - rank + 1]
				result = verdict(med, lo, hi)
				shown = sprintf("%+.1f%% to %+.1f%%",
				    (lo - 1) * 100, (hi - 1) * 100)
			}
			printf("%-7s %-13s %-6s %-5s %10.1f %10.1f %+7.1f%% " \
			    "%17s  %s\n", parts[1], parts[2], parts[3],
			    name[metric], median(as, n) / scale[metric],
			    median(bs, n) / scale[metric], (med - 1) * 100,
			    shown, result)
		}
	}
	printf("\nA and B are medians, in ms for wall and cpu and MiB for " \
	    "rss and size. Rounds: %d. Threshold: %.1f%%.\n", npairs,
	    threshold * 100)
}
