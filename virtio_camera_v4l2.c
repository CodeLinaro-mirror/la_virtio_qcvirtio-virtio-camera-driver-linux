/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
/*
 * Virtio Camera Device
 */

#include "virtio_camera.h"

#include <media/videobuf2-v4l2.h>
#include "media/videobuf2-dma-sg.h"
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <linux/version.h>

typedef int (*fmtfunc)(struct file *file, void *priv, struct v4l2_format *f);

/*
 * Helper functions
 */

static struct virtio_camera_format_info *
find_stream_fmt(struct virtio_camera_stream *stream, uint32_t format)
{
	unsigned int i;
	for (i = 0; i < stream->nformats; i++) {
		if (stream->formats[i].desc.format == format) {
			return &stream->formats[i];
		}
	}

	return NULL;
}

static void virtio_camera_resource_id_get(struct virtio_camera_device *vcd,
					  uint32_t *id)
{
	int handle;

	idr_preload(GFP_KERNEL);
	spin_lock(&vcd->resource_idr_lock);
	handle = idr_alloc(&vcd->resource_idr, NULL, 1, 0, GFP_NOWAIT);
	spin_unlock(&vcd->resource_idr_lock);
	idr_preload_end();
	*id = handle;
}

static void virtio_camera_resource_id_put(struct virtio_camera_device *vcd,
					  uint32_t id)
{
	spin_lock(&vcd->resource_idr_lock);
	idr_remove(&vcd->resource_idr, id);
	spin_unlock(&vcd->resource_idr_lock);
}

static void
virtio_camera_vb2_queue_active_push(struct virtio_camera_device *vcd,
				    struct virtio_camera_buffer *buf)
{
	unsigned long flags;
	spin_lock_irqsave(&vcd->vbq_lock, flags);
	list_add_tail(&buf->list, &vcd->vbq_active);
	spin_unlock_irqrestore(&vcd->vbq_lock, flags);
}

static void virtio_camera_vb2_cancel_bufs(struct virtio_camera_device *vcd)
{
	unsigned long flags;
	struct virtio_camera_buffer *buf;

	spin_lock_irqsave(&vcd->vbq_lock, flags);
	while (!list_empty(&vcd->vbq_active)) {
		buf = list_first_entry(&vcd->vbq_active,
				       struct virtio_camera_buffer, list);
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&vcd->vbq_lock, flags);
}

/*
 * V4L2 ioctl ops
 */

static int virtio_camera_querycap(struct file *file, void *fh,
				  struct v4l2_capability *cap)
{
	struct virtio_camera_device *vcd = video_drvdata(file);
	struct video_device *video_dev = video_devdata(file);

	if (strscpy(cap->driver, DRIVER_NAME, sizeof(cap->driver)) < 0)
		v4l2_warn(&vcd->v4l2_dev, "failed to copy driver name");

	if (strscpy(cap->card, video_dev->name, sizeof(cap->card)) < 0)
		v4l2_warn(&vcd->v4l2_dev, "failed to copy card name");

	/*
	 * TODO: replace platform with virtio once v4l2-compliance and v4l2
	 * kernel docs are updated.
	 */
	if (snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		     video_dev->name) < 0)
		v4l2_warn(&vcd->v4l2_dev, "failed to copy bus_info");
	cap->device_caps = video_dev->device_caps;
	return 0;
}

static int virtio_camera_enum_fmt_vid_cap(struct file *file, void *fh,
					  struct v4l2_fmtdesc *f)
{
	struct virtio_camera_stream *stream = file2stream(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (f->index >= stream->nformats)
		return -EINVAL;

	f->pixelformat = stream->formats[f->index].desc.format;

	return 0;
}

static void fmt_sp2mp(const struct v4l2_format *sp_fmt,
		      struct v4l2_format *mp_fmt)
{
	struct v4l2_pix_format_mplane *mp = &mp_fmt->fmt.pix_mp;
	struct v4l2_plane_pix_format *ppix = &mp->plane_fmt[0];
	const struct v4l2_pix_format *pix = &sp_fmt->fmt.pix;
	bool is_out = sp_fmt->type == V4L2_BUF_TYPE_VIDEO_OUTPUT;

