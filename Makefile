KERNEL_SRC ?= /lib/modules/`uname -r`/build
KDIR ?= $(KERNEL_SRC)

# The variable "M" is used to point to the location of this module, and it is
# passed to kbuild to build this module.
#
# Allow to specify variable "M" from outside. It is needed to set it to the
# relative path to this module. It must be relative to the kernel source
# directory.
#
# If kernel is built with "O" option then setting "M" to the relative path
# empowers the build system to put output/object files (.o, .ko.) into a
# directory different from the module source directory.
M ?= $$PWD

# Kernel directory containing vhost.h (default is relative to KDIR)
VHOST_KDIR ?= $(KDIR)

default:
	$(MAKE) -C $(KDIR) M=$(M) VHOST_KDIR=$(VHOST_KDIR) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(M) INSTALL_MOD_STRIP=1 modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(M) clean
