/*
 * v4l2_mmal - hooking together V4L2 and MMAL.
 * Copyright (C) 2018 Raspberry Pi (Trading) Ltd.
 * 
 * Based on yavta -  Yet Another V4L2 Test Application
 * Copyright (C) 2005-2010 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 */

#define __STDC_FORMAT_MACROS

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <linux/videodev2.h>

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "bcm_host.h"
#include "user-vcsm.h"

#define MAX_COMPONENTS 4

struct destinations {
	char *component_name;
	MMAL_FOURCC_T output_encoding;
	MMAL_PORT_BH_CB_T cb;
};

static void encoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
#define MMAL_ENCODING_UNUSED 0

struct destinations dests[MAX_COMPONENTS] = {
	{"vc.ril.video_encode", MMAL_ENCODING_H264, encoder_buffer_callback},
	{"vc.ril.image_encode", MMAL_ENCODING_JPEG, encoder_buffer_callback},
	{"vc.ril.video_render", MMAL_ENCODING_UNUSED, NULL},
	{NULL, MMAL_ENCODING_UNUSED, NULL}
};


#ifndef V4L2_BUF_FLAG_ERROR
#define V4L2_BUF_FLAG_ERROR	0x0040
#endif

#define ARRAY_SIZE(a)	(sizeof(a)/sizeof((a)[0]))

int debug = 1;
#define print(...) do { if (debug) printf(__VA_ARGS__); }  while (0)

struct buffer
{
	unsigned int idx;
	unsigned int padding[VIDEO_MAX_PLANES];
	unsigned int size[VIDEO_MAX_PLANES];
	void *mem[VIDEO_MAX_PLANES];
	MMAL_BUFFER_HEADER_T *mmal;
	int dma_fd;
	unsigned int vcsm_handle;
};

struct component {
	MMAL_COMPONENT_T *comp;
	MMAL_POOL_T *ip_pool;
	MMAL_POOL_T *op_pool;
	FILE *stream_fd;
	FILE *pts_fd;

	VCOS_THREAD_T save_thread;
	MMAL_QUEUE_T *save_queue;
	int thread_quit;
};

struct device
{
	int fd;
	int opened;

	unsigned int nbufs;
	struct buffer *buffers;

	MMAL_COMPONENT_T *isp;
	MMAL_POOL_T *isp_output_pool;

	struct component components[MAX_COMPONENTS];

	/* V4L2 to MMAL interface */
	MMAL_QUEUE_T *isp_queue;
	MMAL_POOL_T *mmal_pool;
	/* Encoded data */
	MMAL_POOL_T *output_pool;

	MMAL_BOOL_T can_zero_copy;

	unsigned int width;
	unsigned int height;
	unsigned int fps;
	unsigned int frame_time_usec;
	uint32_t buffer_output_flags;
	uint32_t timestamp_type;
	struct timeval starttime;
	int64_t lastpts;

	unsigned char num_planes;

	void *pattern[VIDEO_MAX_PLANES];

	bool write_data_prefix;
};

static void errno_exit(const char *s)
{
        fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
        exit(EXIT_FAILURE);
}

static struct v4l2_format_info {
	const char *name;
	unsigned int fourcc;
	unsigned char n_planes;
	MMAL_FOURCC_T mmal_encoding;
} pixel_formats[] = {
	{ "RGB332", V4L2_PIX_FMT_RGB332, 1, 	MMAL_ENCODING_UNUSED },
	{ "RGB444", V4L2_PIX_FMT_RGB444, 1,	MMAL_ENCODING_UNUSED },
	{ "ARGB444", V4L2_PIX_FMT_ARGB444, 1,	MMAL_ENCODING_UNUSED },
	{ "XRGB444", V4L2_PIX_FMT_XRGB444, 1,	MMAL_ENCODING_UNUSED },
	{ "RGB555", V4L2_PIX_FMT_RGB555, 1,	MMAL_ENCODING_UNUSED },
	{ "ARGB555", V4L2_PIX_FMT_ARGB555, 1,	MMAL_ENCODING_UNUSED },
	{ "XRGB555", V4L2_PIX_FMT_XRGB555, 1,	MMAL_ENCODING_UNUSED },
	{ "RGB565", V4L2_PIX_FMT_RGB565, 1,	MMAL_ENCODING_UNUSED },
	{ "RGB555X", V4L2_PIX_FMT_RGB555X, 1,	MMAL_ENCODING_UNUSED },
	{ "RGB565X", V4L2_PIX_FMT_RGB565X, 1,	MMAL_ENCODING_RGB16 },
	{ "BGR666", V4L2_PIX_FMT_BGR666, 1,	MMAL_ENCODING_UNUSED },
	{ "BGR24", V4L2_PIX_FMT_BGR24, 1,	MMAL_ENCODING_RGB24 },
	{ "RGB24", V4L2_PIX_FMT_RGB24, 1,	MMAL_ENCODING_BGR24 },
	{ "BGR32", V4L2_PIX_FMT_BGR32, 1,	MMAL_ENCODING_BGR32 },
	{ "ABGR32", V4L2_PIX_FMT_ABGR32, 1,	MMAL_ENCODING_BGRA },
	{ "XBGR32", V4L2_PIX_FMT_XBGR32, 1,	MMAL_ENCODING_BGR32 },
	{ "RGB32", V4L2_PIX_FMT_RGB32, 1,	MMAL_ENCODING_RGB32 },
	{ "ARGB32", V4L2_PIX_FMT_ARGB32, 1,	MMAL_ENCODING_ARGB },
	{ "XRGB32", V4L2_PIX_FMT_XRGB32, 1,	MMAL_ENCODING_UNUSED },
	{ "HSV24", V4L2_PIX_FMT_HSV24, 1,	MMAL_ENCODING_UNUSED },
	{ "HSV32", V4L2_PIX_FMT_HSV32, 1,	MMAL_ENCODING_UNUSED },
	{ "Y8", V4L2_PIX_FMT_GREY, 1,		MMAL_ENCODING_UNUSED },
	{ "Y10", V4L2_PIX_FMT_Y10, 1,		MMAL_ENCODING_UNUSED },
	{ "Y12", V4L2_PIX_FMT_Y12, 1,		MMAL_ENCODING_UNUSED },
	{ "Y16", V4L2_PIX_FMT_Y16, 1,		MMAL_ENCODING_UNUSED },
	{ "UYVY", V4L2_PIX_FMT_UYVY, 1,		MMAL_ENCODING_UYVY },
	{ "VYUY", V4L2_PIX_FMT_VYUY, 1,		MMAL_ENCODING_VYUY },
	{ "YUYV", V4L2_PIX_FMT_YUYV, 1,		MMAL_ENCODING_YUYV },
	{ "YVYU", V4L2_PIX_FMT_YVYU, 1,		MMAL_ENCODING_YVYU },
	{ "NV12", V4L2_PIX_FMT_NV12, 1,		MMAL_ENCODING_NV12 },
	{ "NV12M", V4L2_PIX_FMT_NV12M, 2,	MMAL_ENCODING_UNUSED },
	{ "NV21", V4L2_PIX_FMT_NV21, 1,		MMAL_ENCODING_NV21 },
	{ "NV21M", V4L2_PIX_FMT_NV21M, 2,	MMAL_ENCODING_UNUSED },
	{ "NV16", V4L2_PIX_FMT_NV16, 1,		MMAL_ENCODING_UNUSED },
	{ "NV16M", V4L2_PIX_FMT_NV16M, 2,	MMAL_ENCODING_UNUSED },
	{ "NV61", V4L2_PIX_FMT_NV61, 1,		MMAL_ENCODING_UNUSED },
	{ "NV61M", V4L2_PIX_FMT_NV61M, 2,	MMAL_ENCODING_UNUSED },
	{ "NV24", V4L2_PIX_FMT_NV24, 1,		MMAL_ENCODING_UNUSED },
	{ "NV42", V4L2_PIX_FMT_NV42, 1,		MMAL_ENCODING_UNUSED },
	{ "YUV420M", V4L2_PIX_FMT_YUV420M, 3,	MMAL_ENCODING_UNUSED },
	{ "YUV422M", V4L2_PIX_FMT_YUV422M, 3,	MMAL_ENCODING_UNUSED },
	{ "YUV444M", V4L2_PIX_FMT_YUV444M, 3,	MMAL_ENCODING_UNUSED },
	{ "YVU420M", V4L2_PIX_FMT_YVU420M, 3,	MMAL_ENCODING_UNUSED },
	{ "YVU422M", V4L2_PIX_FMT_YVU422M, 3,	MMAL_ENCODING_UNUSED },
	{ "YVU444M", V4L2_PIX_FMT_YVU444M, 3,	MMAL_ENCODING_UNUSED },
	{ "SBGGR8", V4L2_PIX_FMT_SBGGR8, 1,	MMAL_ENCODING_BAYER_SBGGR8 },
	{ "SGBRG8", V4L2_PIX_FMT_SGBRG8, 1,	MMAL_ENCODING_BAYER_SGBRG8 },
	{ "SGRBG8", V4L2_PIX_FMT_SGRBG8, 1,	MMAL_ENCODING_BAYER_SGRBG8 },
	{ "SRGGB8", V4L2_PIX_FMT_SRGGB8, 1,	MMAL_ENCODING_BAYER_SRGGB8 },
	{ "SBGGR10_DPCM8", V4L2_PIX_FMT_SBGGR10DPCM8, 1,	MMAL_ENCODING_UNUSED },
	{ "SGBRG10_DPCM8", V4L2_PIX_FMT_SGBRG10DPCM8, 1,	MMAL_ENCODING_UNUSED },
	{ "SGRBG10_DPCM8", V4L2_PIX_FMT_SGRBG10DPCM8, 1,	MMAL_ENCODING_UNUSED },
	{ "SRGGB10_DPCM8", V4L2_PIX_FMT_SRGGB10DPCM8, 1,	MMAL_ENCODING_UNUSED },
	{ "SBGGR10", V4L2_PIX_FMT_SBGGR10, 1,	MMAL_ENCODING_UNUSED },
	{ "SGBRG10", V4L2_PIX_FMT_SGBRG10, 1,	MMAL_ENCODING_UNUSED },
	{ "SGRBG10", V4L2_PIX_FMT_SGRBG10, 1,	MMAL_ENCODING_UNUSED },
	{ "SRGGB10", V4L2_PIX_FMT_SRGGB10, 1,	MMAL_ENCODING_UNUSED },
	{ "SBGGR10P", V4L2_PIX_FMT_SBGGR10P, 1,	MMAL_ENCODING_BAYER_SBGGR10P },
	{ "SGBRG10P", V4L2_PIX_FMT_SGBRG10P, 1,	MMAL_ENCODING_BAYER_SGBRG10P },
	{ "SGRBG10P", V4L2_PIX_FMT_SGRBG10P, 1,	MMAL_ENCODING_BAYER_SGRBG10P },
	{ "SRGGB10P", V4L2_PIX_FMT_SRGGB10P, 1,	MMAL_ENCODING_BAYER_SRGGB10P },
	{ "SBGGR12", V4L2_PIX_FMT_SBGGR12, 1,	MMAL_ENCODING_UNUSED },
	{ "SGBRG12", V4L2_PIX_FMT_SGBRG12, 1,	MMAL_ENCODING_UNUSED },
	{ "SGRBG12", V4L2_PIX_FMT_SGRBG12, 1,	MMAL_ENCODING_UNUSED },
	{ "SRGGB12", V4L2_PIX_FMT_SRGGB12, 1,	MMAL_ENCODING_UNUSED },
	{ "DV", V4L2_PIX_FMT_DV, 1,		MMAL_ENCODING_UNUSED },
	{ "MJPEG", V4L2_PIX_FMT_MJPEG, 1,	MMAL_ENCODING_UNUSED },
	{ "MPEG", V4L2_PIX_FMT_MPEG, 1,		MMAL_ENCODING_UNUSED },
};

