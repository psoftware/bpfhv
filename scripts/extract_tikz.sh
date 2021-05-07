#!/bin/bash
PREFIX="nmreplay"
TESTNAME="09-lightmark-bpfjit"
SCHEDALG="rr"
SCHEDFLW=4
for IFTYPE in sink netmap; do
  echo -e "\n\n"
  echo "$PREFIX-$IFTYPE"
  for SCEN in none hv guest; do
	echo "\addplot coordinates {"
	for ITER in $(seq 1 19); do
	  grep "Speed" /home/antonio/bpfhv/scripts/results/${TESTNAME}/${PREFIX}-${SCEN}-${IFTYPE}-${SCHEDALG}-${SCHEDFLW}-$(printf '%02u\n' $ITER) | \
		awk -v prefix=${ITER} '{s+= ($3 == "Kpps") ? $2 : $2*1024} END {print "(" prefix "," s/1024 ")"}' | tr -d '\n'
	  #	cut -d  " " -f2 | awk '{print "("NR "," $1/1024 ")"}' | tr -d '\n'
	done
	echo -e "};\n\\\addlegendentry{$SCEN}"
  done
done

