# which xen 4.11 should be installed
SOURCE=0
# 0 --> original xen-4.11.0 without any change
# 1 --> xen-4.11.0-numa the modified version
# 2 --> xen-4.11-tmp temperary repo for test
INSTALL_TOOL=1
# 0 --> do not install xen-tools (xen-create-image) from source
# 1 --> install xen-tools (xen-create-image) from source


# install dependence packages
sudo apt-get --assume-yes install build-essential
sudo apt-get --assume-yes install bcc bin86 gawk bridge-utils iproute2 libcurl4 libcurl4-openssl-dev bzip2 module-init-tools transfig tgif
sudo apt-get --assume-yes install texinfo texlive-latex-base texlive-latex-recommended texlive-fonts-extra texlive-fonts-recommended pciutils-dev mercurial
sudo apt-get --assume-yes install make gcc libc6-dev zlib1g-dev python python-dev python-twisted libncurses5-dev patch libvncserver-dev libsdl-dev libjpeg-dev
sudo apt-get --assume-yes install libnl-route-3-200 libnl-3-dev libnl-cli-3-dev libnl-genl-3-dev libnl-route-3-dev
sudo apt-get --assume-yes install iasl libbz2-dev e2fslibs-dev git-core uuid-dev ocaml ocaml-findlib libx11-dev bison flex xz-utils libyajl-dev
sudo apt-get --assume-yes install gettext libpixman-1-dev libaio-dev markdown pandoc

sudo apt-get --assume-yes install libc6-dev-i386
sudo apt-get --assume-yes install lzma lzma-dev liblzma-dev
sudo apt-get --assume-yes install libsystemd-dev


if [ $SOURCE -eq 0 ]; then

	cd
	wget https://downloads.xenproject.org/release/xen/4.11.0/xen-4.11.0.tar.gz
	tar -xvf xen-4.11.0.tar.gz
	cd xen-4.11.0
	read -p "Do you want to apply patch to xen-4.11 (y/n):" n1
	if [ $n1 == "y" ]; then
    		patch -p0 < /root/numa_scripts/xen_setup/xen_bug_fix.patch
	fi

elif [ $SOURCE -eq 1 ]; then

	# clone customized xen repo (based on xen 4.11.0) from git
	cd ~
	git clone git@bitbucket.org:atom-zju/xen-4.11.0-numa.git
	cd xen-4.11.0-numa

else
	cd ~
	git clone git@bitbucket.org:atom-zju/xen-4.11-tmp.git
	cd xen-4.11-tmp
fi


./autogen.sh
./configure --libdir=/usr/lib
sudo make -j 48 dist # make xen and tool and other stuff
sudo make -j 48 install
# sudo make unistall # uninstall xen

# update grub
sudo update-grub

if [ $INSTALL_TOOL -eq 1 ]; then
	# install xen-tools from source
	cd ~
	git clone https://github.com/xen-tools/xen-tools.git
	cd xen-tools
	sudo make install
fi

echo "Installation is finished if no error occurs, don't forget to edit /etc/default/grub line GRUB_DEFAULT to be 'Ubuntu GNU/Linux, with Xen hypervisor', GRUB_CMDLINE_XEN_DEFAULT='dom0_mem=30720M,max:30720M' and update-grub"
