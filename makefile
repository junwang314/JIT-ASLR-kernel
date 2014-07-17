KERNEL_DIR=linux-3.12.17

all:
	(cd $(KERNEL_DIR); make -j3; make headers_install)

install:
	(cd $(KERNEL_DIR); make modules_install; make install)

install-ubuntu:
	rm -rf /boot/*3.12.17+*
	rm -rf /lib/modules/*3.12.17+*
	(cd $(KERNEL_DIR); make modules_install; make install)
	update-initramfs -u -k 3.12.17+
	update-grub
