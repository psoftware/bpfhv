#!/bin/bash

AVG=$(cat /home/antonio/sharedvm/tests/result_* | grep pps | cut -d" " -f2 | awk '{s+=$1} END {print s/NR}')

echo $AVG

