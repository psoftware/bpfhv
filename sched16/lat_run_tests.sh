#!/bin/bash

SCHED=${1:-1} # 0 --> TC, 1 --> sched
N=${2:-5} # Number of clients (including the observed one)
IF1=enp1s0f1
IF2=enp1s0f0

lsmod | grep netmap
if [ "$?" != "0" ]; then
  sudo insmod ~/git/netmap/LINUX/netmap.ko
fi

MODE=""
if [ "$SCHED" != "0" ]; then
    MODE="${MODE}sched_"
else
    MODE="${MODE}nosched_"
fi
MODE="${MODE}udp"

ARGS="-T -z ${MODE} -t $N"

if [ "$SCHED" != "0" ]; then
    ARGS="${ARGS} -k1 -G -m1 -q 2000 -c 500 -i 500 -- -flowsets 50:60:1,1:60:32 -alg rr"
else
    ARGS="${ARGS} -G -D"
fi

KPPS_LIST=$(seq 20 20 1200)
KPPS_LIST="${KPPS_LIST} $(seq 1500 500 5000)"
KPPS_LIST="${KPPS_LIST} $(seq 6000 1000 9000)"
KPPS_LIST="${KPPS_LIST} $(seq 10000 2000 20000)"
KPPS_LIST="${KPPS_LIST} 25000 30000"
KPPS_LIST="${KPPS_LIST} $(seq 40000 20000 100000)"

set -x
sudo ip addr flush dev $IF1
sudo arp -d 10.60.1.1
sudo ip addr add 10.60.1.2/24 dev $IF1
sudo arp -s 10.60.1.1 68:05:ca:2e:74:30 #00:1b:21:80:ea:18
set +x

sudo ./tc_configure.sh $IF1 x  # clean up TC configuration

for KPPS in ${KPPS_LIST}; do
    echo "Run test #$j at $KPPS Kpps"
    BW=$(($KPPS * 480)) # in Kbps

    if [ "$SCHED" != "0" ]; then
        ARGS2="-b ${BW}K ${ARGS}"
    else
        sudo ./tc_configure.sh $IF1 drr ${BW}kbit 50
        ARGS2=${ARGS}
    fi
    PADKPPS=$(echo $KPPS | awk '{printf "%07.0f", $0}')
    (sudo ./udprecv -N netmap:$IF2 -T -n 1 -c 4 -o lat_${MODE}_drr_N${N}_${PADKPPS}kpps | grep Xth) &
    ./sched $ARGS2 &> /dev/null
    sudo pkill -SIGINT udprecv
    sleep 1
done
