#!/bin/bash

POINTS="$(seq 1 39)"
POINTS="$POINTS $(seq 40 5 100)"
for t in ${POINTS}; do
    VAL=$(./sched -t $t -a39 -G -m1 -b 500G -q 100 -i 3000 -c 3000 -z sched_null -- -flowsets 1:1500:100 -alg rr 2>&1 | grep TOTAL | awk '{print $11}')
    #VAL=$(./sched -t $t -a39 -G -m1 -b 500G -q 100 -i 3000 -c 3000 -z sched_udp -- -flowsets 1:1500:100 -alg rr 2>&1 | grep TOTAL | awk '{print $11}')
    #VAL=$(./sched -t $t -m1 -G -z nosched_udp 2>&1 | grep TOTAL | awk '{print $11}')
    echo "($t, $VAL)"
done
