echo "=========set up linux kernel==========="

cd
git clone https://atom-zju@bitbucket.org/atom-zju/linux_kernel_numa.git linux-4.18.10

sudo apt-get update

sudo apt-get install git fakeroot build-essential ncurses-dev xz-utils libssl-dev bc flex libelf-dev bison

#tar xzf linux-4.18.10-nexttouch.tar.gz


cd linux-4.18.10

cp /boot/config-$(uname -r) .config

make -j24

make modules_install

sudo make install

sudo update-initramfs -c -k 4.18.10

sudo update-grub
