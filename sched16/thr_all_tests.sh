#!/bin/bash

./thr_run_tests.sh 0 0 | tee exp/throughput_nosched_udp_notc.txt
./thr_run_tests.sh 0 1 | tee exp/throughput_nosched_udp_tc_drr.txt
./thr_run_tests.sh 1 | tee exp/throughput_sched_udp_drr.txt

