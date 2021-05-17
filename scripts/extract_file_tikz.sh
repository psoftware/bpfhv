#!/bin/bash
SCHEDALG="rr"
SCHEDFLW="*"
RESULTSPATH=/home/antonio/bpfhv/scripts/results
SAVEPATH=/home/antonio/bpfhv/scripts/results/tikz

cd $RESULTSPATH
for SCENARIO in *-*; do
  for PREFIX in nmreplay pktgen; do
    for IFTYPE in sink netmap; do
      for MARKTYPE in none hv guest; do
	RESBASE=${RESULTSPATH}/${SCENARIO}/${PREFIX}-${MARKTYPE}-${IFTYPE}-${SCHEDALG}-${SCHEDFLW}-
	if [ $(ls -R 2>/dev/null -l $RESBASE* | wc -l) -eq 0  ]; then
		echo $RESBASE does not exists.
		continue;
	fi

        OUTFILE=${SAVEPATH}/$SCENARIO-$PREFIX-$IFTYPE-$MARKTYPE
        echo $OUTFILE

	> $OUTFILE
        for ITER in $(seq 1 19); do
          RESFILE=${RESBASE}$(printf '%02u\n' $ITER)
          grep "Speed" ${RESFILE} | \
                awk -v prefix=${ITER} '{s+= ($3 == "Kpps") ? $2 : $2*1024} END {print prefix " " s/1024}' >> $OUTFILE
          #     awk -v prefix=${ITER} '{s+= ($3 == "Kpps") ? $2 : $2*1024} END {print "(" prefix "," s/1024 ")"}' >> $OUTFILE
          #     cut -d  " " -f2 | awk '{print "("NR "," $1/1024 ")"}' | tr -d '\n'
        done
      done
    done
  done
done