static void list_formats(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pixel_formats); i++)
		print("%s (\"%c%c%c%c\", %u planes)\n",
		       pixel_formats[i].name,
		       pixel_formats[i].fourcc & 0xff,
		       (pixel_formats[i].fourcc >> 8) & 0xff,
		       (pixel_formats[i].fourcc >> 16) & 0xff,
		       (pixel_formats[i].fourcc >> 24) & 0xff,
		       pixel_formats[i].n_planes);
}

static const struct v4l2_format_info *v4l2_format_by_fourcc(unsigned int fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pixel_formats); ++i) {
		if (pixel_formats[i].fourcc == fourcc)
			return &pixel_formats[i];
	}

	return NULL;
}

static const struct v4l2_format_info *v4l2_format_by_name(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pixel_formats); ++i) {
		if (strcasecmp(pixel_formats[i].name, name) == 0)
			return &pixel_formats[i];
	}

	return NULL;
}

static const struct v4l2_format_info *v4l2_format_by_mmal_encoding(MMAL_FOURCC_T encoding)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pixel_formats); ++i) {
		if (pixel_formats[i].mmal_encoding == encoding)
			return &pixel_formats[i];
	}

	return NULL;
}

static const char *v4l2_format_name(unsigned int fourcc)
{
	const struct v4l2_format_info *info;
	static char name[5];
	unsigned int i;

	info = v4l2_format_by_fourcc(fourcc);
	if (info)
		return info->name;

	for (i = 0; i < 4; ++i) {
		name[i] = fourcc & 0xff;
		fourcc >>= 8;
	}

	name[4] = '\0';
	return name;
}

static const struct {
	const char *name;
	enum v4l2_field field;
} fields[] = {
	{ "any", V4L2_FIELD_ANY },
	{ "none", V4L2_FIELD_NONE },
	{ "top", V4L2_FIELD_TOP },
	{ "bottom", V4L2_FIELD_BOTTOM },
	{ "interlaced", V4L2_FIELD_INTERLACED },
	{ "seq-tb", V4L2_FIELD_SEQ_TB },
	{ "seq-bt", V4L2_FIELD_SEQ_BT },
	{ "alternate", V4L2_FIELD_ALTERNATE },
	{ "interlaced-tb", V4L2_FIELD_INTERLACED_TB },
	{ "interlaced-bt", V4L2_FIELD_INTERLACED_BT },
};

static enum v4l2_field v4l2_field_from_string(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fields); ++i) {
		if (strcasecmp(fields[i].name, name) == 0)
			return fields[i].field;
	}

	return -1;
}

static const char *v4l2_field_name(enum v4l2_field field)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fields); ++i) {
		if (fields[i].field == field)
			return fields[i].name;
	}

	return "unknown";
}

static void video_init(struct device *dev)
{
	memset(dev, 0, sizeof *dev);
	dev->fd = -1;
	dev->buffers = NULL;
}

static bool video_has_fd(struct device *dev)
{
	return dev->fd != -1;
}

static int video_set_fd(struct device *dev, int fd)
{
	if (video_has_fd(dev)) {
		print("Can't set fd (already open).\n");
		return -1;
	}

	dev->fd = fd;

	return 0;
}

static int video_open(struct device *dev, const char *devname)
{
	if (video_has_fd(dev)) {
		print("Can't open device (already open).\n");
		return -1;
	}

	dev->fd = open(devname, O_RDWR);
	if (dev->fd < 0) {
		print("Error opening device %s: %s (%d).\n", devname,
		       strerror(errno), errno);
		return dev->fd;
	}

	print("Device %s opened.\n", devname);

	dev->opened = 1;

	return 0;
}

static int video_querycap(struct device *dev, unsigned int *capabilities)
{
	struct v4l2_capability cap;
	unsigned int caps;
	int ret;

	memset(&cap, 0, sizeof cap);
	ret = ioctl(dev->fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0)
		return 0;

	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
	     ? cap.device_caps : cap.capabilities;

	print("Device `%s' on `%s' (driver '%s') is a video %s (%s mplanes) device.\n",
		cap.card, cap.bus_info, cap.driver,
		caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_CAPTURE) ? "capture" : "output",
		caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE) ? "with" : "without");

	*capabilities = caps;

	return 0;
}

static void video_close(struct device *dev)
{
	unsigned int i;

	for (i = 0; i < dev->num_planes; i++)
		free(dev->pattern[i]);

	free(dev->buffers);
	if (dev->opened)
		close(dev->fd);
}

static void video_log_status(struct device *dev)
{
	ioctl(dev->fd, VIDIOC_LOG_STATUS);
}

static int video_get_format(struct device *dev)
{
	struct v4l2_format fmt;
	int ret;

	memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	ret = ioctl(dev->fd, VIDIOC_G_FMT, &fmt);
	if (ret < 0) {
		print("Unable to get format: %s (%d).\n", strerror(errno),
			errno);
		return ret;
	}

	dev->width = fmt.fmt.pix.width;
	dev->height = fmt.fmt.pix.height;
	dev->num_planes = 1;

	print("Video format: %s (%08x) %ux%u (stride %u) field %s buffer size %u\n",
		v4l2_format_name(fmt.fmt.pix.pixelformat), fmt.fmt.pix.pixelformat,
		fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.bytesperline,
		v4l2_field_name(fmt.fmt.pix_mp.field),
		fmt.fmt.pix.sizeimage);

	return 0;
}

