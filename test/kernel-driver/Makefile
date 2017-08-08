ifneq ($(KERNELRELEASE),)
#kbuildsyntax. dependency relationshsip of files and target modules arelisted here.
#mymodule-objs:= file1.o file2.o
#obj-m:= migrate-x86.o

EXTRA_CFLAGS += -I/home/miaohy/Project/memif/memifx86/libsmm
migrate-x86-k-objs := migqueue.o migrate-x86.o
obj-m := migrate-x86-k.o

else
PWD := $(shell pwd)
KVER:= $(shell uname -r)
KDIR:= /lib/modules/$(KVER)/build 

all:
	$(MAKE) -C $(KDIR) M=$(PWD) 
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif

