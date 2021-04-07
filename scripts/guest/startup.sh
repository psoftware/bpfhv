#!/bin/bash
# Code executed on guest at startup, after shared fs mount
IFNAME=ens4
ETHFILE=/sys/class/net/$IFNAME
SYNCFILE=/mnt/hv/release_barrier

# Setup bpfhv module and netmap
cd /root
insmod bpfhv.ko

# wait for ens4 to come up
while [ ! -d "$ETHFILE" ]; do
        sleep 0.2
done

ip link set ${IFNAME} up
insmod netmap.ko

# Get guest info
sleep 1
VMMAC=$(cat /sys/class/net/${IFNAME}/address | cut -d':' -f6);
VMID=$((16#$VMMAC));

# Wait for hv barrier
echo "Waiting for start..."
while [ ! -f "$SYNCFILE" ]; do
        sleep 0.2
done

# Perform test
echo "Started."
./pkt-gen -f tx -i netmap:${IFNAME} -n 8000000 -a 0 -A > /mnt/hv/tests/result_${VMID}.txt
echo "Ended."

# Wait for sharedfs to flush...
sleep 6
shutdown 0
