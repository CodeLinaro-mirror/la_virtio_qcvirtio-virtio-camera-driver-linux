#
# Makefile for mod coqos
#
KDIR ?= $(KERNEL_SRC)
ifeq ($(KDIR),)
$(error "KDIR must be specified.")
endif

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

default:
	$(MAKE) -C $(KDIR) M=$(M)

modules_install:
	$(MAKE) -C $(KDIR) M=$(M) $@

clean:
	$(MAKE) -C $(KDIR) M=$(M) $@
