#!/bin/bash

export LANG=C
FILE=$1
T=${2:-10}

if [ ! -f "$FILE" ]; then
    echo "Wrong or empty input file path"
    exit 1
fi

PPSLIST=$(grep -o "[^ ]\+ pps" $FILE | grep -o "^[^ ]\+")

XLIST=""
for i in $PPSLIST; do
    M=$(echo $i | grep -o "^.....")
    E=$(echo $i | grep -o "..$")
    X=$(echo "$M * (10^$E)" | bc -l)
    XLIST="${XLIST} $X"
done

N=1
K=0
CURLIST=""
for i in $XLIST; do
    CURLIST="$CURLIST $i"
    K=$((K + 1))

    if [ $K -ge $T ]; then
        EXPR="("
        for j in $CURLIST; do
            EXPR="${EXPR}$j+"
        done
        EXPR="${EXPR}0)/$T"
        MEAN=$(echo "$EXPR" | bc -l)

        EXPR="sqrt(("
        for j in $CURLIST; do
            EXPR="${EXPR}($j-$MEAN)^2+"
        done
        EXPR="${EXPR}0)/$T)/1000000"
        STDDEV=$(echo "$EXPR" | bc -l | awk '{printf "%f", $0}')
        MEAN=$(echo "$MEAN/1000000" | bc -l | awk '{printf "%f", $0}')

        echo "$N $MEAN $STDDEV"

        K=0
        N=$((N + 1))
        CURLIST=""
    fi
done
