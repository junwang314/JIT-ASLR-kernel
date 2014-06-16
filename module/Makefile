PWD         := $(shell pwd) 
KVERSION    := $(shell uname -r)
KERNEL_DIR   = ../linux-3.12.17
NOSTDINC_FLAGS += -nostdinc

MODULE_NAME  = mem11
obj-m       := $(MODULE_NAME).o   

all:
	make -C $(KERNEL_DIR) M=$(PWD) modules
clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean
