# Download Install ISO.
# http://www.ubuntu.com/download/desktop

# choose your VG
sudo pvs

# Create a LV
sudo lvcreate -L 4G -n ubuntu-hvm /dev/<VG>
# Create a guest config file /etc/xen/ubuntu-hvm.cfg

#  builder = "hvm"
#  name = "ubuntu-hvm"
#  memory = "512"
#  vcpus = 1
#  vif = ['']
#  disk = ['phy:/dev/<VG>/ubuntu-hvm,hda,w','file:/root/ubuntu-12.04-desktop-amd64.iso,hdc:cdrom,r']
#  vnc = 1
#  boot="dc"

xl create /etc/xen/ubuntu-hvm.cfg
vncviewer localhost:0 

# After the install you can optionally remove the CDROM from the config and/or change the boot order.

# For example /etc/xen/ubuntu-hvm.cfg:


#  builder = "hvm"
#  name = "ubuntu-hvm"
#  memory = "512"
#  vcpus = 1
#  vif = ['']
#  #disk = ['phy:/dev/<VG>/ubuntu-hvm,hda,w','file:/root/ubuntu-12.04-server-amd64.iso,hdc:cdrom,r']
#  disk = ['phy:/dev/<VG>/ubuntu-hvm,hda,w']
#  vnc = 1
#  boot="c"
#  #boot="dc"
