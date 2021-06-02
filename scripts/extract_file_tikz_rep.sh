#!/bin/bash
SCHEDALG="rr"
SCHEDFLW="*"
RESULTSPATH=/home/antonio/bpfhv/scripts/results
SAVEPATH=/home/antonio/bpfhv/scripts/results/tikz

cd $RESULTSPATH
#for SCENARIO in *-*; do
for SCENARIO in 10-heaviermark-bpfjit; do
  for PREFIX in nmreplay pktgen; do
    for IFTYPE in sink netmap; do
      for MARKTYPE in none hv guest; do
	# Check if at least one repetition of this experiment has been made
	RESBASE=${RESULTSPATH}/${SCENARIO}/*/${PREFIX}-${MARKTYPE}-${IFTYPE}-${SCHEDALG}-${SCHEDFLW}-
	if [ $(ls -R 2>/dev/null -l $RESBASE* | wc -l) -eq 0  ]; then
		echo $RESBASE does not exists.
		continue;
	fi

        OUTFILE=${SAVEPATH}/$SCENARIO-$PREFIX-$IFTYPE-$MARKTYPE
        echo $OUTFILE

	# Empty file
	> $OUTFILE

	# For each NGUEST experiment (sorted)
        for ITER in $(seq 1 19); do
	  # Get total scheduler bandwidth by summing client rates and computing mean for all repetitions
	  THARR=()
	  for REPET in $(seq 1 5); do
  	    RESFILE=${RESULTSPATH}/${SCENARIO}/${REPET}/${PREFIX}-${MARKTYPE}-${IFTYPE}-${SCHEDALG}-${SCHEDFLW}-$(printf '%02u\n' $ITER)
            THARR+=( $(grep "Speed" ${RESFILE} | \
                   awk '{s+= ($3 == "Kpps") ? $2 : $2*1024} END {print s/1024}' ) )
            #      awk -v prefix=${ITER} '{s+= ($3 == "Kpps") ? $2 : $2*1024} END {print prefix " " s/1024}' >> $OUTFILE
	  done

	  # Compute sample mean
	  MEAN=0
	  for val in "${THARR[@]}"; do
	    MEAN=$(echo $MEAN ${val} | awk '{print $1 + $2}')
	  done
	  MEAN=$(echo $MEAN | awk '{print $1/5}')

	  # Compute sample variance
	  VARIANCE=0
	  for val in "${THARR[@]}"; do
            VARIANCE=$(echo $VARIANCE ${val} $MEAN | awk '{print $1 + ($2 - $3)^2}')
          done
	  VARIANCE=$(echo $VARIANCE | awk '{print $1/4}')
	  STD=$(echo $VARIANCE | awk '{print sqrt($1)}')

	  echo $ITER : mean=$MEAN std=$STD var=$VARIANCE
	  echo $ITER $MEAN >> $OUTFILE
        done
      done
    done
  done
done
