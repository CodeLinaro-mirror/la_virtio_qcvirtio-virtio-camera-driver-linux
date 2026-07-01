/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: ((GPL-2.0-only WITH Linux-syscall-note) OR BSD-3-Clause-Clear)
 */
/*  Virtio Camera Device */

#ifndef _UAPI_VIRTIO_CAMERA_H
#define _UAPI_VIRTIO_CAMERA_H

#include <linux/types.h>

/* Support of vendor virtqueues */
#define VIRTIO_CAMERA_F_VENDOR 2

/*
 * Commands
 */

enum virtio_camera_cmd_type {
	VIRTIO_CAMERA_CMD_GET_CAMERA_INFO = 0x0100,
	VIRTIO_CAMERA_CMD_ENUM_CAMERA_CTRLS,
	VIRTIO_CAMERA_CMD_GET_STREAM_FORMATS,
	VIRTIO_CAMERA_CMD_QUERY_CAPABILITY,
	VIRTIO_CAMERA_CMD_STREAM_CREATE,
	VIRTIO_CAMERA_CMD_STREAM_DESTROY,
	VIRTIO_CAMERA_CMD_RESOURCE_CREATE,
	VIRTIO_CAMERA_CMD_RESOURCE_QUEUE,
	VIRTIO_CAMERA_CMD_RESOURCE_DESTROY,
	VIRTIO_CAMERA_CMD_RESOURCE_FLUSH,
	VIRTIO_CAMERA_CMD_GET_CONTROL,
	VIRTIO_CAMERA_CMD_SET_CONTROL,
	VIRTIO_CAMERA_CMD_GET_FORMAT,
	VIRTIO_CAMERA_CMD_SET_FORMAT,
};

enum virtio_camera_result {
	/* Success */
	VIRTIO_CAMERA_RESULT_OK = 0x0,

	/* Error */
	VIRTIO_CAMERA_RESULT_ERR_INVALID_OPERATION = 0x0100,
	VIRTIO_CAMERA_RESULT_ERR_INVALID_STREAM_ID,
	VIRTIO_CAMERA_RESULT_ERR_INVALID_RESOURCE_ID,
};

struct virtio_camera_config {
	char name[32];
};

struct virtio_camera_cmd_hdr {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__u8 padding[4];
};

struct virtio_camera_cmd_resp_hdr {
	__le32 result; /* VIRTIO_CAMERA_RESULT_* */
	__u8 padding[4];
};

/* VIRTIO_CAMERA_CMD_GET_CAMERA_INFO */
struct virtio_camera_get_camera_info {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__u8 padding[4];
};

struct virtio_camera_enum_camera_ctrls {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__u8 padding[4];
};

struct virtio_camera_ctrl_entry {
	__le32 ctrl_id;
	__le32 type;
	__le32 min;
	__le32 max;
	__le32 step;
	__le32 def;
	__u8 name[32];
};

struct virtio_camera_get_camera_info_resp {
	__le32 result; /* VIRTIO_CAMERA_RESULT_* */
	__le32 min_buf_needed;
	__le32 stream_cnt;
	__le32 ctrl_info_len;
};

struct virtio_camera_enum_camera_ctrls_resp {
	__le32 result;
	__le32 num_ctrl_entries;
	struct virtio_camera_ctrl_entry ctrl_entries[];
};

/* VIRTIO_CAMERA_CMD_GET_STREAM_CREATE */
struct virtio_camera_stream_create {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__le32 stream_id;
};

struct virtio_camera_stream_create_resp {
	__le32 result;
	__le32 fmt_info_len;
};

/* VIRTIO_CAMERA_CMD_GET_STREAM_FORMATS */
struct virtio_camera_get_stream_formats {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__le32 stream_id;
};

struct virtio_camera_plane_format {
	__le32 sizeimage;
	__le32 bytesperline;
};

struct virtio_camera_format_range {
	__le32 min;
	__le32 max;
	__le32 step;
	__u8 padding[4];
};

struct virtio_camera_stream_format_frame {
	struct virtio_camera_format_range width;
	struct virtio_camera_format_range height;
};

#define VIRTIO_CAMERA_MAX_PLANES 8

