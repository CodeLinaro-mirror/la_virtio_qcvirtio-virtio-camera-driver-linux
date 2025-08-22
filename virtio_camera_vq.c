/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "virtio_camera.h"
#include <media/v4l2-ctrls.h>

#define MAX_INLINE_CMD_SIZE 96
#define MAX_INLINE_RESP_SIZE 96
#define VBUFFER_SIZE                                                           \
	(sizeof(struct virtio_camera_vbuffer) + MAX_INLINE_CMD_SIZE +          \
	 MAX_INLINE_RESP_SIZE)

static void fmt_virtio_to_v4l2(struct virtio_camera_device *vcd,
			       struct v4l2_pix_format_mplane *pix_fmt,
			       const struct virtio_camera_stream_format *fmt);

static bool virtio_camera_vbuf_is_pending(struct virtio_camera_device *vcd,
					  struct virtio_camera_vbuffer *vbuf)
{
	struct virtio_camera_vbuffer *entry;

	list_for_each_entry (entry, &vcd->pending_vbuf_list, pending_list) {
		if (entry == vbuf)
			return true;
	}

	return false;
}

static void virtio_camera_free_vbuf(struct virtio_camera_device *vcd,
				    struct virtio_camera_vbuffer *vbuf)
{
	unsigned long flags;

	spin_lock_irqsave(&vcd->cmd_vq.qlock, flags);
	if (virtio_camera_vbuf_is_pending(vcd, vbuf))
		list_del(&vbuf->pending_list);

	spin_unlock_irqrestore(&vcd->cmd_vq.qlock, flags);

	kmem_cache_free(vcd->vbufs, vbuf);
}

static void detach_unused_vbufs(struct virtqueue *vq,
				struct list_head *detach_list)
{
	struct virtio_camera_vbuffer *vbuf;

	while ((vbuf = virtqueue_detach_unused_buf(vq)) != NULL) {
		list_add_tail(&vbuf->list, detach_list);
	}
}

static void virtio_camera_detach_vbufs(struct virtio_camera_device *vcd)
{
	struct list_head detach_list;
	struct virtio_camera_vbuffer *vbuf, *tmp_vbuf;

	INIT_LIST_HEAD(&detach_list);

	detach_unused_vbufs(vcd->cmd_vq.vq, &detach_list);

	if (list_empty(&detach_list))
		return;

	list_for_each_entry_safe (vbuf, tmp_vbuf, &detach_list, list) {
		list_del(&vbuf->list);
		virtio_camera_free_vbuf(vcd, vbuf);
	}
}

int virtio_camera_alloc_vbufs(struct virtio_camera_device *vcd)
{
	vcd->vbufs =
		kmem_cache_create("virtio-camera-vbufs", VBUFFER_SIZE,
				  __alignof__(struct virtio_camera_vbuffer), 0,
				  NULL);
	if (!vcd->vbufs)
		return -ENOMEM;

	return 0;
}

void virtio_camera_free_vbufs(struct virtio_camera_device *vcd)
{
	virtio_camera_detach_vbufs(vcd);
	kmem_cache_destroy(vcd->vbufs);
	vcd->vbufs = NULL;
}

static struct virtio_camera_vbuffer *
virtio_camera_get_vbuf(struct virtio_camera_device *vcd, uint32_t size,
		       void *resp_buf, uint32_t resp_size,
		       virtio_camera_resp_cb resp_cb)
{
	struct virtio_camera_vbuffer *vbuf;

	vbuf = kmem_cache_alloc(vcd->vbufs, GFP_KERNEL);
	if (!vbuf)
		return ERR_PTR(-ENOMEM);

	memset(vbuf, 0, VBUFFER_SIZE);

	BUG_ON(size > MAX_INLINE_CMD_SIZE);
	vbuf->buf = (void *)vbuf + sizeof(*vbuf);
	vbuf->size = size;

	vbuf->resp_cb = resp_cb;
	vbuf->resp_size = resp_size;
	if (resp_size <= MAX_INLINE_RESP_SIZE && !resp_buf)
		vbuf->resp_buf = (void *)vbuf->buf + size;
	else
		vbuf->resp_buf = resp_buf;

	BUG_ON(!vbuf->resp_buf);

	return vbuf;
}