	memset(mp->reserved, 0, sizeof(mp->reserved));
	mp_fmt->type = is_out ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
				V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	mp->width = pix->width;
	mp->height = pix->height;
	mp->pixelformat = pix->pixelformat;
	mp->field = pix->field;
	mp->colorspace = pix->colorspace;
	mp->xfer_func = pix->xfer_func;
	/* Also copies hsv_enc */
	mp->ycbcr_enc = pix->ycbcr_enc;
	mp->quantization = pix->quantization;
	mp->num_planes = 1;
	mp->flags = pix->flags;
	ppix->sizeimage = pix->sizeimage;
	ppix->bytesperline = pix->bytesperline;
	memset(ppix->reserved, 0, sizeof(ppix->reserved));
}

static void fmt_mp2sp(const struct v4l2_format *mp_fmt,
		      struct v4l2_format *sp_fmt)
{
	const struct v4l2_pix_format_mplane *mp = &mp_fmt->fmt.pix_mp;
	const struct v4l2_plane_pix_format *ppix = &mp->plane_fmt[0];
	struct v4l2_pix_format *pix = &sp_fmt->fmt.pix;

	pix->width = mp->width;
	pix->height = mp->height;
	pix->pixelformat = mp->pixelformat;
	pix->field = mp->field;
	pix->colorspace = mp->colorspace;
	pix->xfer_func = mp->xfer_func;
	/* Also copies hsv_enc */
	pix->ycbcr_enc = mp->ycbcr_enc;
	pix->quantization = mp->quantization;
	pix->sizeimage = ppix->sizeimage;
	pix->bytesperline = ppix->bytesperline;
	pix->flags = mp->flags;
}

static int fmt_sp2mp_func(struct file *file, void *priv, struct v4l2_format *f,
			  fmtfunc func)
{
	struct v4l2_format fmt;
	int ret;

	/* Converts to a mplane format */
	fmt_sp2mp(f, &fmt);
	/* Passes it to the generic mplane format function */
	ret = func(file, priv, &fmt);
	/* Copies back the mplane data to the single plane format */
	fmt_mp2sp(&fmt, f);

	return ret;
}

static int virtio_camera_g_fmt_vid_cap(struct file *file, void *fh,
				       struct v4l2_format *f)
{
	struct virtio_camera_stream *stream = file2stream(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memcpy(&f->fmt.pix_mp, &stream->pix_format, sizeof(f->fmt.pix_mp));

	return 0;
}

static inline bool within_range(uint32_t min, uint32_t val, uint32_t max)
{
	return ((min <= val) && (val <= max));
}

static int virtio_camera_try_fmt_vid_cap(struct file *file, void *fh,
					 struct v4l2_format *f)
{
	struct virtio_camera_stream *stream = file2stream(file);
	struct virtio_camera_format_info *fmt;
	struct virtio_camera_stream_format_frame *frame;
	struct v4l2_pix_format_mplane *mp = &f->fmt.pix_mp;
	int i;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	fmt = find_stream_fmt(stream, mp->pixelformat);
	if (!fmt) {
		/* Requested pixelformat is not supported. Pick up the first
		 * supported one and hope it will be enough.
		 */
		if (stream->nformats) {
			fmt = &stream->formats[0];
		}
	}

	if (!fmt || !fmt->desc.num_frames)
		return -EIO;

	mp->pixelformat = fmt->desc.format;

	/*
	 * TODO: Maybe retrieve the value from backend format info?
	 */
	mp->num_planes = 1;

	/* Try to find a suitable frame size. If there is no match, then
	 * just use the last (usually biggest) frame.
	 */
	frame = &fmt->frames[0].frame;
	for (i = 0; i < fmt->desc.num_frames; i++) {
		frame = &fmt->frames[i].frame;
		if (within_range(frame->width.min, mp->width,
				 frame->width.max) &&
		    within_range(frame->height.min, mp->height,
				 frame->height.max))
			break;
	}

	if (frame->width.step)
		mp->width = ALIGN(mp->width, frame->width.step);
	if (frame->height.step)
		mp->height = ALIGN(mp->height, frame->height.step);
	mp->width = clamp(mp->width, frame->width.min, frame->width.max);
	mp->height = clamp(mp->height, frame->height.min, frame->height.max);

	/* TODO: Implement colorspace adjustment according to backend.
	 * For now, we clamp it to a default one to keep v4l2-compliance happy.
	 */
	if (mp->colorspace == V4L2_COLORSPACE_DEFAULT ||
	    mp->colorspace > V4L2_COLORSPACE_DCI_P3)
		mp->colorspace = VIRTIO_CAMERA_DEFAULT_CS;
	if (mp->xfer_func == V4L2_XFER_FUNC_DEFAULT ||
	    mp->xfer_func > V4L2_XFER_FUNC_SMPTE2084)
		mp->xfer_func = VIRTIO_CAMERA_DEFAULT_XFER_F;
	if (mp->ycbcr_enc > V4L2_YCBCR_ENC_SMPTE240M)
		mp->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	if (mp->quantization > V4L2_QUANTIZATION_LIM_RANGE)
		mp->quantization = V4L2_QUANTIZATION_DEFAULT;
	mp->field = V4L2_FIELD_NONE;

