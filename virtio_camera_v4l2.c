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

static struct viocam_v4l2_format *find_stream_fmt(struct viocam_stream *stream,
						  uint32_t format)
{
	unsigned int i;

	for (i = 0; i < stream->nformats; i++) {
		if (stream->formats[i].pixelformat == format)
			return &stream->formats[i];
	}

	return NULL;
}

/*
 * V4L2 ioctl ops
 */

static int viocam_querycap(struct file *file, void *fh,
			   struct v4l2_capability *cap)
{
	struct video_device *video_dev = video_devdata(file);

	strscpy(cap->driver, DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, video_dev->name, sizeof(cap->card));

	/*
	 * TODO: replace platform with virtio once v4l2-compliance and v4l2
	 * kernel docs are updated.
	 */
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 video_dev->name);
	cap->device_caps = video_dev->device_caps;
	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *fh,
				   struct v4l2_fmtdesc *f)
{
	struct viocam_stream *stream = file2stream(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (f->index >= stream->nformats)
		return -EINVAL;

	f->pixelformat = stream->formats[f->index].pixelformat;

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

static int viocam_g_fmt_vid_cap(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct viocam_stream *stream = file2stream(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memcpy(&f->fmt.pix_mp, &stream->curr_fmt, sizeof(f->fmt.pix_mp));

	return 0;
}

static inline bool within_range(uint32_t min, uint32_t val, uint32_t max)
{
	return ((min <= val) && (val <= max));
}

static int viocam_try_fmt_vid_cap(struct file *file, void *fh,
				  struct v4l2_format *f)
{
	struct viocam_stream *stream = file2stream(file);
	struct viocam_v4l2_format *fmt;
	struct v4l2_frmsize_stepwise *frame;
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
		if (stream->nformats)
			fmt = &stream->formats[0];
	}

	if (!fmt || !fmt->num_frames)
		return -EIO;

	mp->pixelformat = fmt->pixelformat;

	/*
	 * TODO: Maybe retrieve the value from backend format info?
	 */
	mp->num_planes = 1;

	/* Try to find a suitable frame size. If there is no match, then
	 * just use the last (usually biggest) frame.
	 */
	frame = &fmt->frames[0];
	for (i = 0; i < fmt->num_frames; i++) {
		frame = &fmt->frames[i];
		if (within_range(frame->min_width, mp->width,
				 frame->max_width) &&
		    within_range(frame->min_height, mp->height,
				 frame->max_height))
			break;
	}

	if (frame->step_width)
		mp->width = ALIGN(mp->width, frame->step_width);
	if (frame->step_height)
		mp->height = ALIGN(mp->height, frame->step_height);
	mp->width = clamp(mp->width, frame->min_width, frame->max_width);
	mp->height = clamp(mp->height, frame->min_height, frame->max_height);

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

static int viocam_s_fmt_vid_cap(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct viocam_device *vcd = video_drvdata(file);
	struct viocam_stream *stream = file2stream(file);
	int ret;

	if (vb2_is_busy(&stream->vb_q))
		return -EBUSY;

	ret = viocam_try_fmt_vid_cap(file, fh, f);
	if (ret < 0)
		return ret;

	ret = virtio_camera_cmd_set_format(vcd, &f->fmt.pix_mp);
	if (ret < 0)
		return ret;

	memcpy(&stream->curr_fmt, &f->fmt.pix_mp, sizeof(f->fmt.pix_mp));

	return 0;
}

static int vidioc_try_fmt_vid_cap_mplane(struct file *file, void *priv,
					 struct v4l2_format *f)
{
	struct viocam_device *vcd = video_drvdata(file);

	if (!vcd->mplane_api)
		return -ENOTTY;
	return viocam_try_fmt_vid_cap(file, priv, f);
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct viocam_device *vcd = video_drvdata(file);

	if (vcd->mplane_api)
		return -ENOTTY;
	return fmt_sp2mp_func(file, priv, f, viocam_try_fmt_vid_cap);
}

static int vidioc_g_fmt_vid_cap_mplane(struct file *file, void *priv,
				       struct v4l2_format *f)
{
	struct viocam_device *vcd = video_drvdata(file);

	if (!vcd->mplane_api)
		return -ENOTTY;
	return viocam_g_fmt_vid_cap(file, priv, f);
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct viocam_device *vcd = video_drvdata(file);

	if (vcd->mplane_api)
		return -ENOTTY;
	return fmt_sp2mp_func(file, priv, f, viocam_g_fmt_vid_cap);
}

static int vidioc_s_fmt_vid_cap_mplane(struct file *file, void *priv,
				       struct v4l2_format *f)
{
	struct viocam_device *vcd = video_drvdata(file);

	if (!vcd->mplane_api)
		return -ENOTTY;
	return viocam_s_fmt_vid_cap(file, priv, f);
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct viocam_device *vcd = video_drvdata(file);

	if (vcd->mplane_api)
		return -ENOTTY;
	return fmt_sp2mp_func(file, priv, f, viocam_s_fmt_vid_cap);
}

static int vidioc_enum_input(struct file *file, void *fh,
			     struct v4l2_input *inp)
{
	struct viocam_device *vcd = video_drvdata(file);

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
	return i == 0 ? 0 : -EINVAL;
}

static int viocam_enum_framesizes(struct file *file, void *fh,
				  struct v4l2_frmsizeenum *f)
{
	struct viocam_stream *stream = file2stream(file);
	struct viocam_v4l2_format *fmt;
	struct v4l2_frmsize_stepwise *frame;
	int idx = f->index;

	fmt = find_stream_fmt(stream, f->pixel_format);
	if (fmt == NULL)
		return -EINVAL;

	if (idx >= fmt->num_frames)
		return -EINVAL;

	frame = &fmt->frames[idx];

	if (frame->min_width == frame->max_width &&
	    frame->min_height == frame->max_height) {
		f->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		f->discrete.width = frame->min_width;
		f->discrete.height = frame->min_height;
		return 0;
	}

	if (idx != 0)
		return -EINVAL;

	f->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	f->stepwise = *frame;
	return 0;
}

/*
 * File ops
 */

static int viocam_device_open(struct file *file)
{
	struct viocam_device *vcd = video_drvdata(file);
	struct video_device *video_dev = video_devdata(file);
	struct viocam_fh *vfh;

	vfh = kzalloc(sizeof(*vfh), GFP_KERNEL);
	if (vfh == NULL)
		return -ENOMEM;

	vfh->stream = &vcd->stream;

	mutex_lock(video_dev->lock);
	v4l2_fh_init(&vfh->fh, video_dev);
	file->private_data = &vfh->fh;
	v4l2_fh_add(&vfh->fh);
	mutex_unlock(video_dev->lock);

	return 0;
}

static int viocam_device_release(struct file *file)
{
	vb2_fop_release(file);
	return 0;
}

/*
 * Videobuf2 ops
 */

static int viocam_vb2_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
				  unsigned int *nplanes, unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct viocam_stream *stream = vb2_get_drv_priv(vq);
	unsigned int i;
	unsigned int fmt_nplanes = stream->curr_fmt.num_planes;

	if (*nplanes) {
		if (*nplanes != fmt_nplanes)
			return -EINVAL;

		for (i = 0; i < fmt_nplanes; ++i) {
			if (sizes[i] <
				    stream->curr_fmt.plane_fmt[i].sizeimage ||
			    !IS_ALIGNED(sizes[i], PAGE_SIZE))
				return -EINVAL;
		}
	} else {
		for (i = 0; i < fmt_nplanes; ++i)
			sizes[i] = stream->curr_fmt.plane_fmt[i].sizeimage;
	}

	*nplanes = fmt_nplanes;

	return 0;
}

static inline struct viocam_buffer *vb2_to_viocam_buf(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct viocam_buffer, vb);
}

static int viocam_vb2_buf_init(struct vb2_buffer *vb)
{
	struct viocam_stream *stream = vb2_get_drv_priv(vb->vb2_queue);
	struct viocam_device *vcd = video_get_drvdata(&stream->video_dev);
	struct viocam_buffer *buf = vb2_to_viocam_buf(vb);
	int ret;
	uint32_t resource_id;
	unsigned int i;

	ret = ida_alloc(&vcd->resource_ida, GFP_KERNEL);
	if (ret < 0)
		return ret;

	resource_id = ret;

	ret = virtio_camera_cmd_resource_create(vcd, buf, resource_id);
	if (!ret) {
		buf->resource_id = resource_id;
		buf->vb.field = V4L2_FIELD_NONE;
		for (i = 0; i < vb->num_planes; ++i)
			vb2_set_plane_payload(
				vb, i, stream->curr_fmt.plane_fmt[i].sizeimage);
	} else {
		ida_free(&vcd->resource_ida, resource_id);
	}

	return ret;
}

static void viocam_vb2_buf_cleanup(struct vb2_buffer *vb)
{
	struct viocam_stream *stream = vb2_get_drv_priv(vb->vb2_queue);
	struct viocam_device *vcd = video_get_drvdata(&stream->video_dev);
	struct viocam_buffer *buf = vb2_to_viocam_buf(vb);

	ida_free(&vcd->resource_ida, buf->resource_id);

	if (ida_is_empty(&vcd->resource_ida))
		virtio_camera_cmd_resource_destroy(vcd);
}

static void viocam_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct viocam_stream *stream = vb2_get_drv_priv(vb->vb2_queue);
	struct viocam_device *vcd = video_get_drvdata(&stream->video_dev);
	struct viocam_buffer *buf = vb2_to_viocam_buf(vb);
	int ret;