static void *
virtio_camera_alloc_req_resp(struct virtio_camera_device *vcd,
			     struct virtio_camera_vbuffer **vbuffer_p,
			     uint32_t req_size, void *resp_buf,
			     uint32_t resp_size, virtio_camera_resp_cb cb)
{
	struct virtio_camera_vbuffer *vbuf;

	vbuf = virtio_camera_get_vbuf(vcd, req_size, resp_buf, resp_size, cb);
	if (IS_ERR(vbuf)) {
		*vbuffer_p = NULL;
		return ERR_CAST(vbuf);
	}

	*vbuffer_p = vbuf;
	return vbuf->buf;
}

static int virtio_camera_queue_cmd_buffer(struct virtio_camera_device *vcd,
					  struct virtio_camera_vbuffer *vbuf)
{
	unsigned long flags;
	struct virtqueue *vq = vcd->cmd_vq.vq;
	struct scatterlist *sgs[3], vreq, vout, vresp;
	int outcnt = 0, incnt = 0;
	int ret;

	spin_lock_irqsave(&vcd->cmd_vq.qlock, flags);
	list_add_tail(&vbuf->pending_list, &vcd->pending_vbuf_list);

	sg_init_one(&vreq, vbuf->buf, vbuf->size);
	sgs[outcnt + incnt] = &vreq;
	outcnt++;

	if (vbuf->data_size) {
		sg_init_one(&vout, vbuf->data_buf, vbuf->data_size);
		sgs[outcnt + incnt] = &vout;
		outcnt++;
	}

	if (vbuf->resp_size) {
		sg_init_one(&vresp, vbuf->resp_buf, vbuf->resp_size);
		sgs[outcnt + incnt] = &vresp;
		incnt++;
	}

retry:
	ret = virtqueue_add_sgs(vq, sgs, outcnt, incnt, vbuf, GFP_ATOMIC);
	if (ret == -ENOSPC) {
		spin_unlock_irqrestore(&vcd->cmd_vq.qlock, flags);
		wait_event(vcd->cmd_vq.reclaim_queue, vq->num_free);
		spin_lock_irqsave(&vcd->cmd_vq.qlock, flags);
		goto retry;
	} else {
		virtqueue_kick(vq);
	}

	spin_unlock_irqrestore(&vcd->cmd_vq.qlock, flags);

	return ret;
}

static int
virtio_camera_queue_cmd_buffer_sync(struct virtio_camera_device *vcd,
				    struct virtio_camera_vbuffer *vbuf)
{
	int ret;
	unsigned long rem;

	vbuf->is_sync = true;
	init_completion(&vbuf->reclaimed);

	ret = virtio_camera_queue_cmd_buffer(vcd, vbuf);
	if (ret)
		return ret;

	rem = wait_for_completion_timeout(&vbuf->reclaimed, 5 * HZ);
	if (rem == 0)
		ret = -ETIMEDOUT;

	return ret;
}

void virtio_camera_cmd_cb(struct virtqueue *vq)
{
	struct virtio_camera_device *vcd = vq->vdev->priv;
	struct virtio_camera_vbuffer *vbuf;
	unsigned long flags;
	unsigned int len;

	spin_lock_irqsave(&vcd->cmd_vq.qlock, flags);
	do {
		virtqueue_disable_cb(vq);
		while ((vbuf = virtqueue_get_buf(vq, &len))) {
			if (!virtio_camera_vbuf_is_pending(vcd, vbuf))
				continue;

			if (vbuf->resp_cb)
				vbuf->resp_cb(vcd, vbuf);

			if (vbuf->is_sync)
				complete(&vbuf->reclaimed);
			else {
				if (virtio_camera_vbuf_is_pending(vcd, vbuf))
					list_del(&vbuf->pending_list);

				kmem_cache_free(vcd->vbufs, vbuf);
			}
		}
		if (unlikely(virtqueue_is_broken(vq)))
			break;
	} while (!virtqueue_enable_cb(vq));
	spin_unlock_irqrestore(&vcd->cmd_vq.qlock, flags);

	wake_up(&vcd->cmd_vq.reclaim_queue);
}

static void virtio_camera_queue_event(struct virtio_camera_device *vcd,
				      struct virtio_camera_event *event)
{
	struct scatterlist sg;
	struct virtio_camera_event_queue *event_vq = &vcd->event_vq;
	int ret;

	sg_init_one(&sg, event, sizeof(*event));
	ret = virtqueue_add_inbuf(event_vq->vq, &sg, 1, event, GFP_ATOMIC);

	if (ret) {
		v4l2_err(&vcd->v4l2_dev, "failed to queue event");
		return;
	}

	virtqueue_kick(event_vq->vq);
}

