
obj-m += sch_qfq.o
#EXTRA_CFLAGS+=-DDEBUG

all:
	@echo -n 'WARNING: Make sure the header file include/linux/pkt_sched.h is '
	@echo 'copied to /lib/modules/$(shell uname -r)/build/include/linux'
	@echo -n 'WARNING: Make sure the header file include/net/sock.h is '
	@echo 'copied to /lib/modules/$(shell uname -r)/build/include/linux and '
	@echo 'the kernel is compiled against it'
	@#make -C /lib/modules/$(shell uname -r)/build M=`pwd` modules
	make -C /usr/src/linux-headers-$(shell uname -r) M=`pwd` modules

clean:
	@#make -C /lib/modules/$(shell uname -r)/build M=`pwd` clean
	make -C /usr/src/linux-headers-$(shell uname -r) M=`pwd` clean
