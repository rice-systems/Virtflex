#!/bin/bash

# change the target for individual nodes
# usage: ./change_node_target <domID> <node#> <target>

if [ $# -ne 3 ]
then
	echo "usage: ./change_node_target <domID> <node#> <target>"
else
	sudo xenstore-write /local/domain/$1/memory/node/node_target$2 $3
	echo "xenstore-write /local/domain/$1/memory/node/node_target$2 $3"
fi
