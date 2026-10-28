#!/bin/zsh
# lite-scenario.sh <modules> <interval>   e.g.  lite-scenario.sh cpu,gpu,mem,disk 1
D=dev.statslite.app
defaults write $D modules -string ${1:-cpu,gpu,mem,disk}
defaults write $D interval -int ${2:-1}
defaults write $D labels -bool true
