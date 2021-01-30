# use xen-create-image to craete a PV guest
# make sure there is free space at the volume group
# xen-create-image is in the xen-tools package (apt-get install xentools)

# what does xen-create-image cover
	# Create logical volume for rootfs
	# Create logical volume for swap
	# Create filesystem for rootfs
	# Mount rootfs
	# Install operating system using debootstrap (or rinse etc, only debootstrap covered here)
	# Run a series of scripts to generate guest config files like fstab/inittab/menu.lst
	# Create a VM config file for the guest
	# Generate a root password for the guest system
	# Unmount the guest filesystem

read -p "Do you want to install lvm2 and debootstrap? (y/N):" n1
if [ $n1 == "y" ]; then
    sudo apt-get -y install lvm2 debootstrap
fi

read -p "Do you want to create physical volumn on a device? (y/N):" n1
if [ $n1 == "y" ]; then
    lsblk
    read -p "Enter device path (e.g. /dev/sda) :" n1
    sudo pvcreate $n1
    read -p "Do you want to create a volumn group (vg0) on $n1? (y/N):" n2
    if [ $n2 == "y" ]; then
        vgcreate vg0 $n1
    fi
else
    read -n1 "If you alread have vg0 created, please make sure /dev/vg0/ubu-pv0-disk is not mounted"
fi

read -p "Do you have trouble setting up the perl environment (y/n)? :" n1
if [ $n1 == "y" ]; then
	wget https://cpan.metacpan.org/authors/id/C/CA/CAPOEIRAB/File-Slurp-9999.27.tar.gz
	tar -xzf File-Slurp-9999.27.tar.gz
	cd File-Slurp-9999.27
	perl Makefile.PL
	sudo make && sudo make install
	cd ..

	wget http://search.cpan.org/CPAN/authors/id/P/PE/PEREINAR/File-Which-0.05.tar.gz
  	tar -xzf File-Which-0.05.tar.gz
  	cd File-Which-0.05
  	perl Makefile.PL
  	sudo make && sudo make install
	cd ..

	wget https://cpan.metacpan.org/authors/id/S/SO/SONNEN/Data-Validate-URI-0.07.tar.gz
  	tar -xzf Data-Validate-URI-0.07.tar.gz
  	cd Data-Validate-URI-0.07/
  	perl Makefile.PL
  	sudo make && sudo make install
	cd ..

	wget https://cpan.metacpan.org/authors/id/D/DR/DROLSKY/Data-Validate-Domain-0.14.tar.gz
  	tar -xvf Data-Validate-Domain-0.14.tar.gz
  	cd Data-Validate-Domain-0.14/
  	perl Makefile.PL
  	sudo make && sudo make install
	cd ..

  	wget https://cpan.metacpan.org/authors/id/A/AL/ALEXP/Net-Domain-TLD-1.75.tar.gz
  	tar -xvf Net-Domain-TLD-1.75.tar.gz
  	cd Net-Domain-TLD-1.75/
  	perl Makefile.PL
  	sudo make && sudo make install
	cd ..

  	wget https://cpan.metacpan.org/authors/id/D/DR/DROLSKY/Data-Validate-IP-0.27.tar.gz
  	tar -xf Data-Validate-IP-0.27.tar.gz
  	cd Data-Validate-IP-0.27/
  	perl Makefile.PL; sudo make && sudo make install
	cd ..

  	wget https://cpan.metacpan.org/authors/id/M/MI/MIKER/NetAddr-IP-4.079.tar.gz
  	tar -xf NetAddr-IP-4.079.tar.gz
  	cd NetAddr-IP-4.079
  	perl Makefile.PL; sudo make && sudo make install
	cd ..

  	wget https://cpan.metacpan.org/authors/id/B/BI/BINGOS/Term-UI-0.46.tar.gz
  	tar -xf Term-UI-0.46.tar.gz
  	cd Term-UI-0.46
  	perl Makefile.PL; sudo make && sudo make install
	cd ..

  	wget https://cpan.metacpan.org/authors/id/B/BI/BINGOS/Log-Message-Simple-0.10.tar.gz
  	tar -xf Log-Message-Simple-0.10.tar.gz
  	cd Log-Message-Simple-0.10/
  	perl Makefile.PL; sudo make && sudo make install
	cd ..

  	wget https://cpan.metacpan.org/authors/id/B/BI/BINGOS/Log-Message-0.08.tar.gz
  	tar -xf Log-Message-0.08.tar.gz
  	cd Log-Message-0.08/
  	perl Makefile.PL; sudo make && sudo make install
	cd ..

  	wget https://cpan.metacpan.org/authors/id/N/NE/NEILB/Sort-Versions-1.62.tar.gz
  	tar -xf Sort-Versions-1.62.tar.gz
  	cd Sort-Versions-1.62
  	perl Makefile.PL; sudo make && sudo make install
	cd ..

  	wget https://cpan.metacpan.org/authors/id/M/MS/MSCHOUT/Text-Template-1.55.tar.gz
  	tar -xf Text-Template-1.55.tar.gz
  	cd Text-Template-1.55
  	perl Makefile.PL; sudo make && sudo make install
	cd ..

fi


sudo xen-create-image \
	--hostname=ubu-pv1 \
	--vcpus=6 \
	--memory=16384mb \
	--lvm=ubuntu-vg \
	--size=50Gb \
	--ip=10.0.0.4 \
	--netmask=255.255.255.0 \
	--gateway=10.0.0.1 \
	--broadcast=10.0.0.255 \
	--nameserver=8.8.8.8 8.8.4.4 \
	--bridge=xenbr0 \
	--genpass=1 \
	--kernel=/boot/vmlinuz-`uname -r` \
	--verbose \
	--pygrub \
	--mirror=http://archive.ubuntu.com/ubuntu/ \
	--dist=xenial
	#--force \
	#--install-method=copy \
	#--dhcp \
	#--install-source=/home/runhua/linux_kernel_numa \
