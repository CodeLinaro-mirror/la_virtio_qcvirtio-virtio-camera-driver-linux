#
# Kbuild for the virtio-camera driver
#

# During development, to turn-off treating warning as error, pass
# `CFLAGS_MODULE=-Wno-error` to make as an argument or environment variable.
ccflags-y += -Werror

virtio_camera-y := \
	virtio_camera_driver.o \
	virtio_camera_vq.o \
	virtio_camera_v4l2.o \

obj-m += virtio_camera.o
