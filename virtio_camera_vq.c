/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <media/v4l2-ctrls.h>
#include "media/videobuf2-dma-sg.h"
#include <linux/virtio.h>
#include <linux/types.h>

#include "virtio_camera.h"

static_assert(VIRTIO_CAMERA_MAX_PLANES <= VIDEO_MAX_PLANES);

#define VIOCAM_MSG_DEFINE_FULL(hdl, msg_type, cmd_id, req_s, resp_s) \
	struct {                                                     \
		struct viocam_msg *msg;                              \
		msg_type *req;                                       \
		msg_type##_resp(*resp);                              \
		unsigned int req_size;                               \
		unsigned int resp_size;                              \
		u32 cmd;                                             \
		const char *dbg_name;                                \
	} hdl = {                                                    \
		NULL, NULL, NULL, req_s, resp_s, cmd_id, #cmd_id,    \
	}

/* Convenience macros */
#define VIOCAM_MSG_DEFINE(hdl, msg_type, cmd_id)                        \
	VIOCAM_MSG_DEFINE_FULL(hdl, msg_type, cmd_id, sizeof(msg_type), \
			       sizeof(msg_type##_resp))

#define VIOCAM_MSG_DEFINE_WREQ(hdl, msg_type, cmd_id, req_s) \
	VIOCAM_MSG_DEFINE_FULL(hdl, msg_type, cmd_id, req_s, \
			       sizeof(msg_type##_resp))

#define VIOCAM_MSG_DEFINE_WRESP(hdl, msg_type, cmd_id, resp_s) \
	VIOCAM_MSG_DEFINE_FULL(hdl, msg_type, cmd_id, sizeof(msg_type), resp_s)

#define VIOCAM_MSG_INIT(hdl, vcd)                                          \
	({                                                                 \
		typeof(hdl) *_hdl = &(hdl);                                \
		_hdl->msg = viocam_msg_create((vcd), _hdl->req_size,       \
					      _hdl->resp_size, _hdl->cmd); \
		if (_hdl->msg) {                                           \
			_hdl->req = _hdl->msg->req;                        \
			_hdl->resp = _hdl->msg->resp;                      \
		}                                                          \
		_hdl->msg ? 0 : -ENOMEM;                                   \
	})

#define VIOCAM_MSG(hdl) ((hdl).msg)
#define VIOCAM_MSG_RESP(hdl) ((hdl).resp)
#define VIOCAM_MSG_REQ(hdl) ((hdl).req)
#define VIOCAM_DBG(hdl) ((hdl).dbg_name)

static void viocam_msg_release(struct viocam_msg *msg)
{
	struct device *dev;

	if (refcount_dec_and_test(&msg->refcnt)) {
		dev = msg->dev;
		devm_kfree(dev, msg->req);
		devm_kfree(dev, msg->resp);
		devm_kfree(dev, msg);
	}
}

static struct viocam_msg *viocam_msg_create(struct viocam_device *vcd,
					    size_t req_size, size_t resp_size,
					    u32 cmd_type)
{
	struct device *dev = vcd->v4l2_dev.dev;
	struct viocam_msg *msg;
	struct virtio_camera_cmd_hdr *req_head;

	msg = devm_kzalloc(dev, sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return NULL;

	msg->req = devm_kzalloc(dev, req_size, GFP_KERNEL);
	if (!msg->req)
		goto err_free_msg;

	msg->resp = devm_kzalloc(dev, resp_size, GFP_KERNEL);
	if (!msg->resp)
		goto err_free_msg_req;

	refcount_set(&msg->refcnt, 2);
	init_completion(&msg->responded);
	msg->dev = dev;
	msg->req_size = req_size;
	msg->resp_size = resp_size;
	req_head = msg->req;
	req_head->cmd_type = cpu_to_le32(cmd_type);

	return msg;

err_free_msg_req:
	devm_kfree(dev, msg->req);
err_free_msg:
	devm_kfree(dev, msg);
	return NULL;
}

static int viocam_queue_msg(struct viocam_device *vcd, struct viocam_msg *msg)
{
	unsigned long flags;
	struct virtqueue *vq = vcd->cmd_vq.vq;
	struct scatterlist out_sg, in_sg;
	struct scatterlist *sgs[] = { &out_sg, &in_sg };
	int ret;

	sg_init_one(&out_sg, msg->req, msg->req_size);
	sg_init_one(&in_sg, msg->resp, msg->resp_size);

	spin_lock_irqsave(&vcd->cmd_vq.qlock, flags);
	ret = virtqueue_add_sgs(vq, sgs, 1, 1, msg, GFP_ATOMIC);
	if (ret) {
		spin_unlock_irqrestore(&vcd->cmd_vq.qlock, flags);
		viocam_msg_release(msg); //virtqueue cb wouldn't do that
		return ret;
	}
	virtqueue_kick(vq);
	spin_unlock_irqrestore(&vcd->cmd_vq.qlock, flags);

	return ret;
}

static inline int viocam_queue_msg_and_wait(struct viocam_device *vcd,
					    struct viocam_msg *msg,
					    const char *dbg_name)
{
	int ret;
	unsigned long rem;
	struct virtio_camera_cmd_resp_hdr *resp_head = msg->resp;

	ret = viocam_queue_msg(vcd, msg);
	if (ret) {
		v4l2_err(&vcd->v4l2_dev, "%s cmd queue err=%d", dbg_name, ret);
		return ret;
	}

	rem = wait_for_completion_timeout(&msg->responded, 5 * HZ);
	if (rem == 0) {
		v4l2_err(&vcd->v4l2_dev, "virtio cmd=%s timeout", dbg_name);
		return -ETIMEDOUT;
	}

	switch (resp_head->result) {
	case VIRTIO_CAMERA_RESULT_OK:
		return 0;
	case VIRTIO_CAMERA_RESULT_ERR_INVALID_OPERATION:
	case VIRTIO_CAMERA_RESULT_ERR_INVALID_STREAM_ID:
	case VIRTIO_CAMERA_RESULT_ERR_INVALID_RESOURCE_ID:
		return -EINVAL;
	default:
		return -EIO;
	}
}

static int viocam_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct viocam_device *vcd =
		container_of(ctrl->handler, struct viocam_device, ctrls.hdl);
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE_AUTO:
		if (ctrl->is_new) {
			ret = virtio_camera_cmd_set_control(vcd, ctrl->id,
							    ctrl->val);
			if (ret)
				return ret;
		}
		if (ctrl->val == V4L2_EXPOSURE_MANUAL) {
			ret = virtio_camera_cmd_set_control(
				vcd, vcd->ctrls.exposure->id,
				vcd->ctrls.exposure->val);
		}
		return ret;
	default:
		return virtio_camera_cmd_set_control(vcd, ctrl->id, ctrl->val);
	}
}

static const struct v4l2_ctrl_ops ctrl_ops = {
	.s_ctrl = viocam_set_ctrl,
};

static int fmt_v4l2_to_virtio(struct viocam_device *vcd,
			      struct virtio_camera_stream_format *virtio,
			      const struct v4l2_pix_format_mplane *v4l2)
{
	uint8_t i, num_planes;

	num_planes = v4l2->num_planes;
	if (!num_planes || num_planes > VIRTIO_CAMERA_MAX_PLANES) {
		v4l2_err(&vcd->v4l2_dev, "Invalid plane count: %u\n",
			 num_planes);
		return -EINVAL;
	}

	virtio->format = cpu_to_le32(v4l2->pixelformat);
	virtio->width = cpu_to_le32(v4l2->width);
	virtio->height = cpu_to_le32(v4l2->height);
	virtio->num_planes = cpu_to_le32(num_planes);

	for (i = 0; i < num_planes; ++i) {
		virtio->plane_fmt[i].bytesperline =
			cpu_to_le32(v4l2->plane_fmt[i].bytesperline);
		virtio->plane_fmt[i].sizeimage =
			cpu_to_le32(v4l2->plane_fmt[i].sizeimage);
	}

	return 0;
}

static int fmt_virtio_to_v4l2(struct viocam_device *vcd,
			      struct v4l2_pix_format_mplane *v4l2,
			      const struct virtio_camera_stream_format *virtio)
{
	uint8_t i, num_planes;

	num_planes = le32_to_cpu(virtio->num_planes);
	if (!num_planes || num_planes > VIRTIO_CAMERA_MAX_PLANES) {
		v4l2_err(&vcd->v4l2_dev, "Invalid plane count: %u\n",
			 num_planes);
		return -EINVAL;
	}

	v4l2->pixelformat = le32_to_cpu(virtio->format);
	v4l2->width = le32_to_cpu(virtio->width);
	v4l2->height = le32_to_cpu(virtio->height);
	v4l2->field = V4L2_FIELD_NONE;
	v4l2->num_planes = num_planes;

	memset(v4l2->reserved, 0, sizeof(v4l2->reserved));

	for (i = 0; i < v4l2->num_planes; ++i) {
		v4l2->plane_fmt[i].bytesperline =
			le32_to_cpu(virtio->plane_fmt[i].bytesperline);
		v4l2->plane_fmt[i].sizeimage =
			PAGE_ALIGN(le32_to_cpu(virtio->plane_fmt[i].sizeimage));
		memset(v4l2->plane_fmt[i].reserved, 0,
		       sizeof(v4l2->plane_fmt[i].reserved));
	}

	return 0;
}

static void viocam_parse_virtio_frame(
	struct v4l2_frmsize_stepwise *v4l2,
	const struct virtio_camera_stream_format_frame *virtio)
{
	v4l2->min_width = le32_to_cpu(virtio->width.min);
	v4l2->max_width = le32_to_cpu(virtio->width.max);
	v4l2->step_width = le32_to_cpu(virtio->width.step);
	v4l2->min_height = le32_to_cpu(virtio->height.min);
	v4l2->max_height = le32_to_cpu(virtio->height.max);
	v4l2->step_height = le32_to_cpu(virtio->height.step);
}

static ssize_t viocam_parse_virtio_fmt(
	struct viocam_device *vcd, struct viocam_v4l2_format *v4l2_fmt,
	const struct virtio_camera_stream_format_entry *virtio_fmt)
{
	struct virtio_camera_stream_format_frame *virtio_frames;
	size_t idx;

	v4l2_fmt->pixelformat = le32_to_cpu(virtio_fmt->format);
	v4l2_fmt->num_frames = le32_to_cpu(virtio_fmt->num_frames);
	if (v4l2_fmt->num_frames == 0) {
		v4l2_err(&vcd->v4l2_dev, "backend provided no framesizes\n");
		return -EINVAL;
	}
	v4l2_fmt->frames = devm_kcalloc(vcd->v4l2_dev.dev, v4l2_fmt->num_frames,
					sizeof(*v4l2_fmt->frames), GFP_KERNEL);
	if (v4l2_fmt->frames == NULL)
		return -ENOMEM;

	// frames array begins at the end of format struct
	virtio_frames =
		(struct virtio_camera_stream_format_frame *)(virtio_fmt + 1);
	for (idx = 0; idx < v4l2_fmt->num_frames; idx++) {
		viocam_parse_virtio_frame(&v4l2_fmt->frames[idx],
					  &virtio_frames[idx]);
	}

	return sizeof(struct virtio_camera_stream_format_entry) +
	       sizeof(struct virtio_camera_stream_format_frame) * idx;
}

static unsigned int
build_virtio_camera_sglist(struct virtio_camera_resource_sg_list *sgl,
			   struct vb2_buffer *vb, unsigned int plane,
			   bool has_iommu)
{
	int i;
	struct scatterlist *sg;
	struct sg_table *sgt = vb2_dma_sg_plane_desc(vb, plane);

	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		sgl->entries[i].addr = cpu_to_le64(
			has_iommu ? sg_dma_address(sg) : sg_phys(sg));
		sgl->entries[i].length =
			cpu_to_le32(has_iommu ? sg_dma_len(sg) : sg->length);
	}

	sgl->num_entries = sgt->nents;

	return VIRTIO_CAMERA_RESOURCE_SG_SIZE(sgt->nents);
}

static int viocam_parse_stream_formats_resp(
	struct viocam_device *vcd,
	const struct virtio_camera_get_stream_formats_resp *resp,
	struct v4l2_pix_format_mplane *curr_fmt,
	struct viocam_v4l2_format **formats, unsigned int *nformats)
{
	struct viocam_v4l2_format *fmt;
	uint32_t fmt_count;
	int fmt_idx, ret;
	size_t offset;
	struct viocam_v4l2_format *fmts;

	fmt_count = le32_to_cpu(resp->num_entries);
	if (!fmt_count) {
		v4l2_err(&vcd->v4l2_dev, "backend provided no formats\n");
		return -EINVAL;
	}

	fmts = devm_kcalloc(vcd->v4l2_dev.dev, fmt_count, sizeof(*fmts),
			    GFP_KERNEL);
	if (!fmts)
		return -ENOMEM;

	offset = sizeof(struct virtio_camera_get_stream_formats_resp);
	for (fmt_idx = 0; fmt_idx < fmt_count; fmt_idx++) {
		ssize_t fmt_size = 0;

		fmt = &fmts[fmt_idx];

		fmt_size = viocam_parse_virtio_fmt(vcd, fmt,
						   (void *)(resp) + offset);
		if (fmt_size < 0) {
			ret = fmt_size;
			goto parse_fmt_err;
		}
		offset += fmt_size;
	}

	ret = fmt_virtio_to_v4l2(vcd, curr_fmt, &resp->stream_fmt);
	if (ret)
		goto parse_fmt_err;

	/* FIXME: Implement colorspace adjustment according to backend.
	 * For now, we set it to a default one to keep v4l2-compliance happy.
	 */
	curr_fmt->colorspace = VIRTIO_CAMERA_DEFAULT_CS;
	curr_fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(curr_fmt->colorspace);
	curr_fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(curr_fmt->colorspace);
	curr_fmt->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(
		true, curr_fmt->colorspace, curr_fmt->ycbcr_enc);

	*formats = fmts;
	*nformats = fmt_count;

	return 0;

parse_fmt_err:
	while (fmt_count--)
		devm_kfree(vcd->v4l2_dev.dev, fmts[fmt_count].frames);
	devm_kfree(vcd->v4l2_dev.dev, fmts);
	return ret;
}

static void viocam_resource_queue_cmd_complete(struct viocam_msg *msg)
{
	struct viocam_buffer *b = container_of(msg, struct viocam_buffer, msg);
	enum vb2_buffer_state state =
		le32_to_cpu(b->resp.status) == VIRTIO_CAMERA_RESOURCE_QUEUE_OK ?
			VB2_BUF_STATE_DONE :
			VB2_BUF_STATE_ERROR;

	viocam_vb2_buf_queue_done(b, state);
}

void viocam_cmd_cb(struct virtqueue *vq)
{
	struct viocam_device *vcd = vq->vdev->priv;
	struct viocam_msg *msg;
	struct virtio_camera_cmd_hdr *req_head;
	unsigned long flags;
	unsigned int len;

	spin_lock_irqsave(&vcd->cmd_vq.qlock, flags);
	do {
		virtqueue_disable_cb(vq);
		while ((msg = virtqueue_get_buf(vq, &len))) {
			req_head = msg->req;
			if (req_head->cmd_type ==
			    VIRTIO_CAMERA_CMD_RESOURCE_QUEUE) {
				viocam_resource_queue_cmd_complete(msg);
			} else {
				complete(&msg->responded);
				viocam_msg_release(msg);
			}
		}
		if (unlikely(virtqueue_is_broken(vq)))
			break;
	} while (!virtqueue_enable_cb(vq));
	spin_unlock_irqrestore(&vcd->cmd_vq.qlock, flags);
}

void viocam_event_cb(struct virtqueue *vq)
{
	(void)vq;
	// haven't found out what to do with this vqueue yet.
}

static int
viocam_parse_camera_ctrl(struct viocam_device *vcd,
			 const struct virtio_camera_ctrl_entry *entry)
{
	int ret;
	struct v4l2_ctrl *v4l2_ctrl;
	struct v4l2_ctrl_config cfg = { 0 };
	size_t name_len;
	const char *name = entry->name;
	char *name_dup;

	name_len = strnlen(name, sizeof(entry->name) - 1);
	name_dup = devm_kmemdup(vcd->v4l2_dev.dev, name, name_len, GFP_KERNEL);
	if (name_dup == NULL)
		return -ENOMEM;

	name_dup[name_len] = '\0';

	cfg.id = le32_to_cpu(entry->ctrl_id);
	cfg.type = le32_to_cpu(entry->type);
	cfg.min = le32_to_cpu(entry->min);
	cfg.max = le32_to_cpu(entry->max);
	cfg.step = le32_to_cpu(entry->step);
	cfg.def = le32_to_cpu(entry->def);
	cfg.name = name_dup;
	cfg.ops = &ctrl_ops;

	v4l2_ctrl = v4l2_ctrl_new_custom(&vcd->ctrls.hdl, &cfg, name_dup);
	if (v4l2_ctrl == NULL) {
		ret = vcd->ctrls.hdl.error;
		goto err_new_custom;
	}

	if (cfg.id == V4L2_CID_EXPOSURE_AUTO)
		vcd->ctrls.auto_exposure = v4l2_ctrl;
	else if (cfg.id == V4L2_CID_EXPOSURE_ABSOLUTE)
		vcd->ctrls.exposure = v4l2_ctrl;

	return 0;

err_new_custom:
	devm_kfree(vcd->v4l2_dev.dev, name_dup);
	return ret;
}

int virtio_camera_cmd_enum_camera_ctrls(struct viocam_device *vcd,
					uint32_t resp_len)
{
	int ret;
	unsigned int i, nctrl;
	struct v4l2_ctrl *v4l2_ctrl, *next_v4l2_ctrl;

	VIOCAM_MSG_DEFINE_WRESP(hdl, struct virtio_camera_enum_camera_ctrls,
				VIRTIO_CAMERA_CMD_ENUM_CAMERA_CTRLS, resp_len);

	ret = VIOCAM_MSG_INIT(hdl, vcd);
	if (ret)
		return ret;

	ret = viocam_queue_msg_and_wait(vcd, VIOCAM_MSG(hdl), VIOCAM_DBG(hdl));
	if (ret)
		goto queue_msg_err;

	nctrl = le32_to_cpu(VIOCAM_MSG_RESP(hdl)->num_ctrl_entries);

	v4l2_ctrl_handler_init(&vcd->ctrls.hdl, nctrl);

	for (i = 0; i < nctrl; ++i) {
		struct virtio_camera_ctrl_entry *entry =
			(struct virtio_camera_ctrl_entry
				 *)((void *)VIOCAM_MSG_RESP(hdl) +
				    offsetof(typeof(*VIOCAM_MSG_RESP(hdl)),
					     ctrl_entries[i]));
		ret = viocam_parse_camera_ctrl(vcd, entry);
		if (ret)
			goto err_parse;
	}

	viocam_msg_release(VIOCAM_MSG(hdl));

	if (vcd->ctrls.auto_exposure)
		v4l2_ctrl_auto_cluster(2, &vcd->ctrls.auto_exposure,
				       V4L2_EXPOSURE_MANUAL, false);

	vcd->v4l2_dev.ctrl_handler = &vcd->ctrls.hdl;
	v4l2_ctrl_handler_setup(vcd->v4l2_dev.ctrl_handler);

	return 0;

err_parse:
	list_for_each_entry_safe(v4l2_ctrl, next_v4l2_ctrl,
				 &vcd->ctrls.hdl.ctrls, node) {
		devm_kfree(vcd->v4l2_dev.dev, v4l2_ctrl->priv);
	}
	v4l2_ctrl_handler_free(&vcd->ctrls.hdl);
queue_msg_err:
	viocam_msg_release(VIOCAM_MSG(hdl));
	return ret;
}

int virtio_camera_cmd_get_camera_info(struct viocam_device *vcd,
				      uint32_t *ctrl_info_len,
				      uint32_t *min_bufs_needed)
{
	VIOCAM_MSG_DEFINE(hdl, struct virtio_camera_get_camera_info,
			  VIRTIO_CAMERA_CMD_GET_CAMERA_INFO);
	int ret;

	ret = VIOCAM_MSG_INIT(hdl, vcd);
	if (ret)
		return ret;

	ret = viocam_queue_msg_and_wait(vcd, VIOCAM_MSG(hdl), VIOCAM_DBG(hdl));
	if (ret)
		goto queue_msg_err;

	*min_bufs_needed = le32_to_cpu(VIOCAM_MSG_RESP(hdl)->min_buf_needed);
	*ctrl_info_len = le32_to_cpu(VIOCAM_MSG_RESP(hdl)->ctrl_info_len);

queue_msg_err:
	viocam_msg_release(VIOCAM_MSG(hdl));
	return ret;
}

int virtio_camera_cmd_stream_create(struct viocam_device *vcd,
				    uint32_t *fmt_info_len)
{
	VIOCAM_MSG_DEFINE(hdl, struct virtio_camera_stream_create,
			  VIRTIO_CAMERA_CMD_STREAM_CREATE);
	int ret;

	ret = VIOCAM_MSG_INIT(hdl, vcd);
	if (ret)
		return ret;

	VIOCAM_MSG_REQ(hdl)->stream_id = 0;

	ret = viocam_queue_msg_and_wait(vcd, VIOCAM_MSG(hdl), VIOCAM_DBG(hdl));
	if (ret)
		goto queue_msg_err;

	*fmt_info_len = VIOCAM_MSG_RESP(hdl)->fmt_info_len;

queue_msg_err:
	viocam_msg_release(VIOCAM_MSG(hdl));
	return ret;
}

int virtio_camera_cmd_get_stream_formats(
	struct viocam_device *vcd, uint32_t fmt_info_len,
	struct v4l2_pix_format_mplane *curr_fmt,
	struct viocam_v4l2_format **formats, unsigned int *nformats)
{
	VIOCAM_MSG_DEFINE_WRESP(hdl, struct virtio_camera_get_stream_formats,
				VIRTIO_CAMERA_CMD_GET_STREAM_FORMATS,
				fmt_info_len);
	int ret;

	ret = VIOCAM_MSG_INIT(hdl, vcd);
	if (ret)
		return ret;

	VIOCAM_MSG_REQ(hdl)->stream_id = 0;

	ret = viocam_queue_msg_and_wait(vcd, VIOCAM_MSG(hdl), VIOCAM_DBG(hdl));
	if (ret)
		goto queue_msg_err;

	ret = viocam_parse_stream_formats_resp(vcd, VIOCAM_MSG_RESP(hdl),
					       curr_fmt, formats, nformats);

queue_msg_err:
	viocam_msg_release(VIOCAM_MSG(hdl));
	return ret;
}

static size_t calculate_resource_create_req_size(struct vb2_buffer *vb)
{
	size_t req_size = sizeof(struct virtio_camera_resource_create);
	unsigned int i;

	for (i = 0; i < vb->num_planes; i++) {
		req_size += VIRTIO_CAMERA_RESOURCE_SG_SIZE(
			vb2_dma_sg_plane_desc(vb, i)->nents);
	}
	return req_size;
}

int virtio_camera_cmd_resource_create(struct viocam_device *vcd,
				      struct viocam_buffer *buf,
				      uint32_t resource_id)
{
	struct virtio_camera_resource_sg_list *sg_list;
	int ret;
	unsigned int i;
	size_t req_size = calculate_resource_create_req_size(&buf->vb.vb2_buf);
	VIOCAM_MSG_DEFINE_WREQ(hdl, struct virtio_camera_resource_create,
			       VIRTIO_CAMERA_CMD_RESOURCE_CREATE, req_size);

	ret = VIOCAM_MSG_INIT(hdl, vcd);
	if (ret)
		return ret;

	sg_list = ((void *)VIOCAM_MSG_REQ(hdl)) + sizeof(*VIOCAM_MSG_REQ(hdl));
	for (i = 0; i < buf->vb.vb2_buf.num_planes; ++i) {
		sg_list = (void *)sg_list +
			  build_virtio_camera_sglist(sg_list, &buf->vb.vb2_buf,
						     i, vcd->has_iommu);
	}
	VIOCAM_MSG_REQ(hdl)->stream_id = 0;
	VIOCAM_MSG_REQ(hdl)->resource_id = cpu_to_le32(resource_id);

	ret = viocam_queue_msg_and_wait(vcd, VIOCAM_MSG(hdl), VIOCAM_DBG(hdl));
	if (ret)
		goto queue_msg_err;

	// fill the structs that will be used later in RESOURCE_QUEUE
	buf->msg.req_size = sizeof(buf->req);
	buf->msg.resp_size = sizeof(buf->resp);
	buf->msg.req = (void *)&buf->req;
	buf->msg.resp = (void *)&buf->resp;
	buf->req.cmd_type = cpu_to_le32(VIRTIO_CAMERA_CMD_RESOURCE_QUEUE);
	buf->req.stream_id = 0;
	buf->req.resource_id = cpu_to_le32(resource_id);

queue_msg_err:
	viocam_msg_release(VIOCAM_MSG(hdl));
	return ret;
}

int virtio_camera_cmd_resource_queue(struct viocam_device *vcd,
				     struct viocam_buffer *buf)
{
	return viocam_queue_msg(vcd, &buf->msg);
}

int virtio_camera_cmd_resource_destroy(struct viocam_device *vcd)
{
	VIOCAM_MSG_DEFINE(hdl, struct virtio_camera_resource_destroy,
			  VIRTIO_CAMERA_CMD_RESOURCE_DESTROY);
	int ret;

	ret = VIOCAM_MSG_INIT(hdl, vcd);
	if (ret)
		return ret;

	VIOCAM_MSG_REQ(hdl)->stream_id = 0;
	ret = viocam_queue_msg_and_wait(vcd, VIOCAM_MSG(hdl), VIOCAM_DBG(hdl));
	viocam_msg_release(VIOCAM_MSG(hdl));
	return ret;
}

int virtio_camera_cmd_resource_flush(struct viocam_device *vcd)
{
	VIOCAM_MSG_DEFINE(hdl, struct virtio_camera_resource_flush,
			  VIRTIO_CAMERA_CMD_RESOURCE_FLUSH);
	int ret;

	ret = VIOCAM_MSG_INIT(hdl, vcd);
	if (ret)
		return ret;

	VIOCAM_MSG_REQ(hdl)->stream_id = 0;
	ret = viocam_queue_msg_and_wait(vcd, VIOCAM_MSG(hdl), VIOCAM_DBG(hdl));
	viocam_msg_release(VIOCAM_MSG(hdl));
	return ret;
}

int virtio_camera_cmd_set_control(struct viocam_device *vcd, uint32_t ctrl_id,
				  uint32_t ctrl_val)
{
	VIOCAM_MSG_DEFINE(hdl, struct virtio_camera_set_control,
			  VIRTIO_CAMERA_CMD_SET_CONTROL);
	int ret;

	ret = VIOCAM_MSG_INIT(hdl, vcd);
	if (ret)
		return ret;

	VIOCAM_MSG_REQ(hdl)->stream_id = 0;
	VIOCAM_MSG_REQ(hdl)->ctrl_id = cpu_to_le32(ctrl_id);
	VIOCAM_MSG_REQ(hdl)->value = cpu_to_le32(ctrl_val);
	ret = viocam_queue_msg_and_wait(vcd, VIOCAM_MSG(hdl), VIOCAM_DBG(hdl));
	viocam_msg_release(VIOCAM_MSG(hdl));
	return ret;
}

int virtio_camera_cmd_set_format(struct viocam_device *vcd,
				 struct v4l2_pix_format_mplane *fmt)
{
	VIOCAM_MSG_DEFINE(hdl, struct virtio_camera_set_format,
			  VIRTIO_CAMERA_CMD_SET_FORMAT);
	int ret;

	ret = VIOCAM_MSG_INIT(hdl, vcd);
	if (ret)
		return ret;

	VIOCAM_MSG_REQ(hdl)->stream_id = 0;

	ret = fmt_v4l2_to_virtio(vcd, &VIOCAM_MSG_REQ(hdl)->fmt, fmt);
	if (ret)
		goto queue_msg_err;

	ret = viocam_queue_msg_and_wait(vcd, VIOCAM_MSG(hdl), VIOCAM_DBG(hdl));
	if (ret)
		goto queue_msg_err;

	ret = fmt_virtio_to_v4l2(vcd, fmt, &VIOCAM_MSG_RESP(hdl)->fmt);

queue_msg_err:
	viocam_msg_release(VIOCAM_MSG(hdl));
	return ret;
}
