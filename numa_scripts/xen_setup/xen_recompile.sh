# install dependence packages
#sudo apt-get build-dep xen

# clone customized xen repo (based on xen 4.11.0) from git
cd ~
#git clone git@bitbucket.org:atom-zju/xen-4.11.0-numa.git
cd xen-4.11.0
#./autogen.sh
#./configure
sudo make -j 4 dist # make xen and tool and other stuff
sudo make install
# sudo make unistall # uninstall xen

# update grub
sudo update-grub

# install xen-tools from source
#cd ~
#git clone https://github.com/xen-tools/xen-tools.git
#cd xen-tools
#sudo make install
