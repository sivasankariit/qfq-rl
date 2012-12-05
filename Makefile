
obj-m += sch_qfq.o
#EXTRA_CFLAGS+=-DDEBUG

all:
	@echo -n 'WARNING: Make sure the header file include/linux/pkt_sched.h is '
	@echo 'copied to /lib/modules/$(shell uname -r)/build/include/linux'
	make -C /lib/modules/$(shell uname -r)/build M=`pwd` modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=`pwd` clean