int virtio_camera_alloc_events(struct virtio_camera_device *vcd)
{
	struct virtio_camera_event *events;
	unsigned int num = vcd->event_vq.vq->num_free;
	int i;

	events = kcalloc(num, sizeof(*events), GFP_KERNEL);
	if (!events)
		return -ENOMEM;

	for (i = 0; i < num; ++i) {
		virtio_camera_queue_event(vcd, events + i);
	}

	vcd->events = events;

	return 0;
}

void virtio_camera_free_events(struct virtio_camera_device *vcd)
{
	while (virtqueue_detach_unused_buf(vcd->event_vq.vq)) {
		;
	}

	if (vcd->events)
		kfree(vcd->events);
}

static void virtio_camera_handle_event(struct virtio_camera_event *event)
{
	(void)event;
}

void virtio_camera_process_events(struct work_struct *work)
{
	struct virtio_camera_device *vcd = container_of(
		work, struct virtio_camera_device, event_vq.reclaim_work);
	struct virtio_camera_event_queue *event_vq = &vcd->event_vq;
	struct virtio_camera_event *event;
	unsigned int len;

	while ((event = virtqueue_get_buf(event_vq->vq, &len))) {
		virtio_camera_handle_event(event);
		virtio_camera_queue_event(vcd, event);
	}
}

void virtio_camera_event_cb(struct virtqueue *vq)
{
	struct virtio_camera_device *vcd = vq->vdev->priv;

	schedule_work(&vcd->event_vq.reclaim_work);
}

/* VIRTIO_CAMERA_CMD_GET_CAMERA_INFO */
static void virtio_camera_get_camera_info_cb(
	struct virtio_camera_device *vcd,
	struct virtio_camera_get_camera_info_resp *resp)
{
	struct virtio_camera_stream *stream;
	int i;

	for (i = 0; i < le32_to_cpu(resp->stream_cnt); ++i) {
		stream = kzalloc(sizeof(*stream), GFP_KERNEL);
		stream->stream_id = i;
		stream->video_dev = &vcd->video_dev;
		list_add_tail(&stream->list, &vcd->camera_sensor.streams_list);
	}

	vcd->ctrl_info_len = resp->ctrl_info_len;

	vcd->camera_sensor.min_buf_needed = le32_to_cpu(resp->min_buf_needed);
}

static int virtio_camera_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct virtio_camera_device *vcd = container_of(
		ctrl->handler, struct virtio_camera_device, ctrls.hdl);
	//TODO: implement multistream support
	struct virtio_camera_stream *stream =
		list_first_entry_or_null(&vcd->camera_sensor.streams_list,
					 struct virtio_camera_stream, list);
	int ret = 0;
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE_AUTO:
		if (ctrl->is_new) {
			ret = virtio_camera_cmd_set_control(
				vcd, stream->stream_id, ctrl->id, ctrl->val);
			if (ret)
				return ret;
		}
		if (ctrl->val == V4L2_EXPOSURE_MANUAL) {
			ret = virtio_camera_cmd_set_control(
				vcd, stream->stream_id, vcd->ctrls.exposure->id,
				vcd->ctrls.exposure->val);
		}
		return ret;
	default:
		return virtio_camera_cmd_set_control(vcd, stream->stream_id,
						     ctrl->id, ctrl->val);
	}
}

static const struct v4l2_ctrl_ops ctrl_ops = {
	.s_ctrl = virtio_camera_set_ctrl,
};

