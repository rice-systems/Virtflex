#!/bin/bash
git config --global user.email zrh4223616@hotmail.com
git config --global user.name atom-zju
bash recompile.sh
bash setup_numactl.sh
bash compile_gcc.sh
bash setup_hpl.sh
bash setup.sh
