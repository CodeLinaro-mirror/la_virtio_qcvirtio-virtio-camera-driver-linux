/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
/* Driver for virtio camera device. */

#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/version.h>
#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
#include <linux/dma-map-ops.h>
#endif

#include "virtio_camera.h"

static bool mplane_api;
module_param(mplane_api, bool, 0644);
MODULE_PARM_DESC(mplane_api,
		 "0 (default) - single planar API, 1 - multiplanar API");

/*
 * Perform virtio camera protocol specific initialization and
 * register video device.
 */
static int viocam_device_init(struct viocam_device *vcd)
{
	int ret;
	struct viocam_stream *stream = &vcd->stream;
	struct video_device *vd = &stream->video_dev;
	uint32_t ctrl_info_len, fmt_info_len, min_bufs_needed;

	static_assert(sizeof(vd->name) >= sizeof(vcd->name));

	ida_init(&vcd->resource_ida);

	ret = virtio_camera_cmd_get_camera_info(vcd, &ctrl_info_len,
						&min_bufs_needed);
	if (ret < 0)
		return ret;

	ret = virtio_camera_cmd_enum_camera_ctrls(vcd, ctrl_info_len);
	if (ret < 0)
		return ret;

	/* Create a stream. The function/command only purpose is to get stream's
	 * format info size for virtio_camera_cmd_get_stream_formats(). It may
	 * do more in future. Or not.
	 *
	 * STREAM_DESTROY counterpart is left undone since it doesn't
	 * have much of use without multistreaming support feature
	 * clearly defined and implemented.
	 */
	ret = virtio_camera_cmd_stream_create(vcd, &fmt_info_len);
	if (ret < 0)
		goto err_stream_create;

	ret = virtio_camera_cmd_get_stream_formats(vcd, fmt_info_len,
						   &stream->curr_fmt,
						   &stream->formats,
						   &stream->nformats);
	if (ret < 0)
		goto err_stream_create;

	ret = viocam_vb2_queue_init(vcd, stream, min_bufs_needed);
	if (ret < 0) {
		v4l2_err(&vcd->v4l2_dev, "failed to init vb2 queue: %d", ret);
		goto err_stream_create;
	}
	vd->queue = &stream->vb_q;

	mutex_init(&stream->video_dev_mutex);
	vd->lock = &stream->video_dev_mutex;
	vd->v4l2_dev = &vcd->v4l2_dev;
	vd->vfl_dir = VFL_DIR_RX;
	vd->ioctl_ops = &viocam_ioctl_ops;
	vd->fops = &viocam_fops;

	vd->device_caps = V4L2_CAP_STREAMING;
	if (vcd->mplane_api)
		vd->device_caps |= V4L2_CAP_VIDEO_CAPTURE_MPLANE;
	else
		vd->device_caps |= V4L2_CAP_VIDEO_CAPTURE;

	vd->release = video_device_release_empty;

	video_set_drvdata(vd, vcd);

	strscpy(vd->name, vcd->name, sizeof(vd->name));

#if KERNEL_VERSION(5, 7, 0) <= LINUX_VERSION_CODE
	ret = video_register_device(&stream->video_dev, VFL_TYPE_VIDEO, -1);
#else
	ret = video_register_device(&stream->video_dev, VFL_TYPE_GRABBER, -1);
#endif
	if (ret) {
		v4l2_err(&vcd->v4l2_dev, "Failed to register video device: %d",
			 ret);
		goto err_register;
	}

	return 0;

err_register:
	vb2_queue_release(&stream->vb_q);
err_stream_create:
	v4l2_ctrl_handler_free(&vcd->ctrls.hdl);
	return ret;
}

static void viocam_device_deinit(struct viocam_device *vcd)
{
	/* FIXME: replace the sequence below with vb2_video_unregister_device()
	 * once the call is allowed by Android GKI.
	 */
	get_device(&vcd->stream.video_dev.dev);

	video_unregister_device(&vcd->stream.video_dev);
	mutex_lock(vcd->stream.video_dev.queue->lock);
	vb2_queue_release(vcd->stream.video_dev.queue);
	vcd->stream.video_dev.queue->owner = NULL;
	mutex_unlock(vcd->stream.video_dev.queue->lock);

	put_device(&vcd->stream.video_dev.dev);

	v4l2_ctrl_handler_free(&vcd->ctrls.hdl);
}