	memset(mp->reserved, 0, sizeof(mp->reserved));

	return 0;
}

static int virtio_camera_s_fmt_vid_cap(struct file *file, void *fh,
				       struct v4l2_format *f)
{
	struct virtio_camera_device *vcd = video_drvdata(file);
	struct virtio_camera_stream *stream = file2stream(file);
	int ret;

	if (vb2_is_busy(&vcd->vb_q)) {
		return -EBUSY;
	}

	ret = virtio_camera_try_fmt_vid_cap(file, fh, f);
	if (ret < 0)
		return ret;

	memcpy(&stream->pix_format, &f->fmt.pix_mp, sizeof(f->fmt.pix_mp));

	ret = virtio_camera_cmd_set_format(vcd, stream);
	if (ret < 0) {
		v4l2_err(&vcd->v4l2_dev, "failed to set a new format");
		return -EINVAL;
	}

	memcpy(&f->fmt.pix_mp, &stream->pix_format, sizeof(f->fmt.pix_mp));

	return 0;
}

static int vidioc_try_fmt_vid_cap_mplane(struct file *file, void *priv,
					 struct v4l2_format *f)
{
	struct virtio_camera_device *vcd = video_drvdata(file);
	if (!vcd->mplane_api)
		return -ENOTTY;
	return virtio_camera_try_fmt_vid_cap(file, priv, f);
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct virtio_camera_device *vcd = video_drvdata(file);
	if (vcd->mplane_api)
		return -ENOTTY;
	return fmt_sp2mp_func(file, priv, f, virtio_camera_try_fmt_vid_cap);
}

static int vidioc_g_fmt_vid_cap_mplane(struct file *file, void *priv,
				       struct v4l2_format *f)
{
	struct virtio_camera_device *vcd = video_drvdata(file);
	if (!vcd->mplane_api)
		return -ENOTTY;
	return virtio_camera_g_fmt_vid_cap(file, priv, f);
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct virtio_camera_device *vcd = video_drvdata(file);
	if (vcd->mplane_api)
		return -ENOTTY;
	return fmt_sp2mp_func(file, priv, f, virtio_camera_g_fmt_vid_cap);
}

static int vidioc_s_fmt_vid_cap_mplane(struct file *file, void *priv,
				       struct v4l2_format *f)
{
	struct virtio_camera_device *vcd = video_drvdata(file);
	if (!vcd->mplane_api)
		return -ENOTTY;
	return virtio_camera_s_fmt_vid_cap(file, priv, f);
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct virtio_camera_device *vcd = video_drvdata(file);
	if (vcd->mplane_api)
		return -ENOTTY;
	return fmt_sp2mp_func(file, priv, f, virtio_camera_s_fmt_vid_cap);
}

static int vidioc_enum_input(struct file *file, void *fh,
			     struct v4l2_input *inp)
{
	struct virtio_camera_device *vcd = video_drvdata(file);

	if (inp->index != 0)
		return -EINVAL;

	if (strscpy(inp->name, "camera", sizeof(inp->name)) < 0)
		v4l2_warn(&vcd->v4l2_dev, "failed to write input name\n");

	inp->type = V4L2_INPUT_TYPE_CAMERA;

	return 0;
}

static int vidioc_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int vidioc_s_input(struct file *file, void *fh, unsigned int i)
{
	if (i != 0)
		return -EINVAL;

	return 0;
}

static int virtio_camera_enum_framesizes(struct file *file, void *fh,
					 struct v4l2_frmsizeenum *f)
{
	struct virtio_camera_stream *stream = file2stream(file);
	struct virtio_camera_format_info *fmt;
	struct camera_format_frame *frm;
	struct virtio_camera_stream_format_frame *frame;
	int idx = f->index;

	fmt = find_stream_fmt(stream, f->pixel_format);
	if (fmt == NULL) {
		return -EINVAL;
	}

	if (idx >= fmt->desc.num_frames)
		return -EINVAL;

	frm = &fmt->frames[idx];
	frame = &frm->frame;

	if (frame->width.min == frame->width.max &&
	    frame->height.min == frame->height.max) {
		f->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		f->discrete.width = frame->width.min;
		f->discrete.height = frame->height.min;
		return 0;
	}