static int format_bpp(__u32 pixfmt)
{
	switch(pixfmt)
	{
		case V4L2_PIX_FMT_BGR24:
		case V4L2_PIX_FMT_RGB24:
			return 4;
		case V4L2_PIX_FMT_YUYV:
		case V4L2_PIX_FMT_YVYU:
		case V4L2_PIX_FMT_UYVY:
		case V4L2_PIX_FMT_VYUY:
			return 2;
		case V4L2_PIX_FMT_SRGGB8:
		case V4L2_PIX_FMT_SBGGR8:
		case V4L2_PIX_FMT_SGRBG8:
		case V4L2_PIX_FMT_SGBRG8:
			return 1;
		default:
			return 1;
	}
}

static int video_set_format(struct device *dev, unsigned int w, unsigned int h,
			    unsigned int format, unsigned int stride,
			    unsigned int buffer_size, enum v4l2_field field,
			    unsigned int flags)
{
	struct v4l2_format fmt;
	int ret;

	memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = format;
	fmt.fmt.pix.field = field;
	print("stride is %d\n",stride);
	if (!stride)
		stride = ((w+31) &~31)*format_bpp(format);
	print("stride is now %d\n",stride);
	fmt.fmt.pix.bytesperline = stride;
	fmt.fmt.pix.sizeimage = buffer_size;
	fmt.fmt.pix.priv = V4L2_PIX_FMT_PRIV_MAGIC;
	fmt.fmt.pix.flags = flags;

	ret = ioctl(dev->fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		print("Unable to set format: %s (%d).\n", strerror(errno),
			errno);
		return ret;
	}

	print("Video format set: %s (%08x) %ux%u (stride %u) field %s buffer size %u\n",
		v4l2_format_name(fmt.fmt.pix.pixelformat), fmt.fmt.pix.pixelformat,
		fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.bytesperline,
		v4l2_field_name(fmt.fmt.pix.field),
		fmt.fmt.pix.sizeimage);

	return 0;
}

static int video_buffer_mmap(struct device *dev, struct buffer *buffer,
			     struct v4l2_buffer *v4l2buf)
{
	unsigned int length;
	unsigned int offset;
	unsigned int i;

	for (i = 0; i < dev->num_planes; i++) {
		length = v4l2buf->length;
		offset = v4l2buf->m.offset;

		buffer->mem[i] = mmap(0, length, PROT_READ | PROT_WRITE, MAP_SHARED,
				      dev->fd, offset);
		if (buffer->mem[i] == MAP_FAILED) {
			print("Unable to map buffer %u/%u: %s (%d)\n",
			       buffer->idx, i, strerror(errno), errno);
			return -1;
		}

		buffer->size[i] = length;

		print("Buffer %u/%u mapped at address %p.\n",
		       buffer->idx, i, buffer->mem[i]);
	}

	return 0;
}

static int video_buffer_munmap(struct device *dev, struct buffer *buffer)
{
	unsigned int i;
	int ret;

	for (i = 0; i < dev->num_planes; i++) {
		ret = munmap(buffer->mem[i], buffer->size[i]);
		if (ret < 0) {
			print("Unable to unmap buffer %u/%u: %s (%d)\n",
			       buffer->idx, i, strerror(errno), errno);
		}

		buffer->mem[i] = NULL;
	}

	return 0;
}

static void get_ts_flags(uint32_t flags, const char **ts_type, const char **ts_source)
{
	switch (flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) {
	case V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN:
		*ts_type = "unk";
		break;
	case V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC:
		*ts_type = "mono";
		break;
	case V4L2_BUF_FLAG_TIMESTAMP_COPY:
		*ts_type = "copy";
		break;
	default:
		*ts_type = "inv";
	}
	switch (flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) {
	case V4L2_BUF_FLAG_TSTAMP_SRC_EOF:
		*ts_source = "EoF";
		break;
	case V4L2_BUF_FLAG_TSTAMP_SRC_SOE:
		*ts_source = "SoE";
		break;
	default:
		*ts_source = "inv";
	}
}

static int video_alloc_buffers(struct device *dev, int nbufs)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer buf;
	struct buffer *buffers;
	unsigned int i;
	int ret;

	memset(&rb, 0, sizeof rb);
	rb.count = nbufs;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;

	ret = ioctl(dev->fd, VIDIOC_REQBUFS, &rb);
	if (ret < 0) {
		print("Unable to request buffers: %s (%d).\n", strerror(errno),
			errno);
		return ret;
	}

	print("%u buffers requested.\n", rb.count);

	buffers = malloc(rb.count * sizeof buffers[0]);
	if (buffers == NULL)
		return -ENOMEM;

	/* Map the buffers. */
	for (i = 0; i < rb.count; ++i) {
		const char *ts_type, *ts_source;

		memset(&buf, 0, sizeof buf);
		memset(planes, 0, sizeof planes);

		buf.index = i;
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.length = VIDEO_MAX_PLANES;
		buf.m.planes = planes;

		ret = ioctl(dev->fd, VIDIOC_QUERYBUF, &buf);
		if (ret < 0) {
			print("Unable to query buffer %u: %s (%d).\n", i,
				strerror(errno), errno);
			return ret;
		}
		get_ts_flags(buf.flags, &ts_type, &ts_source);
		print("length: %u offset: %u timestamp type/source: %s/%s\n",
		       buf.length, buf.m.offset, ts_type, ts_source);

		buffers[i].idx = i;

		ret = video_buffer_mmap(dev, &buffers[i], &buf);
		if (ret < 0)
			return ret;

		if (dev->mmal_pool) {
			struct v4l2_exportbuffer expbuf;
			MMAL_BUFFER_HEADER_T *mmal_buf;

			mmal_buf = mmal_queue_get(dev->mmal_pool->queue);
			if (!mmal_buf) {
				print("Failed to get a buffer from the pool. Queue length %d\n", mmal_queue_length(dev->mmal_pool->queue));
				return -1;
			}
			mmal_buf->user_data = &buffers[i];

			memset(&expbuf, 0, sizeof(expbuf));
			expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			expbuf.index = i;
			if (!ioctl(dev->fd, VIDIOC_EXPBUF, &expbuf)) {
				buffers[i].dma_fd = expbuf.fd;

				buffers[i].vcsm_handle = vcsm_import_dmabuf(expbuf.fd, "V4L2 buf");
			}
			else
			{
				buffers[i].dma_fd = -1;
				buffers[i].vcsm_handle = 0;
			}

			if (buffers[i].vcsm_handle)
			{
				dev->can_zero_copy = MMAL_TRUE;
				print("Exported buffer %d to dmabuf %d, vcsm handle %u\n", i, buffers[i].dma_fd, buffers[i].vcsm_handle);
				mmal_buf->data = (uint8_t*)vcsm_vc_hdl_from_hdl(buffers[i].vcsm_handle);
			}
			else
			{
				dev->can_zero_copy = MMAL_FALSE;
				mmal_buf->data = buffers[i].mem[0];	//Only deal with the single planar API
			}

			mmal_buf->alloc_size = buf.length;
			buffers[i].mmal = mmal_buf;
			print("Linking V4L2 buffer index %d ptr %p to MMAL header %p. mmal->data 0x%X\n",
				i, &buffers[i], mmal_buf, (uint32_t)mmal_buf->data);
			/* Put buffer back in the pool */
			mmal_buffer_header_release(mmal_buf);
		}
	}

	dev->timestamp_type = buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
	dev->buffers = buffers;
	dev->nbufs = rb.count;
	return 0;
}

static int video_free_buffers(struct device *dev)
{
	struct v4l2_requestbuffers rb;
	unsigned int i;
	int ret;

	if (dev->nbufs == 0)
		return 0;

	for (i = 0; i < dev->nbufs; ++i) {
		if (dev->buffers[i].vcsm_handle)
		{
			print("Releasing vcsm handle %u\n", dev->buffers[i].vcsm_handle);
			vcsm_free(dev->buffers[i].vcsm_handle);
		}
		if (dev->buffers[i].dma_fd >= 0)
		{
			print("Closing dma_buf %d\n", dev->buffers[i].dma_fd);
			close(dev->buffers[i].dma_fd);
		}
		ret = video_buffer_munmap(dev, &dev->buffers[i]);
		if (ret < 0)
			return ret;
	}

	memset(&rb, 0, sizeof rb);
	rb.count = 0;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;

	ret = ioctl(dev->fd, VIDIOC_REQBUFS, &rb);
	if (ret < 0) {
		print("Unable to release buffers: %s (%d).\n",
			strerror(errno), errno);
		return ret;
	}

	print("%u buffers released.\n", dev->nbufs);

	free(dev->buffers);
	dev->nbufs = 0;
	dev->buffers = NULL;

	return 0;
}

