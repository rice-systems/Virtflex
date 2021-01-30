# change passwd
# passwd

sudo apt-get update

#  vim numa git
sudo apt-get install vim numactl git

# enable ssh
sudo apt-get install openssh-server
# change PermitRootLogin line to PermitRootLogin yes
# sudo vim /etc/ssh/sshd_config
sudo service ssh start

# scp -r .ssh root@10.0.0.3:/root
cd
git clone git@bitbucket.org:atom-zju/linux_kernel_numa.git
git clone git@bitbucket.org:atom-zju/numa_scripts.git

