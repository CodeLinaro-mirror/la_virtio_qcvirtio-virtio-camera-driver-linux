#
# Kbuild for the virtio-camera driver
#

# Compose absolute source path to the module source directory.
# This is needed if passed M variable is a relative path as the current working
# directory may be different than kernel source tree. E.g if custom kernel
# output path is configured.
override src := $(if $(patsubst /%,,$(src)),$(srctree)/$(src),$(src))

ccflags-y := -I$(src)/include/uapi

# During development, to turn-off treating warning as error, pass
# `CFLAGS_MODULE=-Wno-error` to make as an argument or environment variable.
ccflags-y += -Werror

virtio_camera-y := \
	virtio_camera_driver.o \
	virtio_camera_device.o \
	virtio_camera_vq.o \
	virtio_camera_v4l2.o \

obj-m += virtio_camera.o
