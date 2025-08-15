#
# Kbuild for the virtio-camera driver
#

# Compose absolute source path to the module source directory.
# This is needed if passed M variable is a relative path as the current working
# directory may be different than kernel source tree. E.g if custom kernel
# output path is configured.
override src := $(if $(patsubst /%,,$(src)),$(srctree)/$(src),$(src))

ccflags-y := -I$(src)/include/uapi

# In case of external kernel module build within Android's kernel/build system
# the previous line doesn't work. Hence, specify headers location in other way
ccflags-y += -I$(VIRTIO_CAMERA_ROOT)/include/uapi

# During development, to turn-off treating warning as error, pass
# `CFLAGS_MODULE=-Wno-error` to make as an argument or environment variable.
ccflags-y += -Werror

virtio_camera-y := \
	virtio_camera_driver.o \
	virtio_camera_device.o \
	virtio_camera_vq.o \
	virtio_camera_v4l2.o \

obj-m += virtio_camera.o
