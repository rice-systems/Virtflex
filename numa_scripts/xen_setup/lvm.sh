# pvcreate /dev/sdb # create physical volumne 
# pvs # list all the pvs

# vgcreate vg00 /dev/sdb /dev/sdc # create a volume group named vg00 on those two devices
# vgdisplay vg00 # display info about vg00

# lvcreate -n lv0 -L 10G vg00 # create an logical volume named lv0 with the size of 10G
# lvs # display logical volume info
