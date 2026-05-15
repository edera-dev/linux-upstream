obj-m := xlate_batch.o
KDIR  := /lib/modules/$(shell uname -r)/build

ccflags-y += -Wno-error

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod xlate_batch.ko
unload:
	echo 0 | sudo tee /sys/kernel/livepatch/xlate_batch/enabled
	sleep 1
	sudo rmmod xlate_batch
