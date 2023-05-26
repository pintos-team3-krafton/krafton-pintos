make clean
make
cd build/
clear
# 원하는 test case
# make tests/threads/alarm-negative.result
make tests/threads/alarm-priority.result
# make tests/threads/alarm-simultaneous.result
# make tests/threads/alarm-wait.result
# make tests/threads/alarm-zero.result
# make tests/threads/priority-donate-one.result
cd ..