int virtio_camera_cmd_enum_camera_ctrls(struct virtio_camera_device *vcd)
{
	struct virtio_camera_enum_camera_ctrls *req_p;
	struct virtio_camera_enum_camera_ctrls_resp *resp;
	struct virtio_camera_vbuffer *vbuf;
	int ret;
	size_t resp_len = vcd->ctrl_info_len;
	unsigned int i, nctrl_info;
	struct v4l2_ctrl *v4l2_ctrl, *next_v4l2_ctrl;

	resp = kzalloc(resp_len, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	req_p = virtio_camera_alloc_req_resp(vcd, &vbuf, sizeof(*req_p), resp,
					     resp_len, NULL);
	if (IS_ERR(req_p)) {
		ret = PTR_ERR(req_p);
		goto error;
	}
	req_p->cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_ENUM_CAMERA_CTRLS);

	ret = virtio_camera_queue_cmd_buffer_sync(vcd, vbuf);
	if (ret == -ETIMEDOUT) {
		v4l2_err(&vcd->v4l2_dev, "timed out waiting for camera ctrls");
		goto error_buffer_sync;
	} else if (le32_to_cpu(resp->result) != VIRTIO_CAMERA_RESULT_OK) {
		v4l2_err(&vcd->v4l2_dev, "failed to enum camera ctrls");
		ret = -EINVAL;
		goto error_buffer_sync;
	}

	nctrl_info = le32_to_cpu(resp->num_ctrl_entries);

	v4l2_ctrl_handler_init(&vcd->ctrls.hdl, nctrl_info);

	for (i = 0; i < nctrl_info; ++i) {
		struct v4l2_ctrl_config cfg = {0};
		struct virtio_camera_ctrl_entry *ctrl_entry =
			(struct virtio_camera_ctrl_entry
				 *)((char *)resp +
				    offsetof(
					    struct virtio_camera_enum_camera_ctrls_resp,
					    ctrl_entries[i]));

		char *name = kstrndup(ctrl_entry->name,
				      sizeof(ctrl_entry->name), GFP_KERNEL);
		if (name == NULL) {
			ret = -ENOMEM;
			goto error_ctrl_hdl;
		}

		cfg.id = le32_to_cpu(ctrl_entry->ctrl_id);
		cfg.type = le32_to_cpu(ctrl_entry->type);
		cfg.min = le32_to_cpu(ctrl_entry->min);
		cfg.max = le32_to_cpu(ctrl_entry->max);
		cfg.step = le32_to_cpu(ctrl_entry->step);
		cfg.def = le32_to_cpu(ctrl_entry->def);
		cfg.name = name;
		cfg.ops = &ctrl_ops;

		v4l2_ctrl = v4l2_ctrl_new_custom(&vcd->ctrls.hdl, &cfg, name);
		if (v4l2_ctrl == NULL) {
			ret = vcd->ctrls.hdl.error;
			goto error_ctrl_hdl;
		}

		if (cfg.id == V4L2_CID_EXPOSURE_AUTO) {
			vcd->ctrls.auto_exposure = v4l2_ctrl;
		} else if (cfg.id == V4L2_CID_EXPOSURE_ABSOLUTE) {
			vcd->ctrls.exposure = v4l2_ctrl;
		}
	}

	if (vcd->ctrls.auto_exposure) {
		v4l2_ctrl_auto_cluster(2, &vcd->ctrls.auto_exposure,
				       V4L2_EXPOSURE_MANUAL, false);
	}

	vcd->v4l2_dev.ctrl_handler = &vcd->ctrls.hdl;
	v4l2_ctrl_handler_setup(vcd->v4l2_dev.ctrl_handler);

	virtio_camera_free_vbuf(vcd, vbuf);

	return 0;

error_ctrl_hdl:
	list_for_each_entry_safe (v4l2_ctrl, next_v4l2_ctrl,
				  &vcd->ctrls.hdl.ctrls, node) {
		kfree(v4l2_ctrl->priv);
	}
	v4l2_ctrl_handler_free(&vcd->ctrls.hdl);
error_buffer_sync:
	virtio_camera_free_vbuf(vcd, vbuf);
error:
	return ret;
}

int virtio_camera_cmd_get_camera_info(struct virtio_camera_device *vcd)
{
	struct virtio_camera_get_camera_info *req_p;
	struct virtio_camera_get_camera_info_resp *resp;
	struct virtio_camera_vbuffer *vbuf;
	int ret;
	size_t resp_len = sizeof(*resp);

	resp = kzalloc(resp_len, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	req_p = virtio_camera_alloc_req_resp(vcd, &vbuf, sizeof(*req_p), resp,
					     resp_len, NULL);
	if (IS_ERR(req_p)) {
		ret = PTR_ERR(req_p);
		goto error;
	}

	req_p->cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_GET_CAMERA_INFO);

	ret = virtio_camera_queue_cmd_buffer_sync(vcd, vbuf);
	if (ret == -ETIMEDOUT) {
		v4l2_err(&vcd->v4l2_dev,
			 "timed out waiting for get camera info");
		goto error_buffer_sync;
	} else if (le32_to_cpu(resp->result) != VIRTIO_CAMERA_RESULT_OK ||
		   le32_to_cpu(resp->stream_cnt) <= 0) {
		v4l2_err(&vcd->v4l2_dev, "failed to get camera info");
		ret = -EINVAL;
		goto error_buffer_sync;
	}

	virtio_camera_get_camera_info_cb(vcd, resp);

error_buffer_sync:
	virtio_camera_free_vbuf(vcd, vbuf);
error:
	kfree(resp);
	return ret;
}

