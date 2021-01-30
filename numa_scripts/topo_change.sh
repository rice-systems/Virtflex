#!/bin/bash
#declare -a NODE0=(0 4 8 12 16 20 24 28 32 36 40 44)
#declare -a MEM0=(0 2 3 4 5 6 7 8)
#declare -a NODE1=(1 5 9 13 17 21 25 29 33 37 41 45)
#declare -a MEM1=(9 10 11 12 13 14 15 16)
#declare -a NODE2=(2 6 10 14 18 22 26 30 34 38 42 46)
#declare -a MEM2=(17 18 19 20 21 22 23 24)
#declare -a NODE3=(3 7 11 15 19 23 27 31 35 39 43 47)
#declare -a MEM3=(25 26 27 28 29 30 31 32)

declare -a NODE0=(0 1 2 3 4 5 6     		28 29 30 31 32 33 34)
declare -a MEM0=(0 2 3 4 5 6 7 8)
declare -a NODE1=(7 8 9 10 11 12 13   		35 36 37 38 39 40 41)
declare -a MEM1=(9 10 11 12 13 14 15 16)
declare -a NODE2=(14 15 16 17 18 19 20 		42 43 44 45 46 47 48)
declare -a MEM2=(17 18 19 20 21 22 23 24)
declare -a NODE3=(21 22 23 24 25 26 27	 	49 50 51 52 53 54 55)
declare -a MEM3=(25 26 27 28 29 30 31 32)

declare -a ACT_CPU_NODE=(2 3)
declare -a ACT_MEM_NODE=(2 3)

SHOW_MIGRATION=0

if [ "$#" -eq 0 ]; then
    CPU_ACT=1
    echo -n "default cpu action:"
    echo $CPU_ACT
elif [ "$#" -eq 1 ]; then
    CPU_ACT=$1
    echo -n "userdefined cpu action:"
    echo $CPU_ACT
else
    echo "illegal number of parameters"
fi

CPU=1
# action = 0 ==> disable
# action = 1 ==> enable

MEM=0
MEM_ACT="offline"
# action = offline ==> disbale
# action = online  ==> enable

#  for CPU in ${NODE1[*]}
#  do
#  	echo "disabling $CPU"
#  	echo $CPU_ACT > /sys/devices/system/cpu/cpu${CPU}/online
#  done
#  
for NODE in ${ACT_CPU_NODE[*]}
do
    
    if [ $NODE -eq 0 ]; then    
        for CPU in ${NODE0[*]}
        do
      	    echo "CPU $CPU, CPU_ACT $CPU_ACT"
      	    echo $CPU_ACT > /sys/devices/system/cpu/cpu${CPU}/online
        done

    elif [ $NODE -eq 1 ]; then
        for CPU in ${NODE1[*]}
        do
      	    echo "CPU $CPU, CPU_ACT $CPU_ACT"
      	    echo $CPU_ACT > /sys/devices/system/cpu/cpu${CPU}/online
      done
      
    elif [ $NODE -eq 2 ]; then
        for CPU in ${NODE2[*]}
        do
      	    echo "CPU $CPU, CPU_ACT $CPU_ACT"
      	    echo $CPU_ACT > /sys/devices/system/cpu/cpu${CPU}/online
        done
    
    elif [ $NODE -eq 3 ]; then
        for CPU in ${NODE3[*]}
        do
      	    echo "CPU $CPU, CPU_ACT $CPU_ACT"
      	    echo $CPU_ACT > /sys/devices/system/cpu/cpu${CPU}/online
        done
    fi
done
    #for MEM in ${MEM1[*]}
    #do
    #	echo "disabling mem unit $MEM"
    #	echo offline > /sys/devices/system/memory/memory${MEM}/state
    #done
    
    
    #for MEM in ${MEM3[*]}
    #do
    #	echo "disabling mem unit $MEM"
    #	echo offline > /sys/devices/system/memory/memory${MEM}/state
    #done
sleep 0.5

if [ $SHOW_MIGRATION -eq 1 ]; then
	echo -n Topo Change: 
	cat /proc/vmstat | grep numa_pages_migrated
	date +"%T.%3N"
	bash /root/numa_scripts/migration_progress.sh &
fi

echo "Topo change: kill -USR1 pidof ld-linux-x86-64.so.2"
#kill -USR1 `pidof ld-linux-x86-64.so.2`
kill -SIGUSR1 `pidof ld-linux-x86-64.so.2`

#echo "Topo change: kill -USR1 pidof streamcluster"
#kill -USR1 `pidof streamcluster`

