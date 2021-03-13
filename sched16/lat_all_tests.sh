#!/bin/bash

./lat_run_tests_hpc.sh 1 35 2>&1 | tee exp/latency_nosched_udp_tc_drr_N35.txt
./lat_run_tests_hpc.sh 0 35 2>&1 | tee exp/latency_sched_udp_drr_N35.txt

