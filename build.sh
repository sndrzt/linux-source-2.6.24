#!/usr/bin/env bash

# apt-cache search linux-source
# sudo apt-get install -y linux-source-2.6.24
# cp /usr/src/linux-source-2.6.24.tar.bz2 .
# tar xf linux-source-2.6.24.tar.bz2

sudo apt-get install -y vim openssh-server git tig build-essential libncurses5-dev
TERM=linux
make menuconfig

date | tee a.txt
make bzImage
make modules
date | tee b.txt

sudo make modules_install
sudo make install
sudo mkinitramfs -o /boot/initrd.img-2.6.24.6 2.6.24.6

# vim /boot/grub/menu.lst

