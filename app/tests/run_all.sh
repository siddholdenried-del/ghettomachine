#!/bin/sh
# Every simulator suite: run_all.sh HOSTBIN OUTDIR  (from the repository's top; totals, and each failing line).
# A suite written with the first factory set (all of them until 0.5's second set) runs with GM_OLD_PRESETS=1; one
# whose first lines say "# presets: new" runs on the set the FM-1 has.
B="$1"; O="$2"; rm -rf "$O"; mkdir -p "$O"; ok=0; fail=0; n=0
for t in app/tests/*.txt; do
  if head -5 "$t" | grep -q '^# presets: new'; then out=$("$B" "$t" "$O" 2>&1); else out=$(GM_OLD_PRESETS=1 "$B" "$t" "$O" 2>&1); fi
  n=$((n+1))
  a=$(printf '%s\n' "$out" | grep -c '^  ok '); f=$(printf '%s\n' "$out" | grep -c '^FAIL')
  ok=$((ok+a)); fail=$((fail+f))
  if [ "$f" != 0 ]; then echo "== $t: $f failed"; printf '%s\n' "$out" | grep '^FAIL' | head -${3:-6} | cut -c1-200; fi
done
echo "suites $n  ok $ok  fail $fail  wavs $(ls "$O"/*.wav 2>/dev/null | wc -l | tr -d ' ')"
[ "$fail" = 0 ]