static int video_queue_buffer(struct device *dev, int index)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	int ret;

	memset(&buf, 0, sizeof buf);
	memset(&planes, 0, sizeof planes);

	buf.index = index;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;

	ret = ioctl(dev->fd, VIDIOC_QBUF, &buf);
	if (ret < 0)
		print("Unable to queue buffer: %s (%d).\n",
			strerror(errno), errno);

	return ret;
}

static int video_enable(struct device *dev, int enable)
{
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int ret;

	ret = ioctl(dev->fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
	if (ret < 0) {
		print("Unable to %s streaming: %s (%d).\n",
			enable ? "start" : "stop", strerror(errno), errno);
		return ret;
	}

	return 0;
}

static int video_prepare_capture(struct device *dev, int nbufs)
{
	int ret;

	/* Allocate and map buffers. */
	if ((ret = video_alloc_buffers(dev, nbufs)) < 0)
		return ret;

	return 0;
}

static int video_queue_all_buffers(struct device *dev)
{
	unsigned int i;
	int ret;

	/* Queue the buffers. */
	for (i = 0; i < dev->nbufs; ++i) {
		ret = video_queue_buffer(dev, i);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void isp_ip_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	struct device *dev = (struct device *)port->userdata;
	unsigned int i;
//	print("Buffer %p (->data %p) returned\n", buffer, buffer->data);
	for (i = 0; i < dev->nbufs; i++) {
		if (dev->buffers[i].mmal == buffer) {
//			print("Matches V4L2 buffer index %d / %d\n", i, dev->buffers[i].idx);
			video_queue_buffer(dev, dev->buffers[i].idx);
			mmal_buffer_header_release(buffer);
			buffer = NULL;
			break;
		}
	}
	if (buffer) {
		print("Failed to find matching V4L2 buffer for mmal buffer %p\n", buffer);
		mmal_buffer_header_release(buffer);
	}
}

static void * save_thread(void *arg)
{
	struct component *comp = (struct component *)arg;
	MMAL_BUFFER_HEADER_T *buffer;
	MMAL_STATUS_T status;
	unsigned int bytes_written;

	while (!comp->thread_quit)
	{
		//Being lazy and using a timed wait instead of setting up a
		//mechanism for skipping this when destroying the thread
		buffer = mmal_queue_timedwait(comp->save_queue, 100);
		if (!buffer)
			continue;

		//print("Buffer %p saving, filled %d, timestamp %llu, flags %04X\n", buffer, buffer->length, buffer->pts, buffer->flags);
		if (comp->stream_fd)
		{
			bytes_written = fwrite(buffer->data, 1, buffer->length, comp->stream_fd);
			fflush(comp->stream_fd);

			if (bytes_written != buffer->length)
			{
				print("Failed to write buffer data (%d from %d)- aborting", bytes_written, buffer->length);
			}
		}

		if (comp->pts_fd &&
		    !(buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) &&
		    buffer->pts != MMAL_TIME_UNKNOWN)
			fprintf(comp->pts_fd, "%lld.%03lld\n", buffer->pts/1000, buffer->pts%1000);

		buffer->length = 0;
		status = mmal_port_send_buffer(comp->comp->output[0], buffer);
		if(status != MMAL_SUCCESS)
		{
			print("mmal_port_send_buffer failed on buffer %p, status %d", buffer, status);
		}
	}
	return NULL;
}

static void encoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	struct component *comp = (struct component *)port->userdata;

	//print("Buffer %p returned, filled %d, timestamp %llu, flags %04X\n", buffer, buffer->length, buffer->pts, buffer->flags);
	//vcos_log_error("File handle: %p", port->userdata);

	if (port->is_enabled)
		mmal_queue_put(comp->save_queue, buffer);
	else
		mmal_buffer_header_release(buffer);
}

/*static void dummy_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	print("Buffer %p returned, filled %d, timestamp %llu, flags %04X\n", buffer, buffer->length, buffer->pts, buffer->flags);

	if (port->is_enabled)
		mmal_port_send_buffer(port, buffer);
	else
		mmal_buffer_header_release(buffer);
}*/

static void buffers_to_isp(struct device *dev)
{
	MMAL_BUFFER_HEADER_T *buffer;

	while ((buffer = mmal_queue_get(dev->isp_output_pool->queue)) != NULL)
	{
		mmal_port_send_buffer(dev->isp->output[0], buffer);
	}

}
static void isp_output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	//print("Buffer %p from isp, filled %d, timestamp %llu, flags %04X\n", buffer, buffer->length, buffer->pts, buffer->flags);
	//vcos_log_error("File handle: %p", port->userdata);
	struct device *dev = (struct device*)port->userdata;
	int i;

	for (i=0; i<MAX_COMPONENTS && dev->components[i].comp; i++)
	{
		MMAL_BUFFER_HEADER_T *out = mmal_queue_get(dev->components[i].ip_pool->queue);
		if (out)
		{
			mmal_buffer_header_replicate(out, buffer);
			mmal_port_send_buffer(dev->components[i].comp->input[0], out);
		}
	}
	mmal_buffer_header_release(buffer);

	buffers_to_isp(dev);
}

static void sink_input_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	//print("Buffer %p returned from %s, filled %d, timestamp %llu, flags %04X\n", buffer, port->name, buffer->length, buffer->pts, buffer->flags);
	//vcos_log_error("File handle: %p", port->userdata);
	struct device *dev = (struct device*)port->userdata;

	mmal_buffer_header_release(buffer);

	buffers_to_isp(dev);
}

#define LOG_DEBUG print

static void dump_port_format(MMAL_ES_FORMAT_T *format)
{
   const char *name_type;

   if (!format)
      return;

   switch(format->type)
   {
   case MMAL_ES_TYPE_AUDIO: name_type = "audio"; break;
   case MMAL_ES_TYPE_VIDEO: name_type = "video"; break;
   case MMAL_ES_TYPE_SUBPICTURE: name_type = "subpicture"; break;
   default: name_type = "unknown"; break;
   }

   LOG_DEBUG("type: %s, fourcc: %4.4s", name_type, (char *)&format->encoding);
   LOG_DEBUG(" bitrate: %i, framed: %i", format->bitrate,
            !!(format->flags & MMAL_ES_FORMAT_FLAG_FRAMED));
   LOG_DEBUG(" extra data: %i, %p", format->extradata_size, format->extradata);
   switch(format->type)
   {
   case MMAL_ES_TYPE_AUDIO:
      LOG_DEBUG(" samplerate: %i, channels: %i, bps: %i, block align: %i",
               format->es->audio.sample_rate, format->es->audio.channels,
               format->es->audio.bits_per_sample, format->es->audio.block_align);
      break;

   case MMAL_ES_TYPE_VIDEO:
      LOG_DEBUG(" width: %i, height: %i, (%i,%i,%i,%i)",
               format->es->video.width, format->es->video.height,
               format->es->video.crop.x, format->es->video.crop.y,
               format->es->video.crop.width, format->es->video.crop.height);
      LOG_DEBUG(" pixel aspect ratio: %i/%i, frame rate: %i/%i",
               format->es->video.par.num, format->es->video.par.den,
               format->es->video.frame_rate.num, format->es->video.frame_rate.den);
      break;

   case MMAL_ES_TYPE_SUBPICTURE:
      break;

   default: break;
   }
}

void mmal_log_dump_port(MMAL_PORT_T *port)
{
   if (!port)
      return;

   LOG_DEBUG("%s(%p)", port->name, port);

   dump_port_format(port->format);

   LOG_DEBUG(" buffers num: %i(opt %i, min %i), size: %i(opt %i, min: %i), align: %i",
            port->buffer_num, port->buffer_num_recommended, port->buffer_num_min,
            port->buffer_size, port->buffer_size_recommended, port->buffer_size_min,
            port->buffer_alignment_min);
}

int video_set_dv_timings(struct device *dev);

static void handle_event(struct device *dev)
{
        struct v4l2_event ev;

        while (!ioctl(dev->fd, VIDIOC_DQEVENT, &ev)) {
            switch (ev.type) {
            case V4L2_EVENT_SOURCE_CHANGE:
                fprintf(stderr, "Source changed\n");

		video_set_dv_timings(dev);
//                stop_capture(V4L2_BUF_TYPE_VIDEO_CAPTURE);
//                unmap_buffers(buffers, n_buffers);

                fprintf(stderr, "Unmapped all buffers\n");
//                free_buffers_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE);

//                init_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE, &buffers, &n_buffers);

//                start_capturing_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE, buffers, n_buffers);
                break;
            case V4L2_EVENT_EOS:
                fprintf(stderr, "EOS\n");
                break;
            }
        }
}

