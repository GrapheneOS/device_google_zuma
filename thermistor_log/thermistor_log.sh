#!/bin/sh
# Script to write THERMISTORS to OUTFILE every 10s.
# Backup OUTFILE to OUTFILE.old if it gets too big (>1000000 bytes)
 
OUTFILE=/mnt/vendor/persist/therm.log
THERMISTORS="soc_therm cam_therm charge_therm rf1_therm disp_therm battery"

while sleep 10; do
	if [ -e ${OUTFILE} ]; then
		if [ `stat -c %s ${OUTFILE}` -ge 1000000 ]; then
			echo "backing up 1MB file";
			mv ${OUTFILE} ${OUTFILE}.old
		else
			echo "exists - small";
		fi
	else
		VALUES="time"
		for thermistor in ${THERMISTORS}; do
			VALUES="${VALUES},${thermistor}"
		done
		echo "${VALUES}" >> ${OUTFILE}
	fi

	TIME=`date +%s`
	VALUES="${TIME}"
	for thermistor in ${THERMISTORS}; do
		READ=`cat /dev/thermal/tz-by-name/${thermistor}/temp`
		TRIMMED=`echo -n ${READ}`
		VALUES="${VALUES},${TRIMMED}"
	done
	echo "${VALUES}" >> ${OUTFILE}
done