static void virtio_camera_free_fmt(struct virtio_camera_stream *stream)
{
	unsigned int i;
	for (i = 0; i < stream->nformats; i++) {
		kfree(stream->formats[i].frames);
	}
	kfree(stream->formats);
}

static void
virtio_camera_copy_fmt_range(struct virtio_camera_format_range *d_rge,
			     struct virtio_camera_format_range *s_rge)
{
	d_rge->min = le32_to_cpu(s_rge->min);
	d_rge->max = le32_to_cpu(s_rge->max);
	d_rge->step = le32_to_cpu(s_rge->step);
}

static size_t virtio_camera_parse_virtio_frame(struct camera_format_frame *frm,
					       void *buf)
{
	struct virtio_camera_stream_format_frame *virtio_frame = buf;
	struct virtio_camera_stream_format_frame *frame = &frm->frame;

	virtio_camera_copy_fmt_range(&frame->width, &virtio_frame->width);
	virtio_camera_copy_fmt_range(&frame->height, &virtio_frame->height);

	return sizeof(struct virtio_camera_stream_format_frame);
}

static size_t
virtio_camera_parse_virtio_fmt(struct virtio_camera_device *vcd,
			       struct virtio_camera_format_info *fmt, void *buf)
{
	struct virtio_camera_stream_format_entry *virtio_fmt_desc;
	struct virtio_camera_stream_format_entry *fmt_desc;
	struct camera_format_frame *frame;
	size_t idx, offset, extra_size;

	virtio_fmt_desc = buf;
	fmt_desc = &fmt->desc;

	fmt_desc->format = le32_to_cpu(virtio_fmt_desc->format);
	fmt_desc->num_frames = le32_to_cpu(virtio_fmt_desc->num_frames);

	if (fmt_desc->num_frames == 0) {
		v4l2_err(&vcd->v4l2_dev, "backend provided zero framesizes\n");
		return 0;
	}

	fmt->frames = kcalloc(fmt_desc->num_frames,
			      sizeof(struct camera_format_frame), GFP_KERNEL);
	if (fmt->frames == NULL) {
		v4l2_err(&vcd->v4l2_dev, "Failed to allocate frame array\n");
		return 0;
	}

	offset = sizeof(struct virtio_camera_stream_format_entry);
	for (idx = 0; idx < fmt_desc->num_frames; idx++) {
		frame = &fmt->frames[idx];
		extra_size =
			virtio_camera_parse_virtio_frame(frame, buf + offset);
		if (extra_size == 0) {
			kfree(fmt->frames);
			return 0;
		}
		offset += extra_size;
	}

	return offset;
}

/* VIRTIO_CAMERA_CMD_GET_STREAM_FORMATS */
static int virtio_camera_cmd_get_stream_formats_done(
	struct virtio_camera_device *vcd, struct virtio_camera_stream *stream,
	struct virtio_camera_get_stream_formats_resp *resp)
{
	struct virtio_camera_format_info *fmt;
	uint32_t fmt_count;
	int fmt_idx;
	size_t offset;
	int ret;

	fmt_count = le32_to_cpu(resp->num_entries);
	if (!fmt_count) {
		v4l2_err(
			&vcd->v4l2_dev,
			"backend provided a stream without any supported formats\n");
		return -EINVAL;
	}

	stream->formats = kcalloc(fmt_count, sizeof(*stream->formats), GFP_KERNEL);
	if (!stream->formats) {
		return -ENOMEM;
	}
	stream->nformats = fmt_count;

	offset = sizeof(struct virtio_camera_get_stream_formats_resp);
	for (fmt_idx = 0; fmt_idx < fmt_count; fmt_idx++) {
		size_t fmt_size = 0;

		fmt = &stream->formats[fmt_idx];

		fmt_size = virtio_camera_parse_virtio_fmt(
			vcd, fmt, (char *)(resp) + offset);
		if (fmt_size == 0) {
			v4l2_err(&vcd->v4l2_dev, "failed to parse fmt\n");
			ret = -ENOENT;
			goto parse_fmt_err;
		}
		offset += fmt_size;
	}

	fmt_virtio_to_v4l2(vcd, &stream->pix_format, &resp->stream_fmt);

	/* TODO: Implement colorspace adjustment according to backend.
	 * For now, we set it to a default one to keep v4l2-compliance happy.
	 */
	stream->pix_format.colorspace = VIRTIO_CAMERA_DEFAULT_CS;
	stream->pix_format.xfer_func = VIRTIO_CAMERA_DEFAULT_XFER_F;
	stream->pix_format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	stream->pix_format.quantization = V4L2_QUANTIZATION_DEFAULT;

	return 0;

parse_fmt_err:
	virtio_camera_free_fmt(stream);
	return ret;
}

/* VIRTIO_CAMERA_CMD_STREAM_CREATE */
int virtio_camera_cmd_stream_create(struct virtio_camera_device *vcd,
				    struct virtio_camera_stream *stream)
{
	struct virtio_camera_stream_create *req_p;
	struct virtio_camera_stream_create_resp *resp;
	struct virtio_camera_vbuffer *vbuf;
	int ret;
	size_t resp_len = sizeof(*resp);

	resp = kzalloc(resp_len, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	req_p = virtio_camera_alloc_req_resp(vcd, &vbuf, sizeof(*req_p), resp,
					     resp_len, NULL);
	if (IS_ERR(req_p)) {
		ret = PTR_ERR(req_p);
		goto error;
	}

	req_p->cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_STREAM_CREATE);
	req_p->stream_id = cpu_to_le32(stream->stream_id);

	ret = virtio_camera_queue_cmd_buffer_sync(vcd, vbuf);
	if (ret == -ETIMEDOUT) {
		v4l2_err(&vcd->v4l2_dev,
			 "timed out waiting for get stream formats");
		goto error_buffer_sync;
	} else if (le32_to_cpu(resp->result) != VIRTIO_CAMERA_RESULT_OK) {
		ret = -EINVAL;
		goto error_buffer_sync;
	}

	stream->fmt_info_len = resp->fmt_info_len;

error_buffer_sync:
	virtio_camera_free_vbuf(vcd, vbuf);
error:
	kfree(resp);
	return ret;
}

int virtio_camera_cmd_get_stream_formats(struct virtio_camera_device *vcd,
					 struct virtio_camera_stream *stream)
{
	struct virtio_camera_get_stream_formats *req_p;
	struct virtio_camera_get_stream_formats_resp *resp;
	struct virtio_camera_vbuffer *vbuf;
	int ret;
	size_t resp_len = stream->fmt_info_len;

	resp = kzalloc(resp_len, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	req_p = virtio_camera_alloc_req_resp(vcd, &vbuf, sizeof(*req_p), resp,
					     resp_len, NULL);
	if (IS_ERR(req_p)) {
		ret = PTR_ERR(req_p);
		goto error;
	}

	req_p->cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_GET_STREAM_FORMATS);
	req_p->stream_id = cpu_to_le32(stream->stream_id);

	ret = virtio_camera_queue_cmd_buffer_sync(vcd, vbuf);
	if (ret == -ETIMEDOUT) {
		v4l2_err(&vcd->v4l2_dev,
			 "timed out waiting for get stream formats");
		goto error_buffer_sync;
	} else if (le32_to_cpu(resp->result) != VIRTIO_CAMERA_RESULT_OK ||
		   le32_to_cpu(resp->num_entries) <= 0 ||
		   le32_to_cpu(resp->stream_fmt.num_planes) <= 0 ||
		   le32_to_cpu(resp->stream_fmt.num_planes) >
			   VIRTIO_CAMERA_MAX_PLANES) {
		ret = -EINVAL;
		goto error_buffer_sync;
	}

	ret = virtio_camera_cmd_get_stream_formats_done(vcd, stream, resp);

error_buffer_sync:
	virtio_camera_free_vbuf(vcd, vbuf);
error:
	kfree(resp);
	return ret;
}

/* VIRTIO_CAMERA_CMD_RESOURCE_CREATE */
int virtio_camera_cmd_resource_create(struct virtio_camera_device *vcd,
				      struct virtio_camera_stream *stream,
				      void *buf, size_t buf_size,
				      uint32_t resource_id)
{
	struct virtio_camera_resource_create *req_p;
	struct virtio_camera_resource_create_resp *resp;
	struct virtio_camera_vbuffer *vbuf;
	int ret;

	req_p = virtio_camera_alloc_req_resp(vcd, &vbuf, sizeof(*req_p), NULL,
					     sizeof(*resp), NULL);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_RESOURCE_CREATE);
	req_p->stream_id = cpu_to_le32(stream->stream_id);
	req_p->resource_id = cpu_to_le32(resource_id);

	vbuf->data_buf = buf;
	vbuf->data_size = buf_size;

	resp = (struct virtio_camera_resource_create_resp *)vbuf->resp_buf;

	ret = virtio_camera_queue_cmd_buffer_sync(vcd, vbuf);
	if (ret == -ETIMEDOUT)
		v4l2_err(&vcd->v4l2_dev,
			 "timed out waiting for resource create");
	else if (le32_to_cpu(resp->result) != VIRTIO_CAMERA_RESULT_OK) {
		v4l2_err(&vcd->v4l2_dev,
			 "failed to create resource for the stream");
		ret = -EINVAL;
	}

	virtio_camera_free_vbuf(vcd, vbuf);
	return ret;
}

