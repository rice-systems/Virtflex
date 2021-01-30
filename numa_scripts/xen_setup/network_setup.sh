# This script is used to set up NAT between xen and guests
# you will create a virtual private network and use iptable to connect
# the private network with your internet interface

# show bridge config: brctl show
# add a bridge
sudo brctl addbr xenbr0

# edit /etc/network/interfaces to config xenbr0 to a virtual network
# following is one example of xenbr0 config, this interface doesn't
# have to have any connect with the internet interface yet
#         auto xenbr0
#         iface xenbr0 inet static
#             address 10.0.0.1
#             broadcast 10.0.0.255
#             netmask 255.255.255.0

printf \
"auto xenbr0 \n\
iface xenbr0 inet static \n\
    address 10.0.0.1 \n\
    broadcast 10.0.0.255 \n\
    netmask 255.255.255.0\n"

read -p "Do you want to paste lines above to /etc/network/interfaces?(y/n) :" n1
if [ $n1 == "y" ]; then
    vim /etc/network/interfaces
fi

# enable forwarding
# sudo echo 1 > /proc/sys/net/ipv4/ip_forward (not working, see below)
# edit file /etc/sysctl.conf and uncomment the following line
# net.ipv4.ip_forward = 1

read -p "Do you want to edit /etc/sysctl.conf to set net.ipv4.ip_forward = 1? (y/n) :" n1
if [ $n1 == "y" ]; then
    vim /etc/sysctl.conf
fi

read -p "Do you want to change the defualt internet interface name (eno1) (y/n) :" n1
if [ $n1 == "y" ]; then
    read -p "Enter interface name:" n2
    ENO=$n2
else
    ENO="eno1"
fi

# setup forwarding rules with iptables, we want eno1 ---> xenbr0
# change eno1 to whatever interface you have for internet connection
# first rule: accept any traffic from xenbr0
sudo iptables -A FORWARD --in-interface xenbr0 -j ACCEPT
# second rule: forward packets to the internet interface eno1
sudo iptables --table nat -A POSTROUTING --out-interface $ENO -j MASQUERADE

# apply the configuration
# service iptables restart (not working)
sudo ifup -a

# when create guest with xen-image-create, specify bridge=xenbr0, ip=10.0.0.2, gateway=10.0.0.1, nameserver=8.8.8.8 8.8.4.4 (google's DNS server)
