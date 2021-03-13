#!/bin/bash

SCHED=${1:-1}
TC=${2:-0}
IO="udp" # null, netmap

MODE=""
if [ "$SCHED" != "0" ]; then
    MODE="${MODE}sched_"
else
    MODE="${MODE}nosched_"
fi
MODE="${MODE}${IO}"

ARGS="-z ${MODE}"

IF1=enp1s0f1
IF2=enp1s0f0

sudo ./tc_configure.sh $IF1 x  # clean up TC configuration

if [ "$SCHED" != "0" ]; then
    ARGS="${ARGS} -G -m1 -b 1000G -q 100 -c 1000 -i 1000 -- -flowsets 1:60:32 -alg rr"
else
    if [ "$TC" != "0" ]; then
        sudo ./tc_configure.sh $IF1 drr 500000mbit
    fi
    ARGS="${ARGS} -G -D"
fi

for i in $(seq 1 8); do
    for j in $(seq 1 10); do
        echo "Run test #$j with $i clients"
        ./sched -t $i $ARGS 2>&1 | grep TOTAL
    done
done