/* VIRTIO_CAMERA_CMD_RESOURCE_QUEUE */
void virtio_camera_cmd_resource_queue_cb(struct virtio_camera_device *vcd,
					 struct virtio_camera_vbuffer *vbuf)
{
	struct virtio_camera_resource_queue_resp *resp =
		(struct virtio_camera_resource_queue_resp *)vbuf->resp_buf;
	struct virtio_camera_buffer *vb_buf =
		(struct virtio_camera_buffer *)vbuf->priv;

	virtio_camera_vb2_buf_queue_done(vb_buf, le32_to_cpu(resp->status),
					 le64_to_cpu(resp->timestamp));
}

int virtio_camera_cmd_resource_queue(struct virtio_camera_device *vcd,
				     struct virtio_camera_stream *stream,
				     struct virtio_camera_buffer *vb_buf)
{
	struct virtio_camera_resource_queue *req_p;
	struct virtio_camera_vbuffer *vbuf;

	req_p = virtio_camera_alloc_req_resp(
		vcd, &vbuf, sizeof(*req_p), NULL,
		sizeof(struct virtio_camera_resource_queue_resp),
		virtio_camera_cmd_resource_queue_cb);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_RESOURCE_QUEUE);
	req_p->stream_id = cpu_to_le32(stream->stream_id);
	req_p->resource_id = cpu_to_le32(vb_buf->resource_id);

	vbuf->priv = vb_buf;

	return virtio_camera_queue_cmd_buffer(vcd, vbuf);
}

/* VIRTIO_CAMERA_CMD_RESOURCE_DESTROY */
int virtio_camera_cmd_resource_destroy(struct virtio_camera_device *vcd,
				       struct virtio_camera_stream *stream)
{
	struct virtio_camera_resource_destroy *req_p;
	struct virtio_camera_resource_destroy_resp *resp;
	struct virtio_camera_vbuffer *vbuf;
	int ret;

	req_p = virtio_camera_alloc_req_resp(vcd, &vbuf, sizeof(*req_p), NULL,
					     sizeof(*resp), NULL);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_RESOURCE_DESTROY);
	req_p->stream_id = cpu_to_le32(stream->stream_id);

	resp = (struct virtio_camera_resource_destroy_resp *)vbuf->resp_buf;

	ret = virtio_camera_queue_cmd_buffer_sync(vcd, vbuf);
	if (ret == -ETIMEDOUT)
		v4l2_err(&vcd->v4l2_dev,
			 "timed out waiting for resource destroy");
	else if (le32_to_cpu(resp->result) != VIRTIO_CAMERA_RESULT_OK) {
		v4l2_err(&vcd->v4l2_dev,
			 "failed to destroy resources for the stream");
		ret = -EINVAL;
	}

	virtio_camera_free_vbuf(vcd, vbuf);
	return ret;
}

/* VIRTIO_CAMERA_CMD_SET_CONTROL */
int virtio_camera_cmd_set_control(struct virtio_camera_device *vcd,
				  uint32_t stream_id, uint32_t ctrl_id,
				  uint32_t ctrl_val)
{
	struct virtio_camera_set_control *req_p;
	struct virtio_camera_set_control_resp *resp;
	struct virtio_camera_vbuffer *vbuf;
	int ret;

	req_p = virtio_camera_alloc_req_resp(vcd, &vbuf, sizeof(*req_p), NULL,
					     sizeof(*resp), NULL);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_SET_CONTROL);
	req_p->stream_id = cpu_to_le32(stream_id);
	req_p->ctrl_id = cpu_to_le32(ctrl_id);
	req_p->value = cpu_to_le32(ctrl_val);

	resp = (struct virtio_camera_set_control_resp *)vbuf->resp_buf;

	ret = virtio_camera_queue_cmd_buffer_sync(vcd, vbuf);
	if (ret == -ETIMEDOUT)
		v4l2_err(&vcd->v4l2_dev, "timed out waiting for set control");
	else if (cpu_to_le32(resp->result) != VIRTIO_CAMERA_RESULT_OK) {
		v4l2_err(&vcd->v4l2_dev, "failed to set control");
		ret = -EINVAL;
	}

	virtio_camera_free_vbuf(vcd, vbuf);
	return ret;
}

