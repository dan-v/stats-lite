#!/bin/zsh
# original-scenario.sh <defaults|all|restore>
# Configures the installed original Stats for a benchmark scenario.
# defaults: the original's out-of-box modules (CPU, RAM, Disk mini; Network speed; Battery) at 1 s.
# four:     CPU, GPU, RAM, Disk mini at 1 s (the user's setup).
# all:      defaults + GPU mini + Sensors mini (CPU temperature).
# restore:  re-import the user's backed-up preferences.
D=eu.exelban.Stats
HERE=${0:A:h}
case $1 in
restore) defaults delete $D; defaults import $D $HERE/.original-prefs-backup.plist; exit ;;
esac
defaults delete $D 2>/dev/null
defaults write $D setupProcess -bool true
defaults write $D runAtLoginInitialized -bool true
defaults write $D version -string 3.0.23
defaults write $D updater_check_ts -int $(date +%s)
defaults write $D remote_monitoring -bool false
for m in CPU RAM Disk; do defaults write $D ${m}_state -bool true; defaults write $D ${m}_widget -string mini; defaults write $D ${m}_updateInterval -int 1; done
defaults write $D Network_state -bool true; defaults write $D Network_widget -string speed; defaults write $D Network_updateInterval -int 1
defaults write $D Battery_state -bool true; defaults write $D Battery_widget -string battery
defaults write $D Bluetooth_state -bool false; defaults write $D Clock_state -bool false; defaults write $D Remote_state -bool false
if [[ $1 == four ]]; then
  # the four widgets the user runs: CPU, GPU, RAM, Disk (mini) at 1 s
  defaults write $D Network_state -bool false; defaults write $D Battery_state -bool false
  defaults write $D GPU_state -bool true; defaults write $D GPU_widget -string mini; defaults write $D GPU_updateInterval -int 1
  defaults write $D Sensors_state -bool false
elif [[ $1 == all ]]; then
  defaults write $D GPU_state -bool true; defaults write $D GPU_widget -string mini; defaults write $D GPU_updateInterval -int 1
  defaults write $D Sensors_state -bool true; defaults write $D Sensors_widget -string mini; defaults write $D Sensors_sensor -string "Average CPU"; defaults write $D Sensors_updateInterval -int 1
else
  defaults write $D GPU_state -bool false; defaults write $D Sensors_state -bool false
fi
