#!/bin/zsh
# quick-ab.sh <appA> <appB> [rounds=3] [window_s=30] [mode=four|panel|chart]
# Runs both builds at the same time over the same window, so machine activity hits
# both equally; prints per-round instruction ratios B/A and the median.
# ARGS_A / ARGS_B add launch arguments to one side (e.g. compare a setting in one build).
HERE=${0:A:h}
A=${1:A}/Contents/MacOS/StatsLite; B=${2:A}/Contents/MacOS/StatsLite  # resolve before cd
cd $HERE
[[ -x $A && -x $B ]] || { echo "no app at $A or $B"; exit 1; }
R=${3:-3}; WIN=${4:-30}; MODE=${5:-four}; WARM=${WARM:-12}
ST=text; [[ $MODE == chart ]] && ST=both
ARGS=(-modules cpu,gpu,mem,disk -style.cpu $ST -style.gpu $ST -style.mem $ST -style.disk $ST -interval 1)
[[ $MODE == panel ]] && ARGS+=(-benchPanel YES)
TMP=$(mktemp -d)
for p in Stats StatsLite AirStats; do pkill -x $p 2>/dev/null; done; sleep 1
caffeinate -dimu -t $(( R * (WARM + WIN + 4) + 10 )) &
CAF=$!
for r in $(seq 1 $R); do
  if (( r % 2 )); then
    $A $ARGS ${=ARGS_A} -historyPath $TMP/a.bin 2>/dev/null & PA=$!; sleep 0.5
    $B $ARGS ${=ARGS_B} -historyPath $TMP/b.bin 2>/dev/null & PB=$!
  else
    $B $ARGS ${=ARGS_B} -historyPath $TMP/b.bin 2>/dev/null & PB=$!; sleep 0.5
    $A $ARGS ${=ARGS_A} -historyPath $TMP/a.bin 2>/dev/null & PA=$!
  fi
  sleep $WARM
  ./rumeter $PA,$PB $WIN > $TMP/r$r.json
  kill $PA $PB 2>/dev/null; wait $PA $PB 2>/dev/null; sleep 1
done
kill $CAF 2>/dev/null
python3 - $TMP $R <<'PY'
import json, sys, statistics as st, glob
tmp, R = sys.argv[1], int(sys.argv[2])
rat = {"instructions": [], "energy_mj": [], "syscalls": [], "cycles": []}
for r in range(1, R + 1):
    rows = [json.loads(l) for l in open(f"{tmp}/r{r}.json") if l.strip().startswith("{")]
    a, b = rows[0], rows[1]  # rumeter prints in the order given: A then B
    for k in rat: rat[k].append(b[k] / a[k] if a[k] else float("nan"))
    print(f"round {r}: A {a['instructions']/1e6:.1f} M  B {b['instructions']/1e6:.1f} M instr  |  A {a['energy_mj']:.2f}  B {b['energy_mj']:.2f} mJ")
for k, v in rat.items():
    print(f"B/A {k:13s} median {st.median(v):.3f}   rounds " + " ".join(f"{x:.3f}" for x in v))
PY
rm -rf $TMP