static void fmt_virtio_to_v4l2(struct virtio_camera_device *vcd,
			       struct v4l2_pix_format_mplane *pix_fmt,
			       const struct virtio_camera_stream_format *fmt)
{
	uint8_t i;

	pix_fmt->pixelformat = le32_to_cpu(fmt->format);
	pix_fmt->width = le32_to_cpu(fmt->width);
	pix_fmt->height = le32_to_cpu(fmt->height);
	pix_fmt->field = V4L2_FIELD_NONE;
	pix_fmt->num_planes = le32_to_cpu(fmt->num_planes);
	if (pix_fmt->num_planes > VIDEO_MAX_PLANES) {
		v4l2_warn(&vcd->v4l2_dev, "plane cnt too big, cap to %d\n",
			  VIDEO_MAX_PLANES);
		pix_fmt->num_planes = VIDEO_MAX_PLANES;
	}

	memset(pix_fmt->reserved, 0, sizeof(pix_fmt->reserved));

	for (i = 0; i < pix_fmt->num_planes; ++i) {
		pix_fmt->plane_fmt[i].bytesperline =
			le32_to_cpu(fmt->plane_fmt[i].bytesperline);
		pix_fmt->plane_fmt[i].sizeimage =
			PAGE_ALIGN(le32_to_cpu(fmt->plane_fmt[i].sizeimage));
		memset(pix_fmt->plane_fmt[i].reserved, 0,
		       sizeof(pix_fmt->plane_fmt[i].reserved));
	}
}

/* VIRTIO_CAMERA_CMD_GET_FORMAT */
int virtio_camera_cmd_get_format(struct virtio_camera_device *vcd,
				 struct virtio_camera_stream *stream)
{
	struct virtio_camera_get_format *req_p;
	struct virtio_camera_get_format_resp *resp;
	struct virtio_camera_vbuffer *vbuf;
	int ret;

	req_p = virtio_camera_alloc_req_resp(vcd, &vbuf, sizeof(*req_p), NULL,
					     sizeof(*resp), NULL);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_GET_FORMAT);
	req_p->stream_id = cpu_to_le32(stream->stream_id);

	resp = (struct virtio_camera_get_format_resp *)vbuf->resp_buf;

	ret = virtio_camera_queue_cmd_buffer_sync(vcd, vbuf);
	if (ret == -ETIMEDOUT)
		v4l2_err(&vcd->v4l2_dev, "timed out waiting for get format");
	else if (le32_to_cpu(resp->result) != VIRTIO_CAMERA_RESULT_OK) {
		v4l2_err(&vcd->v4l2_dev, "failed to get format");
		ret = -EINVAL;
	} else {
		fmt_virtio_to_v4l2(vcd, &stream->pix_format, &resp->fmt);
	}

	virtio_camera_free_vbuf(vcd, vbuf);
	return ret;
}

/* VIRTIO_CAMERA_CMD_SET_FORMAT */
int virtio_camera_cmd_set_format(struct virtio_camera_device *vcd,
				 struct virtio_camera_stream *stream)
{
	struct virtio_camera_set_format *req_p;
	struct virtio_camera_set_format_resp *resp;
	struct virtio_camera_vbuffer *vbuf;
	int ret;

	req_p = virtio_camera_alloc_req_resp(vcd, &vbuf, sizeof(*req_p), NULL,
					     sizeof(*resp), NULL);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_SET_FORMAT);
	req_p->fmt.format = cpu_to_le32(stream->pix_format.pixelformat);
	req_p->fmt.width = cpu_to_le32(stream->pix_format.width);
	req_p->fmt.height = cpu_to_le32(stream->pix_format.height);

	resp = (struct virtio_camera_set_format_resp *)vbuf->resp_buf;

	ret = virtio_camera_queue_cmd_buffer_sync(vcd, vbuf);
	if (ret == -ETIMEDOUT)
		v4l2_err(&vcd->v4l2_dev, "timed out waiting for set format");
	else if (le32_to_cpu(resp->result) != VIRTIO_CAMERA_RESULT_OK) {
		v4l2_err(&vcd->v4l2_dev, "failed to set format");
		ret = -EINVAL;
	} else {
		fmt_virtio_to_v4l2(vcd, &stream->pix_format, &resp->fmt);
	}

	virtio_camera_free_vbuf(vcd, vbuf);
	return ret;
}
