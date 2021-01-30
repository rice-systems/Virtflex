echo "==============set up NPB============"
sudo apt-get update
sudo apt-get install g++ gfortran linux-tools-common linux-tools-generic wget git unzip cmake gdb

cd
wget https://www.nas.nasa.gov/assets/npb/NPB3.3.1.tar.gz
#wget http://parsec.cs.princeton.edu/download/3.0/parsec-3.0.tar.gz
tar -xzf NPB3.3.1.tar.gz 
#tar -xzf parsec-3.0.tar.gz 
#sudo apt-get update
#sudo apt-get install numactl
#sudo apt-get install gfortran
#sudo apt-get install g++-4.8
#sudo apt-get install linux-tools-common linux-tools-generic linux-tools-`uname -r`
#sudo apt-get install git
#sudo apt-get install unzip
#sudo apt-get install cmake

#git clone https://github.com/numactl/numactl.git
#sudo apt-get install autoconf
#sudo apt-get install libtool
#cd numactl/
#./autogen.sh
#./configure
#make
#make install
#cd

cd NPB3.3.1/NPB3.3-OMP/
cd config/
cp /root/numa_scripts/NPB/make.def .
#echo -n "Press any key, you'll enter NPB3.3-OMP/config/make.def"
#echo "Change fortran compiler to f95, add -mcmodel=medium -fopenmp -g to both fortran and C compiler"
#read -n1
#vim make.def
cd ..

# cp /root/numa_scripts/NPB/mg.f MG/
# cp /root/numa_scripts/NPB/cg.f CG/

make MG CLASS=D
make CG CLASS=C
make LU CLASS=C
make IS CLASS=D
make UA CLASS=C
make IS CLASS=D

#scp rz18@ssh.clear.rice.edu:/storage-home/r/rz18/machine_backup/NPB3.3-OMP/*.sh .
cp /root/numa_scripts/NPB/*.sh .

#cd
#cd parsec-3.0/
#cd pkgs/apps/facesim/
#cd src/
#echo "Press any key, you'll enter facesim/src/Makefile, add CXX=g++-4.8"
#read -n1
#vim Makefile
#
#cd
#cd parsec-3.0/
#
#bin/parsecmgmt -a build -p streamcluster -c gcc-pthreads
#bin/parsecmgmt -a build -p fluidanimate -c gcc-pthreads
#bin/parsecmgmt -a build -p facesim -c gcc-pthreads
#
#scp rz18@ssh.clear.rice.edu:/storage-home/r/rz18/machine_backup/parsec-3.0/clean_unistall_n_rebuild_pkg.sh .
#scp rz18@ssh.clear.rice.edu:/storage-home/r/rz18/machine_backup/parsec-3.0/run*.sh .
#
#cd
#scp rz18@ssh.clear.rice.edu:/storage-home/r/rz18/machine_backup/disable_nodes.sh .

echo "======     Installing hpctoolkit   ======"
echo "======Compiling hpctoolkit-externals======"
echo "press any key to proceed:"
read -n1

git clone https://github.com/HPCToolkit/hpctoolkit.git
git clone https://github.com/HPCToolkit/hpctoolkit-externals.git


   cd hpctoolkit-externals
   ./configure
   make all
   #make distclean
cd 
echo "======Compiling hpctoolkit======"
echo "press any key to proceed:"
read -n1

   cd hpctoolkit
   ./configure --with-externals=/root/hpctoolkit-externals
   make -j
   make install

