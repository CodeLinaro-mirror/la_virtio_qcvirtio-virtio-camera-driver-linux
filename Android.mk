# SPDX-License-Identifier: GPL-2.0-only

# Build virtio_camera.ko

LOCAL_PATH := $(call my-dir)

KBUILD_OPTIONS := MODNAME=virtio_camera
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)

# VIRTIO_CAMERA_ROOT needs to be a absolute since it will be used
# for header files. $(TOP) cannot be used here since it will be
# resolved as "." which won't work for Kbuild.
KBUILD_OPTIONS += VIRTIO_CAMERA_ROOT=$(abspath $(LOCAL_PATH))

DLKM_DIR   := device/qcom/common/dlkm

include $(CLEAR_VARS)
# For incremental compilation
LOCAL_SRC_FILES           := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := virtio_camera.ko
LOCAL_MODULE_KBUILD_NAME  := virtio_camera.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

include $(DLKM_DIR)/Build_external_kernelmodule.mk

