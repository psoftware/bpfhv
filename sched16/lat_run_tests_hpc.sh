#!/bin/bash

SCHED=${1:-1} # 0 --> TC, 1 --> sched
N=${2:-5} # Number of clients (including the observed one)
IF1=lo
FLOWS=100

MODE=""
if [ "$SCHED" != "0" ]; then
    MODE="${MODE}sched_"
else
    MODE="${MODE}nosched_"
fi
MODE="${MODE}udp"

ARGS="-T -z ${MODE} -t $N -d 127.0.0.1"

if [ "$SCHED" != "0" ]; then
    ARGS="${ARGS} -k1 -a 39 -G -m1 -q 2000 -c 500 -i 500 -- -flowsets 50:60:1,1:60:${FLOWS} -alg rr"
else
    ARGS="${ARGS} -G -D"
fi

KPPS_LIST=$(seq 20 20 1200)
KPPS_LIST="${KPPS_LIST} $(seq 1500 500 5000)"
KPPS_LIST="${KPPS_LIST} $(seq 6000 1000 9000)"
KPPS_LIST="${KPPS_LIST} $(seq 10000 2000 20000)"
KPPS_LIST="${KPPS_LIST} 25000 30000"
KPPS_LIST="${KPPS_LIST} $(seq 40000 10000 70000)"

sudo ./tc_configure.sh $IF1 x  # clean up TC configuration

for KPPS in ${KPPS_LIST}; do
    echo "Run test #$j at $KPPS Kpps"
    BW=$(($KPPS * 480)) # in Kbps

    if [ "$SCHED" != "0" ]; then
        ARGS2="-b ${BW}K ${ARGS}"
    else
        sudo ./tc_configure.sh $IF1 drr ${BW}kbit 50 ${FLOWS}
        ARGS2=${ARGS}
    fi
    PADKPPS=$(echo $KPPS | awk '{printf "%07.0f", $0}')
    (./udprecv -a 127.0.0.1 -n 100 -m 1 -T -c 20 -o exp/lat_${MODE}_drr_N${N}_${PADKPPS}kpps | grep Xth) &
    ./sched $ARGS2 &> /dev/null
    sudo pkill -SIGINT udprecv
    sleep 1
done
