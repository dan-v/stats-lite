#!/bin/zsh
# Cost attribution / A-B of implementation variants for the app, four widgets at 1 s.
# Each variant sets hidden defaults knobs, runs twice (interleaved), then knobs are reset.
HERE=${0:A:h}; cd $HERE
LITE=${LITE_APP:-$HERE/../build-v2/StatsLite.app}
WIN=${WIN:-60}
D=dev.statslite.app
knobs() { defaults delete $D benchSkip 2>/dev/null; defaults delete $D benchTimerKind 2>/dev/null; defaults delete $D benchTolerance 2>/dev/null; defaults delete $D renderMode 2>/dev/null; defaults delete $D benchPanel 2>/dev/null; defaults delete $D threadMode 2>/dev/null; for kv in "$@"; do defaults write $D ${kv%%=*} -int ${kv#*=}; done; }
VARIANTS=(
  "base:"
  "exact:renderMode=1"
  "surface:renderMode=2"
  "blitimg:renderMode=3"
  "blitsurf:renderMode=4"
  "noupload:benchSkip=4"
  "norender:benchSkip=2"
  "idle:benchSkip=3"
  "unaligned:benchTimerKind=1"
  "dispatch:benchTimerKind=2"
  "panel:benchPanel=1"
  "worker:threadMode=1"
  "worker-blitsurf:threadMode=1 renderMode=4"
  "worker-idle:threadMode=1 benchSkip=3"
)
./lite-scenario.sh cpu,gpu,mem,disk 1
for round in 1 2; do
  ./run.sh control none none 5 $WIN
  for v in $VARIANTS; do
    name=${v%%:*}; kv=${v#*:}
    knobs ${=kv}
    ./run.sh attrib-$name $LITE StatsLite 15 $WIN
  done
done
knobs
echo ATTRIB-DONE
