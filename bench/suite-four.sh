#!/bin/zsh
# Head-to-head on the user's four widgets (CPU, GPU, RAM, Disk). Interleaved, 2 rounds.
HERE=${0:A:h}; cd $HERE
LITE=${LITE_APP:-$HERE/../build/StatsLite.app}
WIN=${WIN:-60}
for round in 1 2; do
  ./run.sh control none none 5 $WIN
  ./original-scenario.sh four; ./run.sh orig-four-1s /Applications/Stats.app Stats 20 $WIN
  ./run.sh airstats-user-2s /Applications/AirStats.app AirStats 20 $WIN
  ./lite-scenario.sh cpu,gpu,mem,disk 1; ./run.sh ${TAG:-lite}-four-1s $LITE StatsLite 20 $WIN
  ./lite-scenario.sh cpu,gpu,mem,disk 2; ./run.sh ${TAG:-lite}-four-2s $LITE StatsLite 20 $WIN
done
./original-scenario.sh restore
echo SUITE-DONE
