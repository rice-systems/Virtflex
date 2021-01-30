echo "===========set up gcc============="

cd
git clone https://atom-zju@bitbucket.org/atom-zju/gcc_openmp_numa.git gcc-7.3.0

sudo apt-get install curl

cd gcc-7.3.0
contrib/download_prerequisites

cd
mkdir gcc_7.3.0_install
mkdir gcc_7.3.0_build && cd gcc_7.3.0_build

../gcc-7.3.0/configure -v --build=x86_64-linux-gnu --host=x86_64-linux-gnu --target=x86_64-linux-gnu --prefix=/root/gcc_7.3.0_install --enable-checking=release --enable-languages=c,c++,fortran --disable-multilib --program-suffix=-7.3

echo "Change makefile in gcc_7.3.0_build line 547 548, add -lnuma and change -O2"
read -n1

make -j8
make install
