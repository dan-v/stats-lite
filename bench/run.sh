#!/bin/zsh
# run.sh <label> <app-bundle|"none"> <process-name> [warmup_s=20] [window_s=60]
# Launches the app hidden, waits for warmup, measures the process with rumeter and
# WindowServer CPU time with ps, then quits the app. Appends one JSON line to
# results/results.jsonl. Use app "none" for a control run (WindowServer only).
set -e
HERE=${0:A:h}
LABEL=$1; APP=$2; PROC=$3; WARM=${4:-20}; WIN=${5:-60}
OUT=$HERE/results/results.jsonl

ws_ms() { ps -o time= -p $(pgrep -x WindowServer) | awk -F'[:.]' '{ if (NF==3) print ($1*60+$2)*1000+$3*10; else print (($1*60+$2)*60+$3)*1000+$4*10 }'; }

# Only one monitor may run at a time so WindowServer numbers are attributable.
for p in Stats StatsLite AirStats; do pkill -x $p 2>/dev/null || true; done; sleep 2
if [[ $APP != none ]]; then
  # By exact bundle path (-a may resolve another bundle with the same name). Knobs go
  # in as launch arguments: `defaults write` right before launch can be read stale.
  if [[ $PROC == StatsLite ]]; then
    mkdir -p $HERE/results/logs
    open -g --env LITE_DEBUG=1 --stderr "$HERE/results/logs/$LABEL-$(date +%H%M%S).log" "${APP:A}" --args ${=ARGS}
  else
    open -g "${APP:A}"
  fi
  for i in {1..50}; do PID=$(pgrep -x $PROC | head -1); [[ -n $PID ]] && break; sleep 0.2; done
  [[ -z $PID ]] && { echo "failed to start $PROC"; exit 1; }
fi
# Keep the display on and the user "active" so no screen saver/lock kicks in mid-run.
caffeinate -dimu -t $((WARM + WIN + 10)) &
CAF=$!
sleep $WARM
LOCK0=$($HERE/session-state)
W0=$(ws_ms)
if [[ $APP != none ]]; then
  R=$($HERE/rumeter $PID $WIN)
else
  sleep $WIN; R='{"pid":0}'
fi
W1=$(ws_ms)
LOCK1=$($HERE/session-state)
kill $CAF 2>/dev/null || true
[[ $LOCK0$LOCK1 == *locked=1* ]] && LABEL="INVALID-locked-$LABEL"
WSPCT=$(echo "($W1 - $W0) / ($WIN * 10)" | bc -l)
LINE=$(echo $R | sed "s/^{/{\"label\":\"$LABEL\",\"ts\":\"$(date +%FT%T)\",\"ws_ms\":$((W1-W0)),\"ws_pct\":$(printf %.3f $WSPCT),/")
echo $LINE | tee -a $OUT
if [[ $APP != none ]]; then pkill -x $PROC || true; sleep 1; fi
