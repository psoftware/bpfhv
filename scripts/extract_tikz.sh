#!/bin/bash
PREFIX="pktgen"
SCHEDALG="rr"
SCHEDFLW=4
for IFTYPE in sink netmap; do
  echo -e "\n\n\n"
  echo "$PREFIX-$IFTYPE"
  for SCEN in none hv guest; do
	echo "\addplot coordinates {"
	grep "sum" /home/antonio/bpfhv/scripts/results/02-nmreplay/${PREFIX}-${SCEN}-${IFTYPE}-${SCHEDALG}-${SCHEDFLW}-* | \
		cut -d  " " -f2 | awk '{print "("NR "," $1/1024 ")"}'
	echo -e "};\n\\addlegendentry{$SCEN}"
  done
done

