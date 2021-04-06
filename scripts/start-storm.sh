#!/bin/bash
NUMGUESTS=$1
#COREMAP=(0 2 4 6 8 10 12 14 16 18 20 22 24 26 28 30 32 34 36 38    1 3 5 7 9 11 13 15 17 19 21 23 25 27 29 31 33 35 37 39)
# NUMA 0 = 0-18,+20 NUMA 1 = 1-19,+20
COREMAP=(0 2 4 6 8 10 12 14 16 18 1 3 5 7 9 11 13 15 17 19)
SOCK=/tmp/server
IMG="/home/antonio/images/debian.qcow2"
SHARED_DIR="/home/antonio/sharedvm"

for I in $(seq 1 $NUMGUESTS); do
	NUMAID=$((I / 10))

	VMID="vm$I"
	MACID=$(printf '%02x\n' $I)
	VNC="-vnc :$I"
	MEM=1G
	CORES=2
	NOGRAPHIC=""
	TMPPID=/tmp/$VMID.pid

	rm -f $TMPPID

	numactl --membind=$NUMAID \
	 qemu-system-x86_64 ${IMG} -snapshot \
		-name ${VMID},debug-threads=on \
		-pidfile $TMPPID \
	        -enable-kvm -smp ${CORES} -m ${MEM} -vga std ${NOGRAPHIC} \
		${VNC} \
		-fsdev local,id=test_dev,path=${SHARED_DIR},security_model=none -device virtio-9p-pci,fsdev=test_dev,mount_tag=hv_mount \
	        -numa node,memdev=mem0 \
	        -object memory-backend-file,id=mem0,size=${MEM},mem-path=/dev/hugepages/${VMID},share=on \
        	-device bpfhv-pci,netdev=data20,mac=00:AA:BB:CC:0a:${MACID},num_tx_bufs=512 \
        	-netdev type=bpfhv-proxy,id=data20,chardev=char20 \
	        -chardev socket,id=char20,path=${SOCK} \
		-daemonize

	sleep 0.3
	QEMUPID=$(cat $TMPPID)
	THIDX=$(($I))
	TH1=${COREMAP[$THIDX]}
	TH2=$(($TH1+20))
	qemu-affinity $QEMUPID -k $TH1 $TH2 -p $TH1,$TH2 -w $TH1,$TH2
	echo "Started $VMID: QEMUPID=$QEMUPID MAC=$MACID NUMAID=$NUMAID TH1=$TH1 TH2=$TH2"
done
