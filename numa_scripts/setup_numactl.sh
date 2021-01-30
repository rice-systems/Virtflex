echo "============set up numactl==========="

cd
git clone https://github.com/numactl/numactl.git

sudo apt-get update
sudo apt-get install git fakeroot build-essential ncurses-dev xz-utils libssl-dev bc flex libelf-dev bison
sudo apt-get install autoconf libtool

cd numactl/
cp /home/runhua/numa_scripts/numaif.h .
./autogen.sh
./configure --prefix=/usr/

sudo make
sudo make install
echo 0 > /proc/sys/kernel/numa_balancing
