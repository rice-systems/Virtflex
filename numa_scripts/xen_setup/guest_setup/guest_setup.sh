#!/bin/bash

sudo apt-get install vim git htop numactl

# allow cpu hotplug automatic enable
cp /root/numa_scripts/xen_setup/guest_setup/cpu-online.rules /etc/udev/rules.d
sudo udevadm control --reload-rules && udevadm trigger

# allow ssh from Dom0
read -p "Do you want to vim /etc/ssh/sshd_config and change PermitRootLogin to yes (y/n)?" n1
if [ $n1 == "y" ]; then
	vim /etc/ssh/sshd_config
fi
service sshd restart