#define MAX_ENCODINGS_NUM 25
typedef struct {
   MMAL_PARAMETER_HEADER_T header;
   MMAL_FOURCC_T encodings[MAX_ENCODINGS_NUM];
} MMAL_SUPPORTED_ENCODINGS_T;


int mmal_dump_supported_formats(MMAL_PORT_T *port)
{
   int new_fw = 0;
   MMAL_STATUS_T ret;

   MMAL_SUPPORTED_ENCODINGS_T sup_encodings = {{MMAL_PARAMETER_SUPPORTED_ENCODINGS, sizeof(sup_encodings)}, {0}};
   ret = mmal_port_parameter_get(port, &sup_encodings.header);
   if (ret == MMAL_SUCCESS || ret == MMAL_ENOSPC)
   {
      //Allow ENOSPC error and hope that the desired formats are in the first
      //MAX_ENCODINGS_NUM entries.
      int i;
      int num_encodings = (sup_encodings.header.size - sizeof(sup_encodings.header)) /
          sizeof(sup_encodings.encodings[0]);
      if(num_encodings > MAX_ENCODINGS_NUM)
         num_encodings = MAX_ENCODINGS_NUM;
      for (i=0; i<num_encodings; i++)
      {
         printf("Format %4.4s\n", (char *)&sup_encodings.encodings[i]);
      }
   }
   return new_fw;
}