struct virtio_camera_stream_format {
	__le32 format;
	__le32 width;
	__le32 height;
	__le32 num_planes;
	struct virtio_camera_plane_format plane_fmt[VIRTIO_CAMERA_MAX_PLANES];
};

struct virtio_camera_stream_format_entry {
	__le32 format;
	__le32 num_frames;
	/* Followed by struct virtio_camera_stream_format_frame[] */
};

struct virtio_camera_get_stream_formats_resp {
	__le32 result;
	__le32 num_entries;
	struct virtio_camera_stream_format stream_fmt;
	struct virtio_camera_stream_format_entry fmt_entries[];
};

/* VIRTIO_CAMERA_CMD_RESOURCE_CREATE */
struct virtio_camera_resource_create {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__le32 stream_id;
	__le32 resource_id;
	__u8 padding[4];
};

struct virtio_camera_resource_sg_entry {
	__le64 addr;
	__le32 length;
	__u8 padding[4];
};

struct virtio_camera_resource_sg_list {
	__le32 num_entries;
	__u8 padding[4];
	struct virtio_camera_resource_sg_entry entries[];
};

#define VIRTIO_CAMERA_RESOURCE_SG_SIZE(n) \
	offsetof(struct virtio_camera_resource_sg_list, entries[n])

struct virtio_camera_resource_create_resp {
	__le32 result; /* VIRTIO_CAMERA_RESULT_* */
	__u8 padding[4];
};

/* VIRTIO_CAMERA_CMD_RESOURCE_QUEUE */
struct virtio_camera_resource_queue {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__le32 stream_id;
	__le32 resource_id;
	__u8 padding[4];
};

enum virtio_camera_resource_queue_status {
	VIRTIO_CAMERA_RESOURCE_QUEUE_OK,
	VIRTIO_CAMERA_RESOURCE_QUEUE_ERR,
};

struct virtio_camera_resource_queue_resp {
	__le32 result; /* VIRTIO_CAMERA_RESULT_* */
	__le32 status; /* one of enum virtio_camera_resource_queue_status */
	__le64 timestamp;
};

/* VIRTIO_CAMERA_CMD_RESOURCE_DESTROY */
struct virtio_camera_resource_destroy {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__le32 stream_id;
};

struct virtio_camera_resource_destroy_resp {
	__le32 result; /* VIRTIO_CAMERA_RESULT_* */
	__u8 padding[4];
};

/* VIRTIO_CAMERA_CMD_RESOURCE_FLUSH */
struct virtio_camera_resource_flush {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__le32 stream_id;
};

struct virtio_camera_resource_flush_resp {
	__le32 result; /* VIRTIO_CAMERA_RESULT_* */
	__u8 padding[4];
};

/* VIRTIO_CAMERA_CMD_SET_CONTROL */
struct virtio_camera_set_control {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__le32 stream_id;
	__le32 ctrl_id;
	__le32 value;
};

struct virtio_camera_set_control_resp {
	__le32 result; /* VIRTIO_CAMERA_RESULT_* */
	__u8 padding[4];
};

/* VIRTIO_CAMERA_CMD_GET_FORMAT */
struct virtio_camera_get_format {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__le32 stream_id;
};

struct virtio_camera_get_format_resp {
	__le32 result; /* VIRTIO_CAMERA_RESULT_* */
	__u8 padding[4];
	struct virtio_camera_stream_format fmt;
};

/* VIRTIO_CAMERA_CMD_SET_FORMAT */
struct virtio_camera_set_format {
	__le32 cmd_type; /* VIRTIO_CAMERA_CMD_* */
	__le32 stream_id;
	struct virtio_camera_stream_format fmt;
};

struct virtio_camera_set_format_resp {
	__le32 result; /* VIRTIO_CAMERA_RESULT_* */
	__u8 padding[4];
	struct virtio_camera_stream_format fmt;
};

/*
 * Events
 */

enum virtio_camera_event_type {
	VIRTIO_CAMERA_EVENT_ERROR = 0x0100,
	VIRTIO_CAMERA_EVENT_CAMERA_HOTPLUGGED = 0x0200,
	VIRTIO_CAMERA_EVENT_CAMERA_UNPLUGGED,
};

struct virtio_camera_event {
	__le32 event_type;
	__u8 padding[4];
};

#endif /* _UAPI_VIRTIO_CAMERA_H */