	if (idx != 0)
		return -EINVAL;

	f->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	f->stepwise.min_width = frame->width.min;
	f->stepwise.max_width = frame->width.max;
	f->stepwise.min_height = frame->height.min;
	f->stepwise.max_height = frame->height.max;
	f->stepwise.step_width = frame->width.step;
	f->stepwise.step_height = frame->height.step;
	return 0;
}

const struct v4l2_ioctl_ops virtio_camera_ioctl_ops = {
	.vidioc_querycap = virtio_camera_querycap,
	.vidioc_enum_fmt_vid_cap = virtio_camera_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = vidioc_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = vidioc_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = vidioc_try_fmt_vid_cap,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 3, 0))
	.vidioc_enum_fmt_vid_cap_mplane = virtio_camera_enum_fmt_vid_cap,
#endif
	.vidioc_g_fmt_vid_cap_mplane = vidioc_g_fmt_vid_cap_mplane,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_s_fmt_vid_cap_mplane,
	.vidioc_try_fmt_vid_cap_mplane = vidioc_try_fmt_vid_cap_mplane,

	.vidioc_enum_framesizes = virtio_camera_enum_framesizes,

	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_enum_input = vidioc_enum_input,
	.vidioc_s_input = vidioc_s_input,
	.vidioc_g_input = vidioc_g_input,

	/* videobuf */
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

/*
 * Videobuf2 ops
 */

static int virtio_camera_vb2_queue_setup(struct vb2_queue *vq,
					 unsigned *nbuffers, unsigned *nplanes,
					 unsigned sizes[],
					 struct device *alloc_devs[])
{
	struct virtio_camera_stream *stream = vb2_get_drv_priv(vq);
	unsigned int i;
	unsigned fmt_nplanes = stream->pix_format.num_planes;

	if (*nplanes) {
		if (*nplanes != fmt_nplanes)
			return -EINVAL;

		for (i = 0; i < fmt_nplanes; ++i) {
			if (sizes[i] <
				    stream->pix_format.plane_fmt[i].sizeimage ||
			    !IS_ALIGNED(sizes[i], PAGE_SIZE))
				return -EINVAL;
		}
	} else {
		for (i = 0; i < fmt_nplanes; ++i) {
			sizes[i] = stream->pix_format.plane_fmt[i].sizeimage;
		}
	}

	*nplanes = fmt_nplanes;

	return 0;
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

static int virtio_camera_vb2_buf_init(struct vb2_buffer *vb)
{
	struct virtio_camera_stream *stream = vb2_get_drv_priv(vb->vb2_queue);
	struct virtio_camera_device *vcd = video_get_drvdata(stream->video_dev);
	struct virtio_camera_resource_sg_list *sg_list;
	struct virtio_camera_buffer *vb_buf =
		(struct virtio_camera_buffer *)to_vb2_v4l2_buffer(vb);
	void *buf;
	size_t offset = 0;
	size_t buf_size = 0;
	unsigned int i;
	int ret;
	uint32_t resource_id;

	for (i = 0; i < vb->num_planes; i++) {
		buf_size += VIRTIO_CAMERA_RESOURCE_SG_SIZE(
			vb2_dma_sg_plane_desc(vb, i)->nents);
	}

	buf = kcalloc(1, buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < vb->num_planes; ++i) {
		sg_list = buf + offset;
		offset += build_virtio_camera_sglist(sg_list, vb, i,
						     vcd->has_iommu);
	}

	virtio_camera_resource_id_get(vcd, &resource_id);

	ret = virtio_camera_cmd_resource_create(vcd, stream, buf, buf_size,
						resource_id);
	if (ret >= 0) {
		vb_buf->resource_id = resource_id;
		vb_buf->vb.field = V4L2_FIELD_NONE;
		ret = 0;
	} else {
		v4l2_err(&vcd->v4l2_dev, "failed to create resource");
		virtio_camera_resource_id_put(vcd, resource_id);
	}

	kfree(buf);

	return ret;
}

static void virtio_camera_vb2_buf_cleanup(struct vb2_buffer *vb)
{
	struct virtio_camera_stream *stream = vb2_get_drv_priv(vb->vb2_queue);
	struct virtio_camera_device *vcd = video_get_drvdata(stream->video_dev);
	struct virtio_camera_buffer *vb_buf =
		(struct virtio_camera_buffer *)to_vb2_v4l2_buffer(vb);

	virtio_camera_resource_id_put(vcd, vb_buf->resource_id);
}

static void virtio_camera_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct virtio_camera_stream *stream = vb2_get_drv_priv(vb->vb2_queue);
	struct virtio_camera_device *vcd = video_get_drvdata(stream->video_dev);
	struct virtio_camera_buffer *vb_buf =
		(struct virtio_camera_buffer *)to_vb2_v4l2_buffer(vb);
	int ret;