static int setup_mmal(struct device *dev, int nbufs, const char *filename)
{
	MMAL_STATUS_T status;
	VCOS_STATUS_T vcos_status;
	MMAL_PORT_T *port;
	const struct v4l2_format_info *info;
	struct v4l2_format fmt;
	int ret, i;
	MMAL_PORT_T *isp_output;

	//FIXME: Clean up after errors

	status = mmal_component_create("vc.ril.isp", &dev->isp);
	if(status != MMAL_SUCCESS)
	{
		print("Failed to create isp\n");
		return -1;
	}

	port = dev->isp->input[0];

	memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	ret = ioctl(dev->fd, VIDIOC_G_FMT, &fmt);
	if (ret < 0) {
		print("Unable to get format: %s (%d).\n", strerror(errno),
			errno);
		return ret;
	}

	info = v4l2_format_by_fourcc(fmt.fmt.pix.pixelformat);
	if (!info || info->mmal_encoding == MMAL_ENCODING_UNUSED)
	{
		print("Unsupported encoding\n");
		return -1;
	}

	port->format->encoding = info->mmal_encoding;
	port->format->es->video.crop.width = fmt.fmt.pix.width;
	port->format->es->video.crop.height = fmt.fmt.pix.height;
	port->format->es->video.width = (port->format->es->video.crop.width+31) & ~31;
	//mmal_encoding_stride_to_width(port->format->encoding, fmt.fmt.pix.bytesperline);
	/* FIXME - buffer may not be aligned vertically */
	port->format->es->video.height = (fmt.fmt.pix.height+15) & ~15;	
	//Ignore for now, but will be wanted for video encode.
	//port->format->es->video.frame_rate.num = 10000;
	//port->format->es->video.frame_rate.den = frame_interval ? frame_interval : 10000;
	port->buffer_num = nbufs;
	if (dev->fps) {
		dev->frame_time_usec = 1000000/dev->fps;
	}

	status = mmal_port_format_commit(port);
	if (status != MMAL_SUCCESS)
	{
		print("Commit failed\n");
		return -1;
	}
	mmal_log_dump_port(port);

	unsigned int mmal_stride = mmal_encoding_width_to_stride(info->mmal_encoding, port->format->es->video.width);
	if (mmal_stride != fmt.fmt.pix.bytesperline) {
		if (video_set_format(dev, fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat, mmal_stride,
				     fmt.fmt.pix.sizeimage, fmt.fmt.pix.field, fmt.fmt.pix.flags) < 0) 
			print("Failed to adjust stride\n");
		else
			// Retrieve settings again so local state is correct
			video_get_format(dev);
	}


	dev->mmal_pool = mmal_pool_create(nbufs, 0);
	if (!dev->mmal_pool) {
		print("Failed to create pool\n");
		return -1;
	}
	print("Created pool of length %d, size %d\n", nbufs, 0);

	port->userdata = (struct MMAL_PORT_USERDATA_T *)dev;

	/* Setup ISP output */
	isp_output = dev->isp->output[0];
	mmal_format_copy(isp_output->format, port->format);
	isp_output = dev->isp->output[0];
	isp_output->format->encoding = MMAL_ENCODING_I420;
	isp_output->buffer_num = 3;

	status = mmal_port_format_commit(isp_output);
	if (status != MMAL_SUCCESS)
	{
		print("ISP o/p commit failed\n");
		return -1;
	}
	print("format->video.size now %dx%d\n", isp_output->format->es->video.width, isp_output->format->es->video.height);

	isp_output->userdata = (struct MMAL_PORT_USERDATA_T *)dev;

	/* Set up all the sink components */
	for(i=0; i<MAX_COMPONENTS && dests[i].component_name; i++)
	{
		MMAL_COMPONENT_T *comp;
		MMAL_PORT_T *ip, *op = NULL;
		status = mmal_component_create(dests[i].component_name, &comp);
		if(status != MMAL_SUCCESS)
		{
			print("Failed to create %s", dests[i].component_name);
			return -1;
		}
		dev->components[i].comp = comp;
		ip = comp->input[0];

		status = mmal_format_full_copy(ip->format, isp_output->format);
		ip->buffer_num = 3;
		if (status == MMAL_SUCCESS)
			status = mmal_port_format_commit(ip);

		status += mmal_port_parameter_set_boolean(ip, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
		if (status != MMAL_SUCCESS)
			return -1;

		ip->userdata = (struct MMAL_PORT_USERDATA_T *)dev;

		//Set up the output of the sink component
		if (dests[i].output_encoding != MMAL_ENCODING_UNUSED && comp->output_num)
		{
			print("Setup output port\n");
			op = comp->output[0];
			op->format->encoding = dests[i].output_encoding;

			//This may not be relevant, but I'm not making it configurable
			op->format->bitrate = 10000000;
			op->buffer_size = 256<<10;

			if (op->buffer_size < op->buffer_size_min)
				op->buffer_size = op->buffer_size_min;

			op->buffer_num = 8;

			if (op->buffer_num < op->buffer_num_min)
				op->buffer_num = op->buffer_num_min;

			// We need to set the frame rate on output to 0, to ensure it gets
			// updated correctly from the input framerate when port connected
			op->format->es->video.frame_rate.num = 0;
			op->format->es->video.frame_rate.den = 1;

			// Commit the port changes to the output port
			status = mmal_port_format_commit(op);
			if (status != MMAL_SUCCESS)
			{
				print("Unable to set format on encoder output port\n");
			}


			status = mmal_port_parameter_set_boolean(op, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
			if (status != MMAL_SUCCESS)
			{
				print("Could not enable zero copy on %s output port\n", dests[i].component_name);
			}

			if (op->format->encoding == MMAL_ENCODING_H264)
			{
				MMAL_PARAMETER_VIDEO_PROFILE_T  param;
				param.hdr.id = MMAL_PARAMETER_PROFILE;
				param.hdr.size = sizeof(param);

				param.profile[0].profile = MMAL_VIDEO_PROFILE_H264_HIGH;//state->profile;
				param.profile[0].level = MMAL_VIDEO_LEVEL_H264_4;

				status = mmal_port_parameter_set(op, &param.hdr);
				if (status != MMAL_SUCCESS)
				{
					print("Unable to set H264 profile\n");
				}

				if (mmal_port_parameter_set_boolean(ip, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, 1) != MMAL_SUCCESS)
				{
					print("Unable to set immutable input flag\n");
					// Continue rather than abort..
				}

				//set INLINE HEADER flag to generate SPS and PPS for every IDR if requested
				if (mmal_port_parameter_set_boolean(op, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, 0) != MMAL_SUCCESS)
				{
					print("failed to set INLINE HEADER FLAG parameters\n");
					// Continue rather than abort..
				}
			}

			status = mmal_port_parameter_set_boolean(op, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
			if(status != MMAL_SUCCESS)
			{
				print("Failed to set zero copy\n");
				return -1;
			}
			op->userdata = (struct MMAL_PORT_USERDATA_T *)&dev->components[i];

			/* Setup the output files */
			if (filename[0] == '-' && filename[1] == '\0')
			{
				dev->components[i].stream_fd = stdout;
				debug = 0;
			}
			else
			{
				char tmp_filename[128];
				sprintf(tmp_filename, "%u_%s", i, filename);

				printf("Writing data to %s\n", tmp_filename);
				dev->components[i].stream_fd = fopen(tmp_filename, "wb");
			}

			{
				char tmp_filename[128];
				sprintf(tmp_filename, "%u_%s.pts", i, filename);

				dev->components[i].pts_fd = (void*)fopen(tmp_filename, "wb");
				if (dev->components[i].pts_fd) /* save header for mkvmerge */
					fprintf(dev->components[i].pts_fd, "# timecode format v2\n");
			}

			dev->components[i].save_queue = mmal_queue_create();
			if(!dev->components[i].save_queue)
			{
				print("Failed to create queue\n");
				return -1;
			}

			vcos_status = vcos_thread_create(&dev->components[i].save_thread, "save-thread",
						NULL, save_thread, &dev->components[i]);
			if(vcos_status != VCOS_SUCCESS)
			{
				print("Failed to create save thread\n");
				return -1;
			}

		}

		status = mmal_port_enable(ip, sink_input_callback);
		if (status != MMAL_SUCCESS)
			return -1;

		print("Create pool of %d buffers for %s\n",
						ip->buffer_num,
						dests[i].component_name);
		dev->components[i].ip_pool = mmal_port_pool_create(ip, ip->buffer_num, 0);
		if(!dev->components[i].ip_pool)
		{
			print("Failed to create %s ip pool\n", dests[i].component_name);
			return -1;
		}

		if (op)
		{
			print("Create pool of %d buffers for %s\n",
							op->buffer_num,
							dests[i].component_name);
			dev->components[i].op_pool = mmal_port_pool_create(op, op->buffer_num, op->buffer_size);
			if(!dev->components[i].op_pool)
			{
				print("Failed to create %s op pool\n", dests[i].component_name);
				return -1;
			}

			status = mmal_port_enable(op, dests[i].cb);
			if (status != MMAL_SUCCESS)
				return -1;

			unsigned int j;
			for(j=0; j<op->buffer_num; j++)
			{
				MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(dev->components[i].op_pool->queue);

				if (!buffer)
				{
					print("Where'd my buffer go?!\n");
					return -1;
				}
				status = mmal_port_send_buffer(op, buffer);
				if(status != MMAL_SUCCESS)
				{
					print("mmal_port_send_buffer failed on buffer %p, status %d\n", buffer, status);
					return -1;
				}
				print("Sent buffer %p", buffer);
			}
		}

		print("Enable %s....\n", dests[i].component_name);
		status = mmal_component_enable(comp);
		if(status != MMAL_SUCCESS)
		{
			print("Failed to enable\n");
			return -1;
		}
	}

	status = mmal_port_parameter_set_boolean(isp_output, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);

	/* All setup, so enable the ISP output and feed it the buffers */
	status = mmal_port_enable(isp_output, isp_output_callback);
	if (status != MMAL_SUCCESS)
		return -1;

	print("Create pool of %d buffers of size %d for encode/render\n", isp_output->buffer_num, isp_output->buffer_size);
	dev->isp_output_pool = mmal_port_pool_create(isp_output, isp_output->buffer_num, isp_output->buffer_size);
	if(!dev->isp_output_pool)
	{
		print("Failed to create pool\n");
		return -1;
	}

	buffers_to_isp(dev);

	return 0;
}

static int enable_isp_input(struct device *dev)
{
	MMAL_STATUS_T status;

	if (mmal_port_parameter_set_boolean(dev->isp->input[0], MMAL_PARAMETER_ZERO_COPY, dev->can_zero_copy) != MMAL_SUCCESS)
	{
		print("Failed to set zero copy\n");
		return -1;
	}
	status = mmal_port_enable(dev->isp->input[0], isp_ip_cb);
	if (status != MMAL_SUCCESS)
	{
		print("ISP input enable failed\n");
		return -1;
	}

	return 0;
}

static void destroy_mmal(struct device *dev)
{
	int i;
	//FIXME: Clean up everything properly
	for (i=0; i<MAX_COMPONENTS; i++)
	{
		dev->components[i].thread_quit = 1;
		vcos_thread_join(&dev->components[i].save_thread, NULL);

		if (dev->components[i].stream_fd)
			fclose(dev->components[i].stream_fd);
		if (dev->components[i].pts_fd)
			fclose(dev->components[i].pts_fd);
	}
}

static void video_save_image(struct device *dev, struct v4l2_buffer *buf,
			     const char *pattern, unsigned int sequence)
{
	unsigned int size;
	unsigned int i;
	char *filename;
	const char *p;
	bool append;
	int ret = 0;
	int fd;

	size = strlen(pattern);
	filename = malloc(size + 12);
	if (filename == NULL)
		return;

	p = strchr(pattern, '#');
	if (p != NULL) {
		sprintf(filename, "%.*s%06u%s", (int)(p - pattern), pattern,
			sequence, p + 1);
		append = false;
	} else {
		strcpy(filename, pattern);
		append = true;
	}

	fd = open(filename, O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC),
		  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	free(filename);
	if (fd == -1)
		return;

	for (i = 0; i < dev->num_planes; i++) {
		void *data = dev->buffers[buf->index].mem[i];
		unsigned int length;

		length = buf->bytesused;

		ret = write(fd, data, length);
		if (ret < 0) {
			print("write error: %s (%d)\n", strerror(errno), errno);
			break;
		} else if (ret != (int)length)
			print("write error: only %d bytes written instead of %u\n",
			       ret, length);
	}
	close(fd);
}

static int video_do_capture(struct device *dev, unsigned int nframes,
	unsigned int skip, const char *pattern,
	int do_requeue_last, int do_queue_late)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct v4l2_buffer buf;
	struct timespec start;
	struct timeval last;
	struct timespec ts;
	unsigned int size;
	unsigned int i = 0;
	double bps;
	double fps;
	int ret;
	int dropped_frames = 0;

	/* Start streaming. */
	ret = video_enable(dev, 1);
	if (ret < 0)
		goto done;

	if (do_queue_late)
		video_queue_all_buffers(dev);

	size = 0;
	clock_gettime(CLOCK_MONOTONIC, &start);
	last.tv_sec = start.tv_sec;
	last.tv_usec = start.tv_nsec / 1000;

        while (i < nframes) {
                fd_set fds[3];
                fd_set *rd_fds = &fds[0]; /* for capture */
                fd_set *ex_fds = &fds[1]; /* for capture */
                fd_set *wr_fds = &fds[2]; /* for output */
                struct timeval tv;
                int r;

                if (rd_fds) {
                    FD_ZERO(rd_fds);
                    FD_SET(dev->fd, rd_fds);
                }

                if (ex_fds) {
                    FD_ZERO(ex_fds);
                    FD_SET(dev->fd, ex_fds);
                }

                if (wr_fds) {
                    FD_ZERO(wr_fds);
                    FD_SET(dev->fd, wr_fds);
                }

                /* Timeout. */
                tv.tv_sec = 60;
                tv.tv_usec = 0;

                r = select(dev->fd + 1, rd_fds, wr_fds, ex_fds, &tv);

                if (-1 == r) {
                        if (EINTR == errno)
                                continue;
                        errno_exit("select");
                }

                if (0 == r) {
                        fprintf(stderr, "select timeout\n");
                        exit(EXIT_FAILURE);
                }

                if (rd_fds && FD_ISSET(dev->fd, rd_fds)) {
			const char *ts_type, *ts_source;
			int queue_buffer = 1;
			/* Dequeue a buffer. */
			memset(&buf, 0, sizeof buf);
			memset(planes, 0, sizeof planes);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.length = VIDEO_MAX_PLANES;
			buf.m.planes = planes;

			ret = ioctl(dev->fd, VIDIOC_DQBUF, &buf);
			if (ret < 0) {
				if (errno != EIO) {
					print("Unable to dequeue buffer: %s (%d).\n",
						strerror(errno), errno);
					goto done;
				}
				buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				buf.memory = V4L2_MEMORY_MMAP;
			}

			//print("bytesused in buffer is %d\n", buf.bytesused);
			size += buf.bytesused;

			fps = (buf.timestamp.tv_sec - last.tv_sec) * 1000000
			    + buf.timestamp.tv_usec - last.tv_usec;
			fps = fps ? 1000000.0 / fps : 0.0;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			get_ts_flags(buf.flags, &ts_type, &ts_source);
			print("%u (%u) [%c] %s %u %u B %ld.%06ld %ld.%06ld %.3f fps ts %s/%s\n", i, buf.index,
				(buf.flags & V4L2_BUF_FLAG_ERROR) ? 'E' : '-',
				v4l2_field_name(buf.field),
				buf.sequence, buf.bytesused,
				buf.timestamp.tv_sec, buf.timestamp.tv_usec,
				ts.tv_sec, ts.tv_nsec/1000, fps,
				ts_type, ts_source);

			last = buf.timestamp;

			/* Save the raw image. */
			if (pattern && !skip)
				video_save_image(dev, &buf, pattern, i);

			if (dev->mmal_pool) {
				MMAL_BUFFER_HEADER_T *mmal = mmal_queue_get(dev->mmal_pool->queue);
				MMAL_STATUS_T status;
				if (!mmal) {
					print("Failed to get MMAL buffer\n");
				} else {
					/* Need to wait for MMAL to be finished with the buffer before returning to V4L2 */
					queue_buffer = 0;
					if (((struct buffer*)mmal->user_data)->idx != buf.index) {
						print("Mismatch in expected buffers. V4L2 gave idx %d, MMAL expecting %d\n",
							buf.index, ((struct buffer*)mmal->user_data)->idx);
					}
					mmal->length = buf.length;	//Deliberately use length as MMAL wants the padding

					if (!dev->starttime.tv_sec)
						dev->starttime = buf.timestamp;

					struct timeval pts;
					timersub(&buf.timestamp, &dev->starttime, &pts);
					//MMAL PTS is in usecs, so convert from struct timeval
					mmal->pts = (pts.tv_sec * 1000000) + pts.tv_usec;
					if (mmal->pts > (dev->lastpts+dev->frame_time_usec+2500)) {
						print("DROPPED FRAME - %lld and %lld, delta %lld\n", dev->lastpts, mmal->pts, mmal->pts-dev->lastpts);
						dropped_frames++;
					}
					dev->lastpts = mmal->pts;

					mmal->flags = MMAL_BUFFER_HEADER_FLAG_FRAME_END;
					//mmal->pts = buf.timestamp;
					status = mmal_port_send_buffer(dev->isp->input[0], mmal);
					if (status != MMAL_SUCCESS)
						print("mmal_port_send_buffer failed %d\n", status);
				}
			}

			if (skip)
				--skip;

			fflush(stdout);

			i++;

			if (i >= nframes - dev->nbufs && !do_requeue_last)
				continue;
			if (!queue_buffer)
				continue;

			ret = video_queue_buffer(dev, buf.index);
			if (ret < 0) {
				print("Unable to requeue buffer: %s (%d).\n",
					strerror(errno), errno);
				goto done;
			}
                }
                if (wr_fds && FD_ISSET(dev->fd, wr_fds)) {
                    fprintf(stderr, "Writing?!?!?\n");
                }
                if (ex_fds && FD_ISSET(dev->fd, ex_fds)) {
                    fprintf(stderr, "Exception\n");
                    handle_event(dev);
                }
                /* EAGAIN - continue select loop. */
        }


	/* Stop streaming. */
	ret = video_enable(dev, 0);
	if (ret < 0)
		return ret;

	if (nframes == 0) {
		print("No frames captured.\n");
		goto done;
	}

	if (ts.tv_sec == start.tv_sec && ts.tv_nsec == start.tv_nsec) {
		print("Captured %u frames (%u bytes) 0 seconds\n", i, size);
		goto done;
	}

	ts.tv_sec -= start.tv_sec;
	ts.tv_nsec -= start.tv_nsec;
	if (ts.tv_nsec < 0) {
		ts.tv_sec--;
		ts.tv_nsec += 1000000000;
	}

	bps = size/(ts.tv_nsec/1000.0+1000000.0*ts.tv_sec)*1000000.0;
	fps = i/(ts.tv_nsec/1000.0+1000000.0*ts.tv_sec)*1000000.0;

	print("Captured %u frames in %lu.%06lu seconds (%f fps, %f B/s).\n",
		i, ts.tv_sec, ts.tv_nsec/1000, fps, bps);
	print("Total number of frames dropped %d\n", dropped_frames);
done:
	return video_free_buffers(dev);
}

int video_set_dv_timings(struct device *dev)
{
	struct v4l2_dv_timings timings;
	v4l2_std_id std;
	int ret;

	memset(&timings, 0, sizeof timings);
	ret = ioctl(dev->fd, VIDIOC_QUERY_DV_TIMINGS, &timings);
	if (ret >= 0) {
		print("QUERY_DV_TIMINGS returned %ux%u pixclk %llu\n", timings.bt.width, timings.bt.height, timings.bt.pixelclock);
		//Can read DV timings, so set them.
		ret = ioctl(dev->fd, VIDIOC_S_DV_TIMINGS, &timings);
		if (ret < 0) {
			print("Failed to set DV timings\n");
			return -1;
		} else {
			double tot_height, tot_width;
			const struct v4l2_bt_timings *bt = &timings.bt;
			
			tot_height = bt->height +
				bt->vfrontporch + bt->vsync + bt->vbackporch +
				bt->il_vfrontporch + bt->il_vsync + bt->il_vbackporch;
			tot_width = bt->width +
				bt->hfrontporch + bt->hsync + bt->hbackporch;
			dev->fps = (unsigned int)((double)bt->pixelclock /
				(tot_width * tot_height));
			print("Framerate is %u\n", dev->fps);
		}
	} else {
		memset(&std, 0, sizeof std);
		ret = ioctl(dev->fd, VIDIOC_QUERYSTD, &std);
		if (ret >= 0) {
			//Can read standard, so set it.
			ret = ioctl(dev->fd, VIDIOC_S_STD, &std);
			if (ret < 0) {
				print("Failed to set standard\n");
				return -1;
			} else {
				// SD video - assume 50Hz / 25fps
				dev->fps = 25;
			}
		}
	}
	return 0;
}

int video_get_fps(struct device *dev)
{
	struct v4l2_streamparm parm;
	int ret;

	memset(&parm, 0, sizeof parm);
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	ret = ioctl(dev->fd, VIDIOC_G_PARM, &parm);
	if (ret < 0) {
		print("Unable to get frame rate: %s (%d).\n",
			strerror(errno), errno);
		/* Make a wild guess at the frame rate */
		dev->fps = 15;
		return ret;
	}

	print("Current frame rate: %u/%u\n",
		parm.parm.capture.timeperframe.denominator,
		parm.parm.capture.timeperframe.numerator);

	dev->fps = parm.parm.capture.timeperframe.denominator/
			parm.parm.capture.timeperframe.numerator;

	return 0;
}

#define V4L_BUFFERS_DEFAULT	8
#define V4L_BUFFERS_MAX		32

static void usage(const char *argv0)
{
	print("Usage: %s [options] device\n", argv0);
	print("Supported options:\n");
	print("-c, --capture[=nframes]		Capture frames\n");
	print("-f, --format format		Set the video format\n");
	print("				use -f help to list the supported formats\n");
	print("-E, --encode-to [file]		Set filename to write to. Default of file.h264.\n");
	print("-F, --file[=name]		Read/write frames from/to disk\n");
	print("\tFor video capture devices, the first '#' character in the file name is\n");
	print("\texpanded to the frame sequence number. The default file name is\n");
	print("\t'frame-#.bin'.\n");
	print("-h, --help			Show this help screen\n");
	print("-I, --fill-frames		Fill frames with check pattern before queuing them\n");
	print("-n, --nbufs n			Set the number of video buffers\n");
	print("-p, --pause			Pause before starting the video stream\n");
	print("-s, --size WxH			Set the frame size\n");
	print("-t, --time-per-frame num/denom	Set the time per frame (eg. 1/25 = 25 fps)\n");
	print("-T, --dv-timings		Query and set the DV timings\n");
	print("    --buffer-prefix		Write portions of buffer before data_offset\n");
	print("    --buffer-size		Buffer size in bytes\n");
	print("    --fd                        Use a numeric file descriptor insted of a device\n");
	print("    --field			Interlaced format field order\n");
	print("    --log-status		Log device status\n");
	print("    --no-query			Don't query capabilities on open\n");
	print("    --offset			User pointer buffer offset from page start\n");
	print("    --premultiplied		Color components are premultiplied by alpha value\n");
	print("    --queue-late		Queue buffers after streamon, not before\n");
	print("    --requeue-last		Requeue the last buffers before streamoff\n");
	print("    --timestamp-source		Set timestamp source on output buffers [eof, soe]\n");
	print("    --skip n			Skip the first n frames\n");
	print("    --stride value		Line stride in bytes\n");
	print("-m  --mmal			Enable MMAL rendering of images\n");
}

#define OPT_ENUM_FORMATS	256
#define OPT_SKIP_FRAMES		258
#define OPT_NO_QUERY		259
#define OPT_REQUEUE_LAST	262
#define OPT_STRIDE		263
#define OPT_FD			264
#define OPT_TSTAMP_SRC		265
#define OPT_FIELD		266
#define OPT_LOG_STATUS		267
#define OPT_BUFFER_SIZE		268
#define OPT_PREMULTIPLIED	269
#define OPT_QUEUE_LATE		270
#define OPT_DATA_PREFIX		271

static struct option opts[] = {
	{"buffer-size", 1, 0, OPT_BUFFER_SIZE},
	{"capture", 2, 0, 'c'},
	{"data-prefix", 0, 0, OPT_DATA_PREFIX},
	{"encode-to", 1, 0, 'E'},
	{"fd", 1, 0, OPT_FD},
	{"field", 1, 0, OPT_FIELD},
	{"file", 2, 0, 'F'},
	{"fill-frames", 0, 0, 'I'},
	{"format", 1, 0, 'f'},
	{"help", 0, 0, 'h'},
	{"log-status", 0, 0, OPT_LOG_STATUS},
	{"mmal", 0, 0, 'm'},
	{"nbufs", 1, 0, 'n'},
	{"no-query", 0, 0, OPT_NO_QUERY},
	{"pause", 0, 0, 'p'},
	{"premultiplied", 0, 0, OPT_PREMULTIPLIED},
	{"queue-late", 0, 0, OPT_QUEUE_LATE},
	{"requeue-last", 0, 0, OPT_REQUEUE_LAST},
	{"size", 1, 0, 's'},
	{"skip", 1, 0, OPT_SKIP_FRAMES},
	{"stride", 1, 0, OPT_STRIDE},
	{"time-per-frame", 1, 0, 't'},
	{"timestamp-source", 1, 0, OPT_TSTAMP_SRC},
	{"dv-timings", 0, 0, 'T'},
	{0, 0, 0, 0}
};

int main(int argc, char *argv[])
{
	struct device dev;
	int ret;

	/* Options parsings */
	const struct v4l2_format_info *info;
	/* Use video capture by default if query isn't done. */
	unsigned int capabilities = V4L2_CAP_VIDEO_CAPTURE;
	int do_file = 0, do_capture = 0, do_pause = 0;
	int do_set_format = 0;
	int do_requeue_last = 0;
	int do_log_status = 0;
	int no_query = 0, do_queue_late = 0;
	int do_set_dv_timings = 0;
	char *endptr;
	int c;

	/* Video buffers */
	unsigned int pixelformat = V4L2_PIX_FMT_YUYV;
	unsigned int fmt_flags = 0;
	unsigned int width = 640;
	unsigned int height = 480;
	unsigned int stride = 0;
	unsigned int buffer_size = 0;
	unsigned int nbufs = V4L_BUFFERS_DEFAULT;
	unsigned int skip = 0;
	enum v4l2_field field = V4L2_FIELD_ANY;

	/* Capture loop */
	unsigned int nframes = (unsigned int)-1;
	const char *filename = "frame-#.bin";
	const char *encode_filename = "file.h264";

	video_init(&dev);
	bcm_host_init();

	opterr = 0;
	while ((c = getopt_long(argc, argv, "c::E:f:F::hn:pr:s:t:T", opts, NULL)) != -1) {

		switch (c) {
		case 'c':
			do_capture = 1;
			if (optarg)
				nframes = atoi(optarg);
			break;
		case 'E':
			print("We're encoding to %s\n", optarg);
			if (optarg)
				encode_filename = optarg;
			break;
		case 'f':
			if (!strcmp("help", optarg)) {
				list_formats();
				return 0;
			}
			do_set_format = 1;
			info = v4l2_format_by_name(optarg);
			if (info == NULL) {
				print("Unsupported video format '%s'\n", optarg);
				return 1;
			}
			pixelformat = info->fourcc;
			break;
		case 'F':
			do_file = 1;
			if (optarg)
				filename = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		case 'n':
			nbufs = atoi(optarg);
			if (nbufs > V4L_BUFFERS_MAX)
				nbufs = V4L_BUFFERS_MAX;
			break;
		case 'p':
			do_pause = 1;
			break;
		case 's':
			do_set_format = 1;
			width = strtol(optarg, &endptr, 10);
			if (*endptr != 'x' || endptr == optarg) {
				print("Invalid size '%s'\n", optarg);
				return 1;
			}
			height = strtol(endptr + 1, &endptr, 10);
			if (*endptr != 0) {
				print("Invalid size '%s'\n", optarg);
				return 1;
			}
			break;
		case 'T':
			do_set_dv_timings = 1;
			break;
		case OPT_BUFFER_SIZE:
			buffer_size = atoi(optarg);
			break;
		case OPT_FD:
			ret = atoi(optarg);
			if (ret < 0) {
				print("Bad file descriptor %d\n", ret);
				return 1;
			}
			print("Using file descriptor %d\n", ret);
			video_set_fd(&dev, ret);
			break;
		case OPT_FIELD:
			field = v4l2_field_from_string(optarg);
			if (field == (enum v4l2_field)-1) {
				print("Invalid field order '%s'\n", optarg);
				return 1;
			}
			break;
		case OPT_LOG_STATUS:
			do_log_status = 1;
			break;
		case OPT_NO_QUERY:
			no_query = 1;
			break;
		case OPT_PREMULTIPLIED:
			fmt_flags |= V4L2_PIX_FMT_FLAG_PREMUL_ALPHA;
			break;
		case OPT_QUEUE_LATE:
			do_queue_late = 1;
			break;
		case OPT_REQUEUE_LAST:
			do_requeue_last = 1;
			break;
		case OPT_SKIP_FRAMES:
			skip = atoi(optarg);
			break;
		case OPT_STRIDE:
			stride = atoi(optarg);
			break;
		case OPT_TSTAMP_SRC:
			if (!strcmp(optarg, "eof")) {
				dev.buffer_output_flags |= V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
			} else if (!strcmp(optarg, "soe")) {
				dev.buffer_output_flags |= V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
			} else {
				print("Invalid timestamp source %s\n", optarg);
				return 1;
			}
			break;
		case OPT_DATA_PREFIX:
			dev.write_data_prefix = true;
			break;
		default:
			print("Invalid option -%c\n", c);
			print("Run %s -h for help.\n", argv[0]);
			return 1;
		}
	}

	if (!do_file)
		filename = NULL;

	if (!video_has_fd(&dev)) {
		if (optind >= argc) {
			usage(argv[0]);
			return 1;
		}
		ret = video_open(&dev, argv[optind]);
		if (ret < 0)
			return 1;
	}

	if (!no_query) {
		ret = video_querycap(&dev, &capabilities);
		if (ret < 0)
			return 1;
	}

	if (do_log_status)
		video_log_status(&dev);

	/* Set the video format. */
	if (do_set_format) {
		if (video_set_format(&dev, width, height, pixelformat, stride,
				     buffer_size, field, fmt_flags) < 0) {
			video_close(&dev);
			return 1;
		}
	}

	if (do_set_dv_timings)
		video_set_dv_timings(&dev);

	if (!no_query || do_capture)
		video_get_format(&dev);

	{
		struct v4l2_event_subscription sub;

		memset(&sub, 0, sizeof(sub));

		sub.type = V4L2_EVENT_SOURCE_CHANGE;
		ioctl(dev.fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
	}

	if (!dev.fps)
		video_get_fps(&dev);

	setup_mmal(&dev, nbufs, encode_filename);

	if (!do_capture) {
		video_close(&dev);
		return 0;
	}

	if (video_prepare_capture(&dev, nbufs)) {
		video_close(&dev);
		return 1;
	}

	if (enable_isp_input(&dev)) {
		print("Failed to enable isp input\n");
		video_close(&dev);
		return 1;
	}

	if (!do_queue_late && video_queue_all_buffers(&dev)) {
		video_close(&dev);
		return 1;
	}

	if (do_pause) {
		print("Press enter to start capture\n");
		getchar();
	}

	if (video_do_capture(&dev, nframes, skip, filename,
			     do_requeue_last, do_queue_late) < 0) {
		video_close(&dev);
		return 1;
	}

	destroy_mmal(&dev);

	video_close(&dev);
	return 0;
}

