
obj-m += sch_qfq.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=`pwd`
