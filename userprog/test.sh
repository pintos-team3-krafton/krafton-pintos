make clean
make
cd build/
clear

pintos --fs-disk=10 -p tests/userprog/args-single:args-single -- -q -f run 'args-single onearg'

cd ..