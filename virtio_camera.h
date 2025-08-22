/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
/* Common header for virtio camera driver. */

#ifndef _VIRTIO_CAMERA_H
#define _VIRTIO_CAMERA_H

#include <linux/list.h>
#include <linux/virtio.h>
#include <linux/virtio_camera.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>

#define DRIVER_NAME "virtio-camera"

#define VIRTIO_ID_CAMERA 43

/* Arbitrarily chosen sRGB format commonly used in USB webcams. */
#define VIRTIO_CAMERA_DEFAULT_CS V4L2_COLORSPACE_SRGB
#define VIRTIO_CAMERA_DEFAULT_XFER_F V4L2_XFER_FUNC_SRGB

#define file2stream(file)                                                      \
	container_of(file->private_data, struct virtio_camera_fh, fh)->stream

struct virtio_camera_device;
struct virtio_camera_vbuffer;

typedef void (*virtio_camera_resp_cb)(struct virtio_camera_device *vcd,
				      struct virtio_camera_vbuffer *vbuf);

struct virtio_camera_vbuffer {
	char *buf;
	uint32_t size;

	void *data_buf;
	uint32_t data_size;

	char *resp_buf;
	uint32_t resp_size;

	void *priv;
	virtio_camera_resp_cb resp_cb;

	bool is_sync;
	struct completion reclaimed;

	struct list_head list;
	struct list_head pending_list;
};

struct virtio_camera_cmd_queue {
	struct virtqueue *vq;
	spinlock_t qlock;
	wait_queue_head_t reclaim_queue;
};

struct virtio_camera_event_queue {
	struct virtqueue *vq;
	spinlock_t qlock;
	struct work_struct reclaim_work;
};

struct virtio_camera_sensor {
	uint32_t min_buf_needed;
	struct list_head streams_list;
};

struct camera_format_frame {
	struct virtio_camera_stream_format_frame frame;
	struct virtio_camera_format_range *frame_rates;
};

struct virtio_camera_format_info {
	struct virtio_camera_stream_format_entry desc;
	struct camera_format_frame *frames;
};

enum virtio_camera_stream_state {
	VIRTIO_CAMERA_STREAM_STATE_STOPPED,
	VIRTIO_CAMERA_STREAM_STATE_RUNNING,
};

struct virtio_camera_fh {
	struct v4l2_fh fh;
	struct virtio_camera_stream *stream;
};

struct virtio_camera_stream {
	uint32_t stream_id;

	uint32_t fmt_info_len;

	struct video_device *video_dev;

	struct v4l2_pix_format_mplane pix_format;

	struct virtio_camera_format_info *formats;
	unsigned int nformats;
	struct list_head list;
};

struct virtio_camera_buffer {
	struct vb2_v4l2_buffer vb;
	uint32_t resource_id;

	struct list_head list;
};

struct virtio_camera_device {
	struct virtio_device *vdev;
	struct virtio_camera_cmd_queue cmd_vq;
	struct virtio_camera_event_queue event_vq;

	struct kmem_cache *vbufs;
	struct list_head pending_vbuf_list;

	void *events;

	struct v4l2_device v4l2_dev;

	struct video_device video_dev;
	struct mutex video_dev_mutex;

	struct virtio_camera_sensor camera_sensor;
	uint32_t ctrl_info_len;
	char name[32];

	struct {
		struct v4l2_ctrl_handler hdl;
		struct {
			struct v4l2_ctrl *auto_exposure;
			struct v4l2_ctrl *exposure;
		};
	} ctrls;

	struct vb2_queue vb_q;
	struct list_head vbq_active;
	spinlock_t vbq_lock;

	bool has_iommu;
	bool mplane_api;

	struct idr resource_idr;
	spinlock_t resource_idr_lock;
};

/* virtio_camera_device.c */

int virtio_camera_device_init(struct virtio_camera_device *vcd);
int virtio_camera_device_deinit(struct virtio_camera_device *vcd);

/* virtio_camera_vq.c */

int virtio_camera_alloc_vbufs(struct virtio_camera_device *vcd);
void virtio_camera_free_vbufs(struct virtio_camera_device *vcd);

void virtio_camera_cmd_cb(struct virtqueue *vq);

int virtio_camera_alloc_events(struct virtio_camera_device *vcd);
void virtio_camera_free_events(struct virtio_camera_device *vcd);
void virtio_camera_process_events(struct work_struct *work);
void virtio_camera_event_cb(struct virtqueue *vq);

int virtio_camera_cmd_get_camera_info(struct virtio_camera_device *vcd);

int virtio_camera_cmd_enum_camera_ctrls(struct virtio_camera_device *vcd);

int virtio_camera_cmd_stream_create(struct virtio_camera_device *vcd,
				    struct virtio_camera_stream *stream);

int virtio_camera_cmd_get_stream_formats(struct virtio_camera_device *vcd,
					 struct virtio_camera_stream *stream);

int virtio_camera_cmd_query_control(struct virtio_camera_device *vcd,
				    uint32_t next_id);
int virtio_camera_cmd_set_control(struct virtio_camera_device *vcd,
				  uint32_t stream_id, uint32_t ctrl_id,
				  uint32_t ctrl_val);

int virtio_camera_cmd_get_format(struct virtio_camera_device *vcd,
				 struct virtio_camera_stream *stream);
int virtio_camera_cmd_set_format(struct virtio_camera_device *vcd,
				 struct virtio_camera_stream *stream);

int virtio_camera_cmd_enum_fmt_cap(struct virtio_camera_device *vcd,
				   struct virtio_camera_stream *stream,
				   uint32_t idx);

int virtio_camera_cmd_resource_create(struct virtio_camera_device *vcd,
				      struct virtio_camera_stream *stream,
				      void *buf, size_t buf_size,
				      uint32_t resource_id);
int virtio_camera_cmd_resource_queue(struct virtio_camera_device *vcd,
				     struct virtio_camera_stream *stream,
				     struct virtio_camera_buffer *vb_buf);
int virtio_camera_cmd_resource_destroy(struct virtio_camera_device *vcd,
				       struct virtio_camera_stream *stream);

/* virtio_camera_v4l2.c */

extern const struct v4l2_ioctl_ops virtio_camera_ioctl_ops;

int virtio_camera_vb2_queue_init(struct virtio_camera_device *vcd,
				 struct virtio_camera_stream *stream);
void virtio_camera_vb2_queue_deinit(struct virtio_camera_device *vcd);

void virtio_camera_vb2_buf_queue_done(
	struct virtio_camera_buffer *vb_buf,
	enum virtio_camera_resource_queue_status status, uint64_t timestamp);

#endif /* _VIRTIO_CAMERA_H */
