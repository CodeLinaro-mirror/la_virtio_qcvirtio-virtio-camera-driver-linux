/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
/* Driver for virtio camera device. */

#include <linux/module.h>
#include <linux/virtio_config.h>
#include <linux/wait.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#include <linux/dma-map-ops.h>
#endif

#include "virtio_camera.h"

static bool mplane_api = false;
module_param(mplane_api, bool, 0644);
MODULE_PARM_DESC(mplane_api,
		 "0 (default) - single planar API, 1 - multiplanar API");

static int virtio_camera_probe(struct virtio_device *vdev)
{
	int ret;
	struct virtio_camera_device *vcd;
	struct device *dev = &vdev->dev;
	struct device *pdev = dev->parent;

	struct virtqueue *vqs[2];
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	struct virtqueue_info vqs_info[] = {
		{ "cmd_vq", virtio_camera_cmd_cb },
		{ "event_vq", virtio_camera_event_cb },
	};
#else
	static const char *const names[] = { "cmd_vq", "event_vq" };
	static vq_callback_t *callbacks[] = { virtio_camera_cmd_cb,
					      virtio_camera_event_cb };
#endif
	dev_dbg(dev, "%s()\n", __func__);

	vcd = devm_kzalloc(dev, sizeof(*vcd), GFP_KERNEL);
	if (!vcd)
		return -ENOMEM;

	vcd->vdev = vdev;
	vdev->priv = vcd;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
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
		goto error;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	ret = virtio_find_vqs(vdev, 2, vqs, vqs_info, NULL);
#else
	ret = virtio_find_vqs(vdev, 2, vqs, callbacks, names, NULL);
#endif
	if (ret) {
		v4l2_err(&vcd->v4l2_dev, "failed to find virt queues\n");
		goto error;
	}

	spin_lock_init(&vcd->cmd_vq.qlock);
	spin_lock_init(&vcd->event_vq.qlock);
	init_waitqueue_head(&vcd->cmd_vq.reclaim_queue);
	INIT_WORK(&vcd->event_vq.reclaim_work, virtio_camera_process_events);

	vcd->cmd_vq.vq = vqs[0];
	vcd->event_vq.vq = vqs[1];

	INIT_LIST_HEAD(&vcd->pending_vbuf_list);

	ret = virtio_camera_alloc_vbufs(vcd);
	if (ret) {
		v4l2_err(&vcd->v4l2_dev, "failed to alloc vbufs\n");
		goto failed_alloc_vbufs;
	}

	ret = virtio_camera_alloc_events(vcd);
	if (ret) {
		v4l2_err(&vcd->v4l2_dev, "failed to alloc events");
		goto failed_alloc_events;
	}

	virtio_device_ready(vdev);

	ret = virtio_camera_device_init(vcd);
	if (ret)
		goto failed_init_camera;

	return 0;

failed_init_camera:
	virtio_camera_free_events(vcd);
failed_alloc_events:
	virtio_camera_free_vbufs(vcd);
failed_alloc_vbufs:
	vdev->config->del_vqs(vdev);
error:
	devm_kfree(&vdev->dev, vcd);
	return ret;
}

static void virtio_camera_remove(struct virtio_device *vdev)
{
	struct virtio_camera_device *vcd = vdev->priv;

	virtio_camera_free_vbufs(vcd);
	virtio_camera_free_events(vcd);
	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);

	v4l2_device_unregister(&vcd->v4l2_dev);
	virtio_camera_device_deinit(vcd);

	devm_kfree(&vdev->dev, vcd);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_CAMERA, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {

};

static struct virtio_driver virtio_camera_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name = KBUILD_MODNAME,
	.driver.owner = THIS_MODULE,
	.id_table = id_table,
	.probe = virtio_camera_probe,
	.remove = virtio_camera_remove,
};

module_virtio_driver(virtio_camera_driver);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("virtio camera driver");
MODULE_AUTHOR("Qualcomm Technologies, Inc. and/or its subsidiaries");
MODULE_LICENSE("GPL");
