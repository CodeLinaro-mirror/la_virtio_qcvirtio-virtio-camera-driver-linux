#SPDX-License-Identifier: GPL-2.0-only

# Build virtio-camera kernel driver

BOARD_VENDOR_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/virtio_camera.ko
# BOARD_VENDOR_RAMDISK_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/virtio_camera.ko
# BOARD_VENDOR_RAMDISK_RECOVERY_KERNEL_MODULES_LOAD += $(KERNEL_MODULES_OUT)/virtio_camera.ko
