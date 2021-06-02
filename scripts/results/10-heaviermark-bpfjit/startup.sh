#!/bin/bash
# Code executed on guest at startup, after shared fs mount
IFNAME=ens4
ETHFILE=/sys/class/net/$IFNAME
HVDIR=/mnt/hv/
SYNCFILE=/mnt/hv/release_barrier

# remove this: avoid kernel addr rand for gdb debugging
#echo 0 > /proc/sys/kernel/randomize_va_space

# enable jit for best performance
echo 1 > /proc/sys/net/core/bpf_jit_enable

# Setup bpfhv module and netmap
cd /root
insmod $HVDIR/bin/bpfhv.ko tx_napi=1

# wait for ens4 to come up
while [ ! -d "$ETHFILE" ]; do
        sleep 0.2
done

ip link set ${IFNAME} up
insmod $HVDIR/bin/netmap.ko

# evaluate if to remove: disable netmap txqdisc!
#sleep 1
#echo 0 > /sys/module/netmap/parameters/generic_txqdisc

# Get guest info
sleep 1
VMMAC=$(cat /sys/class/net/${IFNAME}/address | cut -d':' -f6);
VMID=$((16#$VMMAC));

# Wait for hv barrier
echo "Waiting for start..."
while [ ! -f "$SYNCFILE" ]; do
        sleep 0.2
done

TESTFILENAME=$HVDIR/tests/result_$(printf '%02u\n' $VMID).txt
# Perform test
echo "Started."
# 1) pkt-gen
#./pkt-gen -f tx -i netmap:${IFNAME} -a 0 -A > $TESTFILENAME &
# 2) nmreplay
$HVDIR/bin/nmreplay-nort-s -f $HVDIR/traces/bigFlows.pcap -i netmap:$IFNAME -B 5G -A > $TESTFILENAME &

PGRPID=$!
sleep 30
kill -INT $PGRPID
echo "Ended."

# Wait for sharedfs to flush...
sleep 10
shutdown 0