static int viocam_probe(struct virtio_device *vdev)
{
	int ret;
	struct viocam_device *vcd;
	struct device *dev = &vdev->dev;
	struct device *pdev = dev->parent;

	struct virtqueue *vqs[2];
#if KERNEL_VERSION(6, 11, 0) <= LINUX_VERSION_CODE
	struct virtqueue_info vqs_info[] = {
		{ "cmd_vq", viocam_cmd_cb },
		{ "event_vq", viocam_event_cb },
	};
#else
	static const char *const names[] = { "cmd_vq", "event_vq" };
	static vq_callback_t *callbacks[] = { viocam_cmd_cb, viocam_event_cb };
#endif

	vcd = devm_kzalloc(dev, sizeof(*vcd), GFP_KERNEL);
	if (!vcd)
		return -ENOMEM;

	vdev->priv = vcd;

#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
	vcd->has_iommu = !virtio_has_dma_quirk(vdev);
#else
	vcd->has_iommu = !virtio_has_iommu_quirk(vdev);
#endif
	vcd->mplane_api = mplane_api;

	dev->iommu_group = pdev->iommu_group;
#if KERNEL_VERSION(5, 7, 0) > LINUX_VERSION_CODE
	dev->iommu_fwspec = pdev->iommu_fwspec;
#endif
#if KERNEL_VERSION(5, 6, 0) <= LINUX_VERSION_CODE
	dev->iommu = pdev->iommu;
#elif KERNEL_VERSION(5, 2, 0) <= LINUX_VERSION_CODE
	dev->iommu_param = pdev->iommu_param;
#endif

	if (!dev->dma_ops)
		set_dma_ops(dev, pdev->dma_ops);

	/*
	 * Set it to coherent_dma_mask by default if the architecture
	 * code has not set it.
	 */
	if (!dev->dma_mask)
		dev->dma_mask = &dev->coherent_dma_mask;

	dma_set_mask(dev, *pdev->dma_mask);

	virtio_cread_bytes(vdev, offsetof(struct virtio_camera_config, name),
			   &vcd->name, sizeof(vcd->name));

	ret = v4l2_device_register(dev, &vcd->v4l2_dev);
	if (ret)
		return ret;

#if KERNEL_VERSION(6, 11, 0) <= LINUX_VERSION_CODE
	ret = virtio_find_vqs(vdev, 2, vqs, vqs_info, NULL);
#else
	ret = virtio_find_vqs(vdev, 2, vqs, callbacks, names, NULL);
#endif
	if (ret) {
		v4l2_err(&vcd->v4l2_dev, "failed to find virt queues\n");
		goto err_find_vq;
	}

	spin_lock_init(&vcd->cmd_vq.qlock);

	vcd->cmd_vq.vq = vqs[0];
	vcd->event_vq.vq = vqs[1];

	virtio_device_ready(vdev);

	ret = viocam_device_init(vcd);
	if (ret)
		goto err_init_camera;

	dev_info(&vdev->dev, "Register '%s' as /dev/video%d",
		 vcd->stream.video_dev.name, vcd->stream.video_dev.num);

	return 0;

err_init_camera:
#if KERNEL_VERSION(5, 17, 0) <= LINUX_VERSION_CODE
	virtio_reset_device(vdev);
#else
	vdev->config->reset(vdev);
#endif
	vdev->config->del_vqs(vdev);
err_find_vq:
	v4l2_device_unregister(&vcd->v4l2_dev);
	return ret;
}

static void viocam_remove(struct virtio_device *vdev)
{
	struct viocam_device *vcd = vdev->priv;

	viocam_device_deinit(vcd);

#if KERNEL_VERSION(5, 17, 0) <= LINUX_VERSION_CODE
	virtio_reset_device(vdev);
#else
	vdev->config->reset(vdev);
#endif
	vdev->config->del_vqs(vdev);

	v4l2_device_unregister(&vcd->v4l2_dev);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_CAMERA, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {

};

static struct virtio_driver viocam_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name = KBUILD_MODNAME,
	.driver.owner = THIS_MODULE,
	.id_table = id_table,
	.probe = viocam_probe,
	.remove = viocam_remove,
};

module_virtio_driver(viocam_driver);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("virtio camera driver");
MODULE_AUTHOR("Qualcomm Technologies, Inc. and/or its subsidiaries");
MODULE_LICENSE("GPL");
