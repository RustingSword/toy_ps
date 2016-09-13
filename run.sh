make

# change NODE to actual hostname/ip, corresponding to ip specified in test.cpp

scp test NODE1:
scp test NODE2:
scp test NODE3:
scp test NODE4:

ssh NODE1 -t "./test 0" &
ssh NODE2 -t "./test 1" &
ssh NODE3 -t "./test 2" &
ssh NODE4 -t "./test 3" &
