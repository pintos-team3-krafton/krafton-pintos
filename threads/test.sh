make clean
make
cd build/
clear
# 원하는 test case
# make tests/threads/alarm-negative.result
# make tests/threads/alarm-priority.result
# make tests/threads/alarm-simultaneous.result
# make tests/threads/alarm-zero.result
# make tests/threads/alarm-single.result
# make tests/threads/alarm-multiple.result

# make tests/threads/priority-change.result
# make tests/threads/priority-donate-one.result
# make tests/threads/priority-donate-multiple.result
# make tests/threads/priority-donate-multiple2.result
# make tests/threads/priority-donate-nest.result
# make tests/threads/priority-donate-sema.result
# make tests/threads/priority-donate-lower.result
# make tests/threads/priority-donate-chain.result
# make tests/threads/priority-fifo.result
# make tests/threads/priority-preempt.result
make tests/threads/priority-sema.result
# make tests/threads/priority-condvar.result

# pintos -- -q run alarm-multiple


cd ..