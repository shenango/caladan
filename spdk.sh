patch -p 1 -d spdk < spdk-completion-availability.patch
cd spdk
git submodule update --init
scripts/pkgdep.sh
./configure
make
sudo scripts/setup.sh
