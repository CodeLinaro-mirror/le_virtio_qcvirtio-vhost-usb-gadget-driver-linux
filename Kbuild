# During development, to turn-off treating warning as error, pass
# `CFLAGS_MODULE=-Wno-error` to make as an argument or environment variable.
ccflags-y += -Werror -Wno-implicit-fallthrough

ccflags-y += -I${VHOST_KDIR}/drivers/vhost

# Uncomment to enable pr_debug() output
#ccflags-y += -DDEBUG

obj-m := vhost_usb_gadget.o

vhost_usb_gadget-y := \
	vhost.o \
	usb_gadget.o