	ret = virtio_camera_cmd_resource_queue(vcd, buf);
	if (ret < 0)
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
}

static void viocam_vb2_stop_streaming(struct vb2_queue *q)
{
	struct viocam_stream *stream = vb2_get_drv_priv(q);
	struct viocam_device *vcd = video_get_drvdata(&stream->video_dev);

	virtio_camera_cmd_resource_flush(vcd);

	/* Has to wait until all RESOURCE_QUEUE responses are collected. Not safe
	 * to continue otherwise.
	 */
	vb2_wait_for_all_buffers(q);
}

static const struct vb2_ops viocam_vb2_ops = {
	.queue_setup = viocam_vb2_queue_setup,
	.buf_init = viocam_vb2_buf_init,
	.buf_cleanup = viocam_vb2_buf_cleanup,
	.buf_queue = viocam_vb2_buf_queue,
	.stop_streaming = viocam_vb2_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

void viocam_vb2_buf_queue_done(struct viocam_buffer *buf,
			       enum vb2_buffer_state state)
{
	struct vb2_buffer *vb = &buf->vb.vb2_buf;
	unsigned int i;

	vb->timestamp = ktime_get_ns();
	if (state == VB2_BUF_STATE_DONE) {
		for (i = 0; i < vb->num_planes; i++)
			vb2_set_plane_payload(vb, i, vb->planes[i].length);
	}
	vb2_buffer_done(vb, state);
}

int viocam_vb2_queue_init(struct viocam_device *vcd,
			  struct viocam_stream *stream,
			  uint32_t min_bufs_needed)
{
	struct vb2_queue *q = &stream->vb_q;

	if (vcd->mplane_api)
		q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	else
		q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = stream;
	q->buf_struct_size = sizeof(struct viocam_buffer);
	q->ops = &viocam_vb2_ops;
	q->mem_ops = &vb2_dma_sg_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
#if KERNEL_VERSION(6, 8, 0) <= LINUX_VERSION_CODE
	q->min_queued_buffers = min_bufs_needed;
#else
	q->min_buffers_needed = min_bufs_needed;
#endif
	q->lock = &stream->video_dev_mutex;
	q->dev = vcd->v4l2_dev.dev;

	return vb2_queue_init(q);
}

const struct v4l2_file_operations viocam_fops = {
	.owner = THIS_MODULE,
	.open = viocam_device_open,
	.release = viocam_device_release,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
};

const struct v4l2_ioctl_ops viocam_ioctl_ops = {
	.vidioc_querycap = viocam_querycap,
	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = vidioc_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = vidioc_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = vidioc_try_fmt_vid_cap,
#if KERNEL_VERSION(5, 3, 0) > LINUX_VERSION_CODE
	.vidioc_enum_fmt_vid_cap_mplane = vidioc_enum_fmt_vid_cap,
#endif
	.vidioc_g_fmt_vid_cap_mplane = vidioc_g_fmt_vid_cap_mplane,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_s_fmt_vid_cap_mplane,
	.vidioc_try_fmt_vid_cap_mplane = vidioc_try_fmt_vid_cap_mplane,
	.vidioc_enum_framesizes = viocam_enum_framesizes,
	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_enum_input = vidioc_enum_input,
	.vidioc_s_input = vidioc_s_input,
	.vidioc_g_input = vidioc_g_input,
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
