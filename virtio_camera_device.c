/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "virtio_camera.h"

#include <linux/version.h>
#include <media/v4l2-ioctl.h>

/*
 * Helper sunctions
 */

static void virtio_camera_clean_ctrl(struct virtio_camera_device *vcd)
{
	struct v4l2_ctrl *v4l2_ctrl, *next_v4l2_ctrl;

	list_for_each_entry_safe(v4l2_ctrl, next_v4l2_ctrl,
				 &vcd->ctrls.hdl.ctrls, node) {
		kfree(v4l2_ctrl->priv);
	}
	v4l2_ctrl_handler_free(&vcd->ctrls.hdl);
}

static void virtio_camera_clean_stream_fmt(struct virtio_camera_stream *stream)
{
	unsigned int i = 0;
	for (i = 0; i < stream->nformats; i++) {
		kfree(stream->formats[i].frames);
	}
	kfree(stream->formats);
}

static void virtio_camera_clean(struct virtio_camera_device *vcd)
{
	struct virtio_camera_stream *stream, *tmp_stream;

	virtio_camera_clean_ctrl(vcd);
	list_for_each_entry_safe(stream, tmp_stream,
				 &vcd->camera_sensor.streams_list, list) {
		virtio_camera_clean_stream_fmt(stream);
		list_del(&stream->list);
		kfree(stream);
	}
}

/*
 * Device ops
 */

static int virtio_camera_device_open(struct file *file)
{
	struct virtio_camera_device *vcd = video_drvdata(file);
	struct video_device *video_dev = video_devdata(file);
	struct virtio_camera_fh *vfh;
	int ret;

	vfh = kzalloc(sizeof(*vfh), GFP_KERNEL);
	if (vfh == NULL) {
		return -ENOMEM;
	}

	//TODO: Add multistream support.
	vfh->stream = list_first_entry_or_null(&vcd->camera_sensor.streams_list,
					       struct virtio_camera_stream,
					       list);
	if (!vfh->stream) {
		v4l2_err(&vcd->v4l2_dev, "Could not find available stream");
		ret = -ENOENT;
		goto error;
	}

	mutex_lock(video_dev->lock);
	v4l2_fh_init(&vfh->fh, video_dev);
	file->private_data = &vfh->fh;
	v4l2_fh_add(&vfh->fh);
	mutex_unlock(video_dev->lock);

	return 0;

error:
	kfree(vfh);
	return ret;
}

static int virtio_camera_device_release(struct file *file)
{
	struct virtio_camera_device *vcd = video_drvdata(file);
	struct video_device *video_dev = video_devdata(file);
	struct virtio_camera_fh *vfh =
		container_of(file->private_data, struct virtio_camera_fh, fh);

	mutex_lock(video_dev->lock);

	if (file->private_data == video_dev->queue->owner)
		virtio_camera_vb2_queue_deinit(vcd);

	mutex_unlock(video_dev->lock);

	v4l2_fh_del(&vfh->fh);
	v4l2_fh_exit(&vfh->fh);
	kfree(vfh);
	file->private_data = NULL;

	return 0;
}

static const struct v4l2_file_operations virtio_camera_device_fops = {
	.owner = THIS_MODULE,
	.open = virtio_camera_device_open,
	.release = virtio_camera_device_release,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
};

int virtio_camera_device_init(struct virtio_camera_device *vcd)
{
	int ret;
	struct video_device *vd = &vcd->video_dev;
	struct virtio_camera_stream *stream;

	INIT_LIST_HEAD(&vcd->camera_sensor.streams_list);
	spin_lock_init(&vcd->resource_idr_lock);
	idr_init(&vcd->resource_idr);

	/* Fetch camera info from device side */
	ret = virtio_camera_cmd_get_camera_info(vcd);
	if (ret < 0) {
		v4l2_err(&vcd->v4l2_dev, "failed to get camera info");
		return ret;
	}

	ret = virtio_camera_cmd_enum_camera_ctrls(vcd);
	if (ret < 0) {
		v4l2_err(&vcd->v4l2_dev, "failed to enum camera ctrls\n");
		return ret;
	}

	list_for_each_entry(stream, &vcd->camera_sensor.streams_list, list) {
		/* Create a stream. The function/command only effective purpose
		 * now is to get stream's format info size for
		 * virtio_camera_cmd_stream_create(). Its purpose will expand
		 * in future.
		 *
		 * STREAM_DESTROY counterpart is left undone since it doesn't
		 * have much of use without multistreaming support feature
		 * clearly defined and implemented. */
		ret = virtio_camera_cmd_stream_create(vcd, stream);
		if (ret < 0) {
			v4l2_err(&vcd->v4l2_dev, "failed to create stream\n");
			goto error;
		}

		/* Fetch all available stream formats and get current stream format */
		ret = virtio_camera_cmd_get_stream_formats(vcd, stream);
		if (ret < 0) {
			v4l2_err(&vcd->v4l2_dev,
				 "failed to get stream formats");
			goto error;
		}

		ret = virtio_camera_vb2_queue_init(vcd, stream);
		if (ret < 0) {
			v4l2_err(&vcd->v4l2_dev, "failed to init vb2_queue");
			goto error;
		}
		vd->queue = &vcd->vb_q;
	}

	/* Initialize video_device */
	INIT_LIST_HEAD(&vcd->vbq_active);
	spin_lock_init(&vcd->vbq_lock);

	mutex_init(&vcd->video_dev_mutex);
	vd->lock = &vcd->video_dev_mutex;
	vd->v4l2_dev = &vcd->v4l2_dev;
	vd->vfl_dir = VFL_DIR_RX;
	vd->ioctl_ops = &virtio_camera_ioctl_ops;
	vd->fops = &virtio_camera_device_fops;

	vd->device_caps = V4L2_CAP_STREAMING;
	if (vcd->mplane_api)
		vd->device_caps |= V4L2_CAP_VIDEO_CAPTURE_MPLANE;
	else
		vd->device_caps |= V4L2_CAP_VIDEO_CAPTURE;

	vd->release = video_device_release_empty;

	video_set_drvdata(vd, vcd);

	if (strscpy(vd->name, vcd->name, sizeof(vd->name)) < 0)
		v4l2_warn(&vcd->v4l2_dev, "failed to copy device name");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	ret = video_register_device(&vcd->video_dev, VFL_TYPE_VIDEO, -1);
#else
	ret = video_register_device(&vcd->video_dev, VFL_TYPE_GRABBER, -1);
#endif
	if (ret) {
		v4l2_err(&vcd->v4l2_dev, "Failed to register video device");
		goto error;
	}

	dev_info(&vcd->vdev->dev, "Register '%s' as /dev/video%d", vd->name,
		 vd->num);
	return 0;

error:
	virtio_camera_clean(vcd);
	return ret;
}

int virtio_camera_device_deinit(struct virtio_camera_device *vcd)
{
	struct video_device *vd = &vcd->video_dev;

	video_unregister_device(vd);
	virtio_camera_clean(vcd);

	return 0;
}
