/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
/* Common header for virtio camera driver. */

#ifndef _VIRTIO_CAMERA_H
#define _VIRTIO_CAMERA_H

#include "include/uapi/linux/virtio_camera.h"
#include <media/videobuf2-v4l2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>

#define DRIVER_NAME "virtio-camera"

#define VIRTIO_ID_CAMERA 43

/* Arbitrarily chosen sRGB format commonly used in USB webcams. */
#define VIRTIO_CAMERA_DEFAULT_CS V4L2_COLORSPACE_SRGB
#define VIRTIO_CAMERA_DEFAULT_XFER_F V4L2_XFER_FUNC_SRGB

#define file2stream(file) \
	container_of(file->private_data, struct viocam_fh, fh)->stream

struct viocam_msg {
	struct completion responded;
	refcount_t refcnt;
	struct device *dev;

	size_t req_size;
	size_t resp_size;
	void *req;
	void *resp;
};

struct viocam_buffer {
	struct vb2_v4l2_buffer vb;
	uint32_t resource_id;

	/* For asynchronous resource_queue cmd handling. */
	struct virtio_camera_resource_queue req;
	struct virtio_camera_resource_queue_resp resp;
	struct viocam_msg msg;
};

struct viocam_cmd_queue {
	struct virtqueue *vq;
	spinlock_t qlock;
};

struct viocam_event_queue {
	struct virtqueue *vq;
};

struct viocam_v4l2_format {
	__u32 pixelformat;
	size_t num_frames;
	struct v4l2_frmsize_stepwise *frames;
};

struct viocam_stream {
	struct v4l2_pix_format_mplane curr_fmt;

	struct viocam_v4l2_format *formats;
	unsigned int nformats;

	struct video_device video_dev;
	struct vb2_queue vb_q;
	struct mutex video_dev_mutex;

	uint32_t min_buf_needed;
};

struct viocam_fh {
	struct v4l2_fh fh;
	struct viocam_stream *stream;
};

struct viocam_device {
	struct viocam_cmd_queue cmd_vq;
	struct viocam_event_queue event_vq;

	struct v4l2_device v4l2_dev;

	struct viocam_stream stream;
	char name[32];

	struct {
		struct v4l2_ctrl_handler hdl;
		struct {
			struct v4l2_ctrl *auto_exposure;
			struct v4l2_ctrl *exposure;
		};
	} ctrls;

	bool has_iommu;
	bool mplane_api;

	struct ida resource_ida;
};

/* virtio_camera_vq.c */

void viocam_cmd_cb(struct virtqueue *vq);
void viocam_event_cb(struct virtqueue *vq);
int virtio_camera_cmd_get_camera_info(struct viocam_device *vcd,
				      uint32_t *ctrl_info_len,
				      uint32_t *min_bufs_needed);
int virtio_camera_cmd_enum_camera_ctrls(struct viocam_device *vcd,
					uint32_t resp_len);
int virtio_camera_cmd_stream_create(struct viocam_device *vcd,
				    uint32_t *fmt_info_len);
int virtio_camera_cmd_get_stream_formats(
	struct viocam_device *vcd, uint32_t fmt_info_len,
	struct v4l2_pix_format_mplane *curr_fmt,
	struct viocam_v4l2_format **formats, unsigned int *nformats);
int virtio_camera_cmd_query_control(struct viocam_device *vcd,
				    uint32_t next_id);
int virtio_camera_cmd_set_control(struct viocam_device *vcd, uint32_t ctrl_id,
				  uint32_t ctrl_val);
int virtio_camera_cmd_set_format(struct viocam_device *vcd,
				 struct v4l2_pix_format_mplane *fmt);
int virtio_camera_cmd_resource_create(struct viocam_device *vcd,
				      struct viocam_buffer *buf,
				      uint32_t resource_id);
int virtio_camera_cmd_resource_queue(struct viocam_device *vcd,
				     struct viocam_buffer *buf);
int virtio_camera_cmd_resource_destroy(struct viocam_device *vcd);
int virtio_camera_cmd_resource_flush(struct viocam_device *vcd);

/* virtio_camera_v4l2.c */

extern const struct v4l2_ioctl_ops viocam_ioctl_ops;
extern const struct v4l2_file_operations viocam_fops;

int viocam_vb2_queue_init(struct viocam_device *vcd,
			  struct viocam_stream *stream,
			  uint32_t min_bufs_needed);
void viocam_vb2_buf_queue_done(struct viocam_buffer *buf,
			       enum vb2_buffer_state state);

#endif /* _VIRTIO_CAMERA_H */
