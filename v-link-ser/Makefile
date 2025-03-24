# Define the kernel source directory, typically found in /lib/modules/$(uname -r)/build
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
# Define the path to the kernel module source directory
SRC_DIR := $(shell pwd)

# Name of the kernel module
obj-m := v-link-ser.o

# Default target
all:
	$(MAKE) -C $(KERNEL_DIR) M=$(SRC_DIR) modules
	
# Install the module
install:
	$(MAKE) -C $(KERNEL_DIR) M=$(SRC_DIR) modules_install
	sudo depmod -a
# Clean the build directory
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(SRC_DIR) clean

# Uninstall the module
uninstall:
	$(MAKE) -C $(KERNEL_DIR) M=$(SRC_DIR) modules_remove

