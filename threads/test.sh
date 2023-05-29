make clean
make
cd build/
clear
# 원하는 test case
make tests/threads/alarm-negative.result
make tests/threads/alarm-priority.result
make tests/threads/alarm-simultaneous.result
make tests/threads/alarm-zero.result
make tests/threads/alarm-single.result
make tests/threads/alarm-multiple.result
# test_alarm_simultaneous;
# test_alarm_priority;
# test_alarm_zero;
# test_alarm_negative;
# make tests/threads/priority-donate-one.result

pintos -- -q run alarm-multiple


cd ..