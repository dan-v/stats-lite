#!/bin/zsh
# A/B two builds of Stats Lite: suite-ab.sh <labelA> <appA> <labelB> <appB> [rounds]
# STYLE=text|chart|both sets the menu bar style of the four modules (default text).
HERE=${0:A:h}; cd $HERE
LA=$1; A=$2; LB=$3; B=$4; R=${5:-2}; WIN=${WIN:-60}
ST=${STYLE:-text}
FOUR="-modules cpu,gpu,mem,disk -style.cpu $ST -style.gpu $ST -style.mem $ST -style.disk $ST -interval 1"
for round in $(seq 1 $R); do
  ARGS="$FOUR" ./run.sh $LA-four-1s $A StatsLite 20 $WIN
  ARGS="$FOUR" ./run.sh $LB-four-1s $B StatsLite 20 $WIN
  ARGS="$FOUR -benchPanel YES" ./run.sh $LA-panel-1s $A StatsLite 20 $WIN
  ARGS="$FOUR -benchPanel YES" ./run.sh $LB-panel-1s $B StatsLite 20 $WIN
done
echo AB-DONE