	virtio_camera_vb2_queue_active_push(vcd, vb_buf);

	ret = virtio_camera_cmd_resource_queue(vcd, stream, vb_buf);
	if (ret < 0) {
		unsigned long flags;
		v4l2_err(&vcd->v4l2_dev, "failed to queue resource(%u)",
			 vb_buf->resource_id);

		spin_lock_irqsave(&vcd->vbq_lock, flags);
		list_del(&vb_buf->list);
		spin_unlock_irqrestore(&vcd->vbq_lock, flags);
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
	}
}

void virtio_camera_vb2_buf_queue_done(
	struct virtio_camera_buffer *vb_buf,
	enum virtio_camera_resource_queue_status status, uint64_t timestamp)
{
	struct vb2_v4l2_buffer *v4l2_vb = &vb_buf->vb;
	struct vb2_buffer *vb = &v4l2_vb->vb2_buf;
	struct vb2_queue *q = vb->vb2_queue;
	struct virtio_camera_stream *stream = vb2_get_drv_priv(q);
	struct virtio_camera_device *vcd = video_get_drvdata(stream->video_dev);
	enum vb2_buffer_state state;
	unsigned int i;
	unsigned long flags;

	spin_lock_irqsave(&vcd->vbq_lock, flags);
	if (list_empty(&vcd->vbq_active)) {
		spin_unlock_irqrestore(&vcd->vbq_lock, flags);
		return;
	}
	list_del(&vb_buf->list);
	spin_unlock_irqrestore(&vcd->vbq_lock, flags);

	switch (status) {
	case VIRTIO_CAMERA_RESOURCE_QUEUE_OK:
		state = VB2_BUF_STATE_DONE;
		for (i = 0; i < vb->num_planes; ++i) {
			vb->planes[i].bytesused =
				stream->pix_format.plane_fmt[i].sizeimage;
		}
		vb->timestamp = ktime_get_ns();
		break;
	case VIRTIO_CAMERA_RESOURCE_QUEUE_ERR:
	default:
		state = VB2_BUF_STATE_ERROR;
		for (i = 0; i < vb->num_planes; ++i) {
			vb->planes[i].bytesused = 0;
		}
		break;
	}

	vb2_buffer_done(vb, state);
}

static void virtio_camera_vb2_stop_streaming(struct vb2_queue *q)
{
	struct virtio_camera_stream *stream = vb2_get_drv_priv(q);
	struct virtio_camera_device *vcd = video_get_drvdata(stream->video_dev);

	if (virtio_camera_cmd_resource_destroy(vcd, stream) < 0)
		v4l2_err(&vcd->v4l2_dev, "failed to destroy resources");

	virtio_camera_vb2_cancel_bufs(vcd);
}

static const struct vb2_ops virtio_camera_vb2_ops = {
	.queue_setup = virtio_camera_vb2_queue_setup,
	.buf_init = virtio_camera_vb2_buf_init,
	.buf_cleanup = virtio_camera_vb2_buf_cleanup,
	.buf_queue = virtio_camera_vb2_buf_queue,
	.stop_streaming = virtio_camera_vb2_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

int virtio_camera_vb2_queue_init(struct virtio_camera_device *vcd,
				 struct virtio_camera_stream *stream)
{
	struct vb2_queue *q = &vcd->vb_q;

	if (vcd->mplane_api)
		q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	else
		q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = stream;
	q->buf_struct_size = sizeof(struct virtio_camera_buffer);
	q->ops = &virtio_camera_vb2_ops;
	q->mem_ops = &vb2_dma_sg_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	q->min_queued_buffers = vcd->camera_sensor.min_buf_needed;
#else
	q->min_buffers_needed = vcd->camera_sensor.min_buf_needed;
#endif
	q->lock = &vcd->video_dev_mutex;
	q->dev = vcd->v4l2_dev.dev;
	q->gfp_flags = GFP_DMA;

	return vb2_queue_init(q);
}

void virtio_camera_vb2_queue_deinit(struct virtio_camera_device *vcd)
{
	vb2_queue_release(&vcd->vb_q);
	vcd->video_dev.queue->owner = NULL;
}
