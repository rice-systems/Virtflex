#!/bin/bash
sleep 1
prev=0
while true
do
	curr=`cat /proc/vmstat | grep numa_pages_migrated | awk '{print $2}'`
	if [ $curr -eq $prev ]; then
		break;
	fi
	prev=$curr
	sleep 0.1
done	
echo -n  Migration progress: numa_pages_migrated $curr , 
date +"%T.%3N"
echo Migration progress: done
