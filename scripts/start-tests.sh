#!/bin/bash

STATUSFILE=/tmp/bpfhvstatus
SCHEDAFFINITY=0
SHOWSTATS="-S"
VERBOSE="-v"

wait_for_status() {
	while true; do
		if [ $(grep "$1" $STATUSFILE -c) -eq 1 ] ; then
			break;
		fi
		sleep 0.5
	done
}

for NCL in $(seq 1 18); do
	IFTYPE="sink"
	MARKSIDE="none"
	NFLOWS=4
	SCHEDALG="rr"

	TESTNAME=$MARKSIDE-$IFTYPE-$SCHEDALG-$NFLOWS-$(printf '%02x\n' $NCL)
	echo "-> Starting test $TESTNAME"

	# Delete stats and barrier file
	rm -f /home/antonio/sharedvm/release_barrier
	rm -f /home/antonio/sharedvm/tests/*

	# Start backend
	echo "Starting backend"
	rm -f /tmp/server
	/home/antonio/bpfhv/proxy/backend -w $NCL -m $IFTYPE -f $MARKSIDE -a $SCHEDAFFINITY -s $STATUSFILE $SHOWSTATS $VERBOSE -- \
		-flowsets 1:1500:$NFLOWS -alg $SCHEDALG >/dev/null 2>/dev/null &
	#/home/antonio/bpfhv/proxy/backend -w $NCL -m $IFTYPE -f $MARKSIDE  -a 0 -S -v -- -flowsets 1:1500:$NFLOWS -alg $SCHEDALG  &
	BACKENDPID=$!
	sleep 2

	# Start guests
	echo "Starting guests"
	/home/antonio/bpfhv/scripts/start-storm.sh $NCL

	# Wait for guest boot
	echo "Waiting for boot"
	#sleep 30
	wait_for_status "Scheduler thread active:1"

	# Start test
	echo "Start guest test"
	touch /home/antonio/sharedvm/release_barrier

	# Wait for test ending
	echo "Waiting for end"
	#sleep 35
	wait_for_status "Active clients:0"

	# Get stats
	/home/antonio/bpfhv/scripts/stats.sh > /home/antonio/bpfhv/scripts/results/$TESTNAME

	# Delete stats and barrier file
	rm /home/antonio/sharedvm/release_barrier
	rm /home/antonio/sharedvm/tests/*

	# Stop backend!
	echo "Stopping backend"
	kill -SIGINT $BACKENDPID
	sleep 2
done
