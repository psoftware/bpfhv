#!/bin/bash

PKTSIZE=1500
BASEPORT=10302

if [ true ]; then
	map[0]=0
	map[1]=4
	map[2]=1
	map[3]=5
	map[4]=2
	map[5]=6
else
	map[0]=0
	map[1]=4
	map[2]=1
	map[3]=5
	map[4]=2
	map[5]=6
fi

for i in $(seq $1 $2); do
    PORT=$(($BASEPORT + $i))
    set -x
    sudo taskset -c ${map[$i]} pkt-gen -f tx -l $PKTSIZE -i enp1s0f1-$i -s 10.60.1.2:$PORT &> /dev/null &
    #sudo taskset -c ${map[$i]} pkt-gen -f tx -l $PKTSIZE -i enp1s0f1-$i -s 10.60.1.2:10302 &> /dev/null &
    set +x
done
