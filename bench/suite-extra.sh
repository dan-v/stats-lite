#!/bin/zsh
# New modules and lazy-framework A/B.
HERE=${0:A:h}; cd $HERE
A=$1; B=$2
for round in 1 2; do
  ARGS="-modules cpu,gpu,mem,disk -interval 1" ./run.sh lazyA-four-1s $A StatsLite 20 60
  ARGS="-modules cpu,gpu,mem,disk -interval 1" ./run.sh lazyB-four-1s $B StatsLite 20 60
  ARGS="-modules cpu,gpu,mem,disk,net,sensors,battery,bluetooth,clock -interval 1" ./run.sh lite-nine-1s $B StatsLite 20 60
  ARGS="-modules clock -interval 1" ./run.sh lite-clock-only $B StatsLite 20 120
done
echo EXTRA-DONE
