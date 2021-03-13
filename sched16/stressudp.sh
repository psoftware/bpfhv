#!/bin/bash

sudo pkill udpsend

EXCLUDECORES="3 7"
MAXCORES="8"
NUMSENDERS=1
if [ -n "$1" ]; then
    NUMSENDERS="$1"
fi

CORECNT=0
for i in $(seq 1 $NUMSENDERS); do
    echo "udpsend -N -i $i -c $CORECNT"
    ./udpsend -N -i $i -c $CORECNT > /dev/null 2>&1 &
    while true; do
        CORECNT=$(($CORECNT + 1))
        if [ $CORECNT == $MAXCORES ]; then
            CORECNT="0"
        fi
        FOUND="NO"
        for e in ${EXCLUDECORES}; do
            if [ "$CORECNT" == "$e" ]; then
                FOUND="YES"
                break
            fi
        done
        if [ $FOUND == "NO" ]; then
            break;
        fi
    done
done
