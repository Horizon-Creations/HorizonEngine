#!/bin/zsh
# keepawake.sh <seconds>: re-declare user activity every 5 s so the display neither sleeps nor stays off
end=$(( $(date +%s) + $1 ))
while [ $(date +%s) -lt $end ]; do /usr/bin/caffeinate -u -t 6; sleep 4; done
