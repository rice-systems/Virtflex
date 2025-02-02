#!/bin/bash

# get the DomID of guest
sudo xl list
read -p "Please enter guest DomID:" n1
DOMID=$n1

# totoal number of NUMA nodes for the guest
NUM_NODE=4
# node size in MB
#NODE_SIZE=2048
# node size for most applications
NODE_SIZE=4096
#NODE_SIZE=5120
#NODE_SIZE=6144
#NODE_SIZE=8192
# node size for CG C
#NODE_SIZE=1024
# node size for MG D
#NODE_SIZE=14336

# node that the action take effect on
declare -a NODES=(3 2 0 1)

# topo info to help make the decisions

# the target should be set for expansion
EXP_TARGET=$( expr 1024 '*' "$NODE_SIZE" )
# the target should be set for shrink
#SHR_TARGET=353600 # 150 MB
#SHR_TARGET=0 # 150 MB
#SHR_TARGET=253600 # 150 MB
SHR_TARGET=$( expr 80 '+' "$NODE_SIZE" '/' 1024 '*' 17 )

# cpu affnity array
#declare -a NODE_0=(1 3 5 7)
#declare -a NODE_1=(0 2 4 6)


# declare -a NODE_0=(0 1 2 3 4 5 6 28 29)
# declare -a NODE_1=(7 8 9 10 11 12 13 35 36)
# declare -a NODE_2=(14 15 16 17 18 19 20 42 43)
# declare -a NODE_3=(21 22 23 24 25 26 27 49 50)

declare -a NODE_0=(0 1 2 3 4 5 6 28 29 30 31 32 33 34)
declare -a NODE_1=(7 8 9 10 11 12 13 35 36 37 38 39 40 41)
declare -a NODE_2=(14 15 16 17 18 19 20 42 43 44 45 46 47 48)
declare -a NODE_3=(21 22 23 24 25 26 27 49 50 51 52 53 54 55)
#declare -a NODE_2=()
#declare -a NODE_3=()

# declare -a NODE_0=(0 4)
# declare -a NODE_1=(1 5)
# declare -a NODE_2=(2 6)
# declare -a NODE_3=(3 7)

current_idx=0
while true; do

# choose action, 1 is expand the node, -1 is shrink the node
read -p "Please enter action code(1 for expansion, -1 for shrinkage):" n1
ACTION=$n1
if [ $n1 != '1' ] && [ $n1 != '-1' ]; then
	echo "Bad action code..."
	exit
fi

if [ $ACTION -eq 1 ]; then
	CPU_ACTION=online
	TARGET=$EXP_TARGET
else
	CPU_ACTION=offline
	TARGET=$( expr 1024 '*' "$SHR_TARGET" )
fi

# sequence for adding node: add CPUs; write the topo_change flag; set memory target
# sequence for removing node: write the topo_change flag; set memory target; remove CPUs

if [ $ACTION -eq 1 ]; then # if adding resources
	
	if [ $current_idx -gt 0 ]
	then
		current_idx=$((current_idx-1))
	fi

	# 1. add CPUs
#	for NODE in ${NODES[*]}
#	do
#		# enable/disable CPU in the node
#		CPUS=NODE_$NODE[*]
#		for CPU in ${!CPUS}
#		do
#			CMD="sudo xenstore-write /local/domain/$DOMID/cpu/$CPU/availability $CPU_ACTION"
#			echo $CMD
#			$CMD
#		done
#	done
	
	#sudo xl vcpu-set $DOMID 56	
	#for NODE in ${NODES[*]}
	NODE=${NODES[$current_idx]}
	#do
		# enable/disable CPU in the node
		CPUS=NODE_$NODE[*]
		for CPU in ${!CPUS}
		do
			CMD="sudo xenstore-write /local/domain/$DOMID/cpu/$CPU/availability $CPU_ACTION"
			echo $CMD
			$CMD
		done
	#done
	
	# 3. set the topo_change flag
	CMD="sudo xenstore-write /local/domain/$DOMID/numa/topo_change 1"
        echo $CMD
        $CMD
	CMD="sudo xenstore-chmod /local/domain/$DOMID/numa/topo_change b"
        echo $CMD
        $CMD

	# 2. set target for memory	
	#for NODE in ${NODES[*]}
	#do
		# set the target to be
	       	CMD="sudo xenstore-write /local/domain/$DOMID/numa/node/$NODE/target $TARGET"
		echo $CMD
		$CMD
	#done
	
	
else # else if removing resources
	len=${#NODES[@]}
	len=$((len-1))
	if [ $current_idx -eq $len ]
	then
		current_idx=$((current_idx-1))
	fi
	
	# 3. removing CPUs
	NODE=${NODES[$current_idx]}
	#for NODE in ${NODES[*]}
	#do
		# enable/disable CPU in the node
		CPUS=NODE_$NODE[*]
		for CPU in ${!CPUS}
		do
			CMD="sudo xenstore-write /local/domain/$DOMID/cpu/$CPU/availability $CPU_ACTION"
			echo $CMD
			$CMD
		done
	#done

	#sleep 1
	
	# 2. set the topo_change flag
	CMD="sudo xenstore-write /local/domain/$DOMID/numa/topo_change 2"
        echo $CMD
        $CMD
	CMD="sudo xenstore-chmod /local/domain/$DOMID/numa/topo_change b"
        echo $CMD
        $CMD
	
	# 1. set target for memory	
	#for NODE in ${NODES[*]}
	#do
		# set the target to be
	       	CMD="sudo xenstore-write /local/domain/$DOMID/numa/node/$NODE/target $TARGET"
		echo $CMD
		$CMD
	#done
	

	# TODO wait for memory target change to complete
	TOPO_CHANGE=2
	#while [ $TOPO_CHANGE -eq 2 ]
	#do
	#	TOPO_CHANGE="$(sudo xenstore-read /local/domain/$DOMID/memory/topo_change)"
	#	echo "sudo xenstore-read /local/domain/$DOMID/numa/topo_change $TOPO_CHANGE"
	#	sleep 0.5
	#done
	len=${#NODES[@]}
	len=$((len-1))
	if [ $current_idx -lt $len ]
	then
		current_idx=$((current_idx+1))
	fi

fi

done

# # loop over all the nodes that take effects
# for NODE in ${NODES[*]}
# do
# 	# enable/disable CPU in the node
# 	CPUS=NODE_$NODE[*]
# 	for CPU in ${!CPUS}
# 	do
# 		CMD="sudo xenstore-write /local/domain/$DOMID/cpu/$CPU/availability $CPU_ACTION"
# 		echo $CMD
# 		$CMD
# 	done
# 	# set the target to be
#        	CMD="sudo xenstore-write /local/domain/$DOMID/memory/node/node_target$NODE $TARGET"
# 	echo $CMD
# 	$CMD
# done
