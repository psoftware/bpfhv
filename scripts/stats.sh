#!/bin/bash

cat /home/antonio/sharedvm/tests/result_* | grep pps
cat /home/antonio/sharedvm/tests/result_* | grep pps | cut -d" " -f2 | awk '{s+=$1} END {print "sum: " s " Kpps, avg: " s/NR " Kpps"}'
