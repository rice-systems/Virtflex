BARE_METAL=1
CPU_ACT=0

if [ $BARE_METAL -eq 1 ]; then
	echo $CPU_ACT > /sys/devices/system/cpu/cpu7/online
	echo $CPU_ACT > /sys/devices/system/cpu/cpu39/online
	echo $CPU_ACT > /sys/devices/system/cpu/cpu15/online 
	echo $CPU_ACT > /sys/devices/system/cpu/cpu47/online 
	echo $CPU_ACT > /sys/devices/system/cpu/cpu23/online
	echo $CPU_ACT > /sys/devices/system/cpu/cpu55/online
	echo $CPU_ACT > /sys/devices/system/cpu/cpu31/online
	echo $CPU_ACT > /sys/devices/system/cpu/cpu63/online
else
	xl vcpu-pin 0 all  7,39,15,47,23,55,31,63  7,39,15,47,23,55,31,63
	xl vcpu-list 0
fi
