#!/bin/zsh
# Final head-to-head: original Stats vs AirStats (user's config) vs Stats Lite.
# Three interleaved rounds with no-app controls. ~45 minutes.
HERE=${0:A:h}; cd $HERE
LITE=${LITE_APP:-$HERE/../build/StatsLite.app}
WIN=${WIN:-60}
T=${TAG:-final}
FOUR="-modules cpu,gpu,mem,disk -style.cpu text -style.gpu text -style.mem text -style.disk text"
ALL="-modules cpu,gpu,mem,disk,net,sensors,battery"
for round in 1 2 3; do
  ./run.sh control none none 5 $WIN
  ./original-scenario.sh four; sleep 3; ./run.sh orig-four-1s /Applications/Stats.app Stats 20 $WIN
  ./run.sh airstats-user-2s /Applications/AirStats.app AirStats 20 $WIN
  ARGS="$FOUR -interval 1" ./run.sh $T-four-1s $LITE StatsLite 20 $WIN
  ARGS="$FOUR -interval 2" ./run.sh $T-four-2s $LITE StatsLite 20 $WIN
  ARGS="$FOUR -interval 1 -style.cpu both -style.gpu both" ./run.sh $T-four-charts-1s $LITE StatsLite 20 $WIN
  ARGS="$FOUR -interval 1 -style.cpu spark -style.gpu spark -style.mem spark" ./run.sh $T-four-spark-1s $LITE StatsLite 20 $WIN
  ARGS="$FOUR -interval 1 -benchPanel YES" ./run.sh $T-four-panel-1s $LITE StatsLite 20 $WIN
  ./original-scenario.sh all; sleep 3; ./run.sh orig-all-1s /Applications/Stats.app Stats 20 $WIN
  ARGS="$ALL -interval 1" ./run.sh $T-all-1s $LITE StatsLite 20 $WIN
done
./original-scenario.sh restore
echo SUITE-DONE
