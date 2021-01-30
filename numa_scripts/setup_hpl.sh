echo "========= set up GUPS benchmark =========="
cd
#sudo apt-get install mpich mpich-doc libblas-dev
#wget http://icl.cs.utk.edu/projectsfiles/hpcc/download/hpcc-1.5.0.tar.gz
#tar xf hpcc-1.5.0.tar.gz
#cd hpcc-1.5.0
#cp /root/numa_scripts/hpl/Make.Linux_ATHLON_CBLAS hpl/Make.Linux_ATHLON_CBLAS
#make arch=Linux_ATHLON_CBLAS
git clone https://github.com/bryantclc/MyRandomAccess.git
cd MyRandomAccess/omp
cp /root/numa_scripts/hpl/single_node_lcg.c .
cp /root/numa_scripts/hpl/Makefile .
make
