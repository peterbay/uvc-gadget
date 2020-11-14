/*
 * UVC gadget application
 * 
 * Modified by Petr Vavrin <pvavrin@gmail.com>
 *
 * Added video controls, code refactoring, enhanced logging, etc.
 *
 * Source tree:
 *   Primary source - wlhe - https://github.com/wlhe/uvc-gadget
 *   Forked source - climberhunt (Dave Hunt) - https://github.com/climberhunt/uvc-gadget
 *   Forked source - peterbay (Petr Vavrin) - https://github.com/peterbay/uvc-gadget
 * 
 * Fixes and inspiration
 *   delay when not streaming - https://github.com/kinweilee/v4l2-mmal-uvc/blob/master/v4l2-mmal-uvc.c
 * 
 * Original author:
 * Copyright (C) 2010 Ideas on board SPRL <laurent.pinchart@ideasonboard.com>
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

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <ftw.h>

#include <linux/usb/ch9.h>
#include <linux/usb/video.h>
#include <linux/videodev2.h>

#include "uvc-gadget.h"

volatile sig_atomic_t terminate = 0;

void term(int signum)
{
    (void)(signum); /* avoid warning: unused parameter 'signum' */
    terminate = 1;
}

static int sys_gpio_write(unsigned int type, char pin[], char value[])
{
    FILE * sys_file;
    char path[255];

    strcpy(path, "/sys/class/gpio/");

    switch(type) {
        case GPIO_EXPORT:
            strcat(path, "export");
            value = pin;
            break;

        case GPIO_DIRECTION:
            strcat(path, "gpio");
            strcat(path, pin);
            strcat(path, "/direction");
            break;

        case GPIO_VALUE:
            strcat(path, "gpio");
            strcat(path, pin);
            strcat(path, "/value");
            break;
    }

    printf("GPIO WRITE: Path: %s, Value: %s\n", path, value);

    sys_file = fopen(path, "w");
    if (!sys_file) {
        printf("GPIO ERROR: File write failed: %s (%d).\n", strerror(errno), errno);
        return -1;
    }

    fwrite(value, 1, strlen(value), sys_file);
    fclose(sys_file);
   
    return 0;
}

static int sys_led_write(unsigned int type, char value[])
{
    FILE * sys_file;
    char path[255];

    strcpy(path, "/sys/class/leds/led0/");

    switch(type) {
        case LED_TRIGGER:
            strcat(path, "trigger");
            break;

        case LED_BRIGHTNESS:
            strcat(path, "brightness");
            break;
    }

    printf("LED WRITE: Path: %s, Value: %s\n", path, value);

    sys_file = fopen(path, "w");
    if (!sys_file) {
        printf("LED ERROR: File write failed: %s (%d).\n", strerror(errno), errno);
        return -1;
    }

    fwrite(value, 1, strlen(value), sys_file);
    fclose(sys_file);
   
    return 0;
}

static void streaming_status_enable()
{
    int ret;
    if (!settings.streaming_status_enabled && settings.streaming_status_pin) {
        ret = sys_gpio_write(GPIO_EXPORT, settings.streaming_status_pin, NULL);
        if (ret < 0) {
            return;
        }

        ret = sys_gpio_write(GPIO_DIRECTION, settings.streaming_status_pin, GPIO_DIRECTION_OUT);
        if (ret < 0) {
            return;
        }

        ret = sys_gpio_write(GPIO_VALUE, settings.streaming_status_pin, GPIO_VALUE_OFF);
        if (ret < 0) {
            return;
        }

        settings.streaming_status_enabled = true;
    }

    if (settings.streaming_status_onboard) {
        ret = sys_led_write(LED_TRIGGER, LED_TRIGGER_NONE);
        if (ret < 0) {
            return;
        }

        ret = sys_led_write(LED_BRIGHTNESS, LED_BRIGHTNESS_LOW);
        if (ret < 0) {
            return;
        }
        settings.streaming_status_onboard_enabled = true;
    }
    return;
}

static void streaming_status_value(enum video_stream_action status)
{
    char * gpio_value = (status == STREAM_ON) ? GPIO_VALUE_ON : GPIO_VALUE_OFF;
    char * led_value = (status == STREAM_ON) ? LED_BRIGHTNESS_HIGH : LED_BRIGHTNESS_LOW;

    if (settings.streaming_status_enabled) {
        sys_gpio_write(GPIO_VALUE, settings.streaming_status_pin, gpio_value);
    }

    if (settings.streaming_status_onboard_enabled) {
        sys_led_write(LED_BRIGHTNESS, led_value);
    }
}

static char * uvc_request_code_name(unsigned int uvc_control)
{
    switch (uvc_control) {
    case UVC_RC_UNDEFINED:
        return "RC_UNDEFINED";

    case UVC_SET_CUR:
        return "SET_CUR";

    case UVC_GET_CUR:
        return "GET_CUR";

    case UVC_GET_MIN:
        return "GET_MIN";

    case UVC_GET_MAX:
        return "GET_MAX";

    case UVC_GET_RES:
        return "GET_RES";

    case UVC_GET_LEN:
        return "GET_LEN";

    case UVC_GET_INFO:
        return "GET_INFO";

    case UVC_GET_DEF:
        return "GET_DEF";

    default:
        return "UNKNOWN";

    }
}

static char * uvc_vs_interface_control_name(unsigned int interface)
{
    switch (interface) {
    case UVC_VS_CONTROL_UNDEFINED:
        return "CONTROL_UNDEFINED";

    case UVC_VS_PROBE_CONTROL:
        return "PROBE";

    case UVC_VS_COMMIT_CONTROL:
        return "COMMIT";

    case UVC_VS_STILL_PROBE_CONTROL:
        return "STILL_PROBE";

    case UVC_VS_STILL_COMMIT_CONTROL:
        return "STILL_COMMIT";

    case UVC_VS_STILL_IMAGE_TRIGGER_CONTROL:
        return "STILL_IMAGE_TRIGGER";

    case UVC_VS_STREAM_ERROR_CODE_CONTROL:
        return "STREAM_ERROR_CODE";

    case UVC_VS_GENERATE_KEY_FRAME_CONTROL:
        return "GENERATE_KEY_FRAME";

    case UVC_VS_UPDATE_FRAME_SEGMENT_CONTROL:
        return "UPDATE_FRAME_SEGMENT";

    case UVC_VS_SYNC_DELAY_CONTROL:
        return "SYNC_DELAY";

    default:
        return "UNKNOWN";
    }
}

static unsigned int get_frame_size(int pixelformat, int width, int height)
{
    switch (pixelformat) {
    case V4L2_PIX_FMT_YUYV:
        return width * height * 2;

    case V4L2_PIX_FMT_MJPEG:
        return width * height;
        break;
    }

    return width * height;
}
struct v4l2_device * v4l2_open(char *devname, enum device_type type)
{
    struct v4l2_device * dev;
    struct v4l2_capability cap;
    int fd;
    const char * type_name = (type == DEVICE_TYPE_UVC) ? "DEVICE_UVC" : "DEVICE_V4L2";

    printf("%s: Opening %s device\n", type_name, devname);

    fd = open(devname, O_RDWR | O_NONBLOCK, 0);
    if (fd == -1) {
        printf("%s: Device open failed: %s (%d).\n", type_name, strerror(errno), errno);
        return NULL;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        printf("%s: VIDIOC_QUERYCAP failed: %s (%d).\n", type_name, strerror(errno), errno);
        goto err;
    }

    switch (type) {
    case DEVICE_TYPE_V4L2:
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            printf("%s: %s is no video capture device\n", type_name, devname);
            goto err;
        }

        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
            printf("%s: %s does not support streaming i/o\n", type_name, devname);
            goto err;
        }
        break;

    case DEVICE_TYPE_UVC:
    default:
        if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
            printf("%s: %s is no video output device\n", type_name, devname);
            goto err;
        }
        break;
    }

    dev = calloc(1, sizeof *dev);
    if (dev == NULL) {
        goto err;
    }

    printf("%s: Device is %s on bus %s\n", type_name, cap.card, cap.bus_info);

    dev->fd = fd;
    dev->device_type = type;
    dev->device_type_name = type_name;
    dev->buffer_type = (type == DEVICE_TYPE_V4L2) ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
    return dev;

err:
    close(fd);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * V4L2 streaming related
 */

static void v4l2_uninit_device(struct v4l2_device *dev)
{
    unsigned int i;

    switch (dev->memory_type) {
    case V4L2_MEMORY_MMAP:
        if (!dev->mem) {
            return;
        }
        for (i = 0; i < dev->nbufs; ++i) {
            if (munmap(dev->mem[i].start, dev->mem[i].length) < 0) {
                printf("%s: munmap failed\n", dev->device_type_name);
                return;
            }
        }
        free(dev->mem);
        break;

    case V4L2_MEMORY_USERPTR:
    default:
        break;
    }
}

static int v4l2_video_stream(struct v4l2_device * dev, enum video_stream_action action)
{
    int type = dev->buffer_type;
    int ret;

    if (action == STREAM_ON) {
        ret = ioctl(dev->fd, VIDIOC_STREAMON, &type);
        if (ret < 0) {
            printf("%s: STREAM ON failed: %s (%d).\n", dev->device_type_name, strerror(errno), errno);
            return ret;
        }

        printf("%s: STREAM ON success\n", dev->device_type_name);
        dev->is_streaming = 1;
        uvc_shutdown_requested = false;

    } else if (dev->is_streaming) {
        ret = ioctl(dev->fd, VIDIOC_STREAMOFF, &type);
        if (ret < 0) {
            printf("%s: STREAM OFF failed: %s (%d).\n", dev->device_type_name, strerror(errno), errno);
            return ret;
        }

        printf("%s: STREAM OFF success\n", dev->device_type_name);
        dev->is_streaming = 0;
    }
    return 0;
}

static int v4l2_init_buffers(struct v4l2_device *dev, struct v4l2_requestbuffers * req,
    unsigned int count)
{
    int ret;
    req->count = count;
    req->type = dev->buffer_type;
    req->memory = dev->memory_type;

    ret = ioctl(dev->fd, VIDIOC_REQBUFS, req);
    if (ret < 0) {
        if (ret == -EINVAL) {
            printf("%s: Does not support %s\n", dev->device_type_name,
                (dev->memory_type == V4L2_MEMORY_USERPTR) ? "user pointer i/o" : "memory mapping");

        } else {
            printf("%s: VIDIOC_REQBUFS error: %s (%d).\n",
                dev->device_type_name, strerror(errno), errno);

        }
        return ret;
    }
    return count;
}

static int v4l2_reqbufs_mmap(struct v4l2_device *dev, struct v4l2_requestbuffers req)
{
    int ret;
    unsigned int i = 0;

    /* Map the buffers. */
    dev->mem = calloc(req.count, sizeof dev->mem[0]);
    if (!dev->mem) {
        printf("%s: Out of memory\n", dev->device_type_name);
        ret = -ENOMEM;
        goto err;
    }

    for (i = 0; i < req.count; ++i) {
        CLEAR(dev->mem[i].buf);

        dev->mem[i].buf.type = dev->buffer_type;
        dev->mem[i].buf.memory = V4L2_MEMORY_MMAP;
        dev->mem[i].buf.index = i;

        ret = ioctl(dev->fd, VIDIOC_QUERYBUF, &(dev->mem[i].buf));
        if (ret < 0) {
            printf("%s: VIDIOC_QUERYBUF failed for buf %d: %s (%d).\n",
                dev->device_type_name, i, strerror(errno), errno);

            ret = -EINVAL;
            goto err_free;
        }

        dev->mem[i].start =
            mmap(NULL /* start anywhere */,
                dev->mem[i].buf.length,
                PROT_READ | PROT_WRITE /* required */,
                MAP_SHARED /* recommended */,
                dev->fd, dev->mem[i].buf.m.offset
            );

        if (MAP_FAILED == dev->mem[i].start) {
            printf("%s: Unable to map buffer %u: %s (%d).\n",
                dev->device_type_name, i, strerror(errno), errno);

            dev->mem[i].length = 0;
            ret = -EINVAL;
            goto err_free;
        }

        dev->mem[i].length = dev->mem[i].buf.length;
        printf("%s: Buffer %u mapped at address %p, length %d.\n",
            dev->device_type_name, i, dev->mem[i].start, dev->mem[i].length);
    }

    return 0;

err_free:
    free(dev->mem);
err:
    return ret;
}

static int v4l2_reqbufs(struct v4l2_device *dev, int nbufs)
{
    int ret = 0;
    struct v4l2_requestbuffers req;
    CLEAR(req);

    dev->dqbuf_count = 0;
    dev->qbuf_count = 0;

    ret = v4l2_init_buffers(dev, &req, nbufs);
    if (ret < 1) {
        return ret;
    }

    if (!req.count) {
        return 0;
    }

    if (dev->memory_type == V4L2_MEMORY_MMAP) {
        if (req.count < 2) {
            printf("%s: Insufficient buffer memory.\n", dev->device_type_name);
            return -EINVAL;
        }

        ret = v4l2_reqbufs_mmap(dev, req);
        if (ret < 0) {
            return -EINVAL;
        }
    }

    dev->nbufs = req.count;
    printf("%s: %u buffers allocated.\n", dev->device_type_name, req.count);

    return ret;
}

static int v4l2_qbuf_mmap(struct v4l2_device * dev)
{
    unsigned int i;
    int ret;

    for (i = 0; i < dev->nbufs; ++i) {
        CLEAR(dev->mem[i].buf);

        dev->mem[i].buf.type = dev->buffer_type;
        dev->mem[i].buf.memory = V4L2_MEMORY_MMAP;
        dev->mem[i].buf.index = i;

        ret = ioctl(dev->fd, VIDIOC_QBUF, &(dev->mem[i].buf));
        if (ret < 0) {
            printf("%s: VIDIOC_QBUF failed : %s (%d).\n",
                dev->device_type_name, strerror(errno), errno);

            return ret;
        }
        dev->qbuf_count++;
    }
    return 0;
}

static void v4l2_process_data(struct v4l2_device *dev)
{
    struct v4l2_buffer vbuf;
    struct v4l2_buffer ubuf;

    if (dev->udev->is_streaming && dev->dqbuf_count >= dev->qbuf_count) {
        return;
    }

    /* Dequeue spent buffer from V4L2 domain. */
    CLEAR(vbuf);
    vbuf.type = dev->buffer_type;
    vbuf.memory = dev->memory_type;

    if (ioctl(dev->fd, VIDIOC_DQBUF, &vbuf) < 0) {
        return;
    }

    dev->dqbuf_count++;

    /* Queue video buffer to UVC domain. */
    CLEAR(ubuf);
    ubuf.type = dev->udev->buffer_type;
    ubuf.memory = dev->udev->memory_type;
    ubuf.m.userptr = (unsigned long)dev->mem[vbuf.index].start;
    ubuf.length = dev->mem[vbuf.index].length;
    ubuf.index = vbuf.index;
    ubuf.bytesused = vbuf.bytesused;

    if (ioctl(dev->udev->fd, VIDIOC_QBUF, &ubuf) < 0) {
        /* Check for a USB disconnect/shutdown event. */
        if (errno == ENODEV) {
            uvc_shutdown_requested = true;
            printf("UVC: Possible USB shutdown requested from Host, seen during VIDIOC_QBUF\n");
        }
        return;
    }

    dev->udev->qbuf_count++;

    if (!dev->udev->is_streaming) {
        v4l2_video_stream(dev->udev, STREAM_ON);
        streaming_status_value((dev->udev->is_streaming) ? STREAM_ON : STREAM_OFF);
    }
}

/* ---------------------------------------------------------------------------
 * V4L2 generic stuff
 */

static int v4l2_get_format(struct v4l2_device *dev)
{
    struct v4l2_format fmt;
    int ret;

    CLEAR(fmt);
    fmt.type = dev->buffer_type;

    ret = ioctl(dev->fd, VIDIOC_G_FMT, &fmt);
    if (ret < 0) {
        return ret;
    }

    printf("%s: Getting current format: %c%c%c%c %ux%u\n",
        dev->device_type_name, pixfmtstr(fmt.fmt.pix.pixelformat),
        fmt.fmt.pix.width, fmt.fmt.pix.height);

    return 0;
}

static int v4l2_set_format(struct v4l2_device *dev, struct v4l2_format *fmt)
{
    int ret;

    ret = ioctl(dev->fd, VIDIOC_S_FMT, fmt);
    if (ret < 0) {
        printf("%s: Unable to set format %s (%d).\n",
            dev->device_type_name, strerror(errno), errno);
        return ret;
    }

    printf("%s: Setting format to: %c%c%c%c %ux%u\n",
        dev->device_type_name, pixfmtstr(fmt->fmt.pix.pixelformat),
        fmt->fmt.pix.width, fmt->fmt.pix.height);

    return 0;
}

static int v4l2_apply_format(struct v4l2_device *dev, unsigned int pixelformat,
    unsigned int width, unsigned int height)
{
    struct v4l2_format fmt;
    int ret = -EINVAL;

    if (dev->is_streaming) {
        return ret;
    }

    CLEAR(fmt);
    fmt.type = dev->buffer_type;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.sizeimage = get_frame_size(pixelformat, width, height);
    fmt.fmt.pix.pixelformat = pixelformat;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    ret = v4l2_set_format(dev, &fmt);
    if (ret < 0) {
        return ret;
    }

    return v4l2_get_format(dev);
}

static void v4l2_set_ctrl_value(struct v4l2_device *dev,
    struct control_mapping_pair ctrl, unsigned int ctrl_v4l2, int v4l2_ctrl_value)
{
    struct v4l2_queryctrl queryctrl;
    struct v4l2_control control;
    CLEAR(queryctrl);

    queryctrl.id = ctrl_v4l2;
    if (ioctl(dev->fd, VIDIOC_QUERYCTRL, &queryctrl) == -1) {
        if (errno != EINVAL) {
            printf("%s: %s VIDIOC_QUERYCTRL failed: %s (%d).\n",
                dev->device_type_name, ctrl.v4l2_name, strerror(errno), errno);

        } else {
            printf("%s: %s is not supported: %s (%d).\n",
                dev->device_type_name, ctrl.v4l2_name, strerror(errno), errno);

        }

    } else if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
        printf("%s: %s is disabled.\n", dev->device_type_name, ctrl.v4l2_name);

    } else {
        CLEAR(control);
        control.id = ctrl.v4l2;
        control.value = v4l2_ctrl_value;

        if (ioctl(dev->fd, VIDIOC_S_CTRL, &control) == -1) {
            printf("%s: %s VIDIOC_S_CTRL failed: %s (%d).\n",
                dev->device_type_name, ctrl.v4l2_name, strerror(errno), errno);
            return;
        }
        printf("%s: %s changed value (V4L2: %d, UVC: %d)\n",
            dev->device_type_name, ctrl.v4l2_name, v4l2_ctrl_value, ctrl.value);
    }
}

static void v4l2_set_ctrl(struct v4l2_device *dev, struct control_mapping_pair ctrl)
{
    int v4l2_ctrl_value = 0;
    int v4l2_diff = ctrl.v4l2_maximum - ctrl.v4l2_minimum;
    int ctrl_diff = ctrl.maximum - ctrl.minimum;

    if (ctrl.value < ctrl.minimum) {
        ctrl.value = ctrl.minimum;
    }

    if (ctrl.value > ctrl.maximum) {
        ctrl.value = ctrl.maximum;
    }

    v4l2_ctrl_value = (ctrl.value - ctrl.minimum) * v4l2_diff / ctrl_diff + ctrl.v4l2_minimum;

    v4l2_set_ctrl_value(dev, ctrl, ctrl.v4l2, v4l2_ctrl_value);

    if (ctrl.v4l2 == V4L2_CID_RED_BALANCE) {
        v4l2_set_ctrl_value(dev, ctrl, V4L2_CID_BLUE_BALANCE, v4l2_ctrl_value);
    }
}

static void v4l2_apply_camera_control(struct control_mapping_pair * mapping,
    struct v4l2_queryctrl queryctrl, struct v4l2_control control)
{
    mapping->enabled = true;
    mapping->control_type = queryctrl.type;
    mapping->v4l2_minimum = queryctrl.minimum;
    mapping->v4l2_maximum = queryctrl.maximum;
    mapping->minimum = 0;
    mapping->maximum = (0 - queryctrl.minimum) + queryctrl.maximum;
    mapping->step = queryctrl.step;
    mapping->default_value = (0 - queryctrl.minimum) + queryctrl.default_value;
    mapping->value = (0 - queryctrl.minimum) + control.value;
    
    printf("V4L2: Supported control %s (%s = %s)\n", queryctrl.name,
        mapping->v4l2_name, mapping->uvc_name);

    printf("V4L2:   V4L2: min: %d, max: %d, step: %d, default: %d, value: %d\n",
        queryctrl.minimum,
        queryctrl.maximum,
        queryctrl.step,
        queryctrl.default_value,
        control.value
    );

    printf("V4L2:   UVC: min: %d, max: %d, step: %d, default: %d, value: %d\n",
        mapping->minimum,
        mapping->maximum,
        queryctrl.step,
        mapping->default_value,
        mapping->value
    );
}

static void v4l2_get_controls(struct v4l2_device *dev)
{
    int i;
    struct v4l2_queryctrl queryctrl;
    struct v4l2_control control;
    unsigned int id;
    const unsigned next_fl = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    CLEAR(queryctrl);

    queryctrl.id = next_fl;
    while (0 == ioctl (dev->fd, VIDIOC_QUERYCTRL, &queryctrl)) {
        
        id = queryctrl.id;
        queryctrl.id |= next_fl;

        if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
            continue;
        }

        if (id && V4L2_CTRL_CLASS_USER) {
            for (i = 0; i < control_mapping_size; i++) {
                if (control_mapping[i].v4l2 == id) {
                    control.id = queryctrl.id;
                    if (0 == ioctl (dev->fd, VIDIOC_G_CTRL, &control)) {
                        v4l2_apply_camera_control(&control_mapping[i], queryctrl, control);
                    }
                }
            }
        }
    }
}

static void v4l2_close(struct v4l2_device *dev)
{
    close(dev->fd);
    free(dev);
}

// https://stackoverflow.com/a/15683117
static void v4l2_get_available_formats(struct v4l2_device *dev)
{
    struct v4l2_fmtdesc fmtdesc;
    struct v4l2_frmsizeenum frmsize;
    unsigned int width;
    unsigned int height;
    CLEAR(fmtdesc);
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    while (ioctl(dev->fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_MJPEG || fmtdesc.pixelformat == V4L2_PIX_FMT_YUYV) {
            frmsize.pixel_format = fmtdesc.pixelformat;
            frmsize.index = 0;
            while (ioctl(dev->fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) >= 0) {
                width = 0;
                height = 0;
                if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                    width = frmsize.discrete.width;
                    height = frmsize.discrete.height;

                } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                    width = frmsize.stepwise.max_width;
                    height = frmsize.stepwise.max_height;

                }

                if (width && height) {
                    printf("%s: Getting highest frame size: %c%c%c%c %ux%u\n",
                        dev->device_type_name, pixfmtstr(fmtdesc.pixelformat), width, height);
                }
                frmsize.index++;
            }
        }
        fmtdesc.index++;
    }
}

/* ---------------------------------------------------------------------------
 * UVC streaming related
 */

static void uvc_video_process(struct v4l2_device *dev)
{
    struct v4l2_buffer ubuf;
    struct v4l2_buffer vbuf;
    /*
     * Do not dequeue buffers from UVC side until there are atleast
     * 2 buffers available at UVC domain.
     */
    if (!uvc_shutdown_requested && ((dev->dqbuf_count + 1) >= dev->qbuf_count)) {
        return;
    }

    /* Prepare a v4l2 buffer to be dequeued from UVC domain. */
    CLEAR(ubuf);
    ubuf.type = dev->buffer_type;
    ubuf.memory = dev->memory_type;

    /* Dequeue the spent buffer from UVC domain */
    if (ioctl(dev->fd, VIDIOC_DQBUF, &ubuf) < 0) {
        printf("UVC: Unable to dequeue buffer: %s (%d).\n", strerror(errno), errno);
        return;
    }

    dev->dqbuf_count++;

    /*
        * If the dequeued buffer was marked with state ERROR by the
        * underlying UVC driver gadget, do not queue the same to V4l2
        * and wait for a STREAMOFF event on UVC side corresponding to
        * set_alt(0). So, now all buffers pending at UVC end will be
        * dequeued one-by-one and we will enter a state where we once
        * again wait for a set_alt(1) command from the USB host side.
        */
    if (ubuf.flags & V4L2_BUF_FLAG_ERROR) {
        uvc_shutdown_requested = true;
        printf("UVC: Possible USB shutdown requested from Host, seen during VIDIOC_DQBUF\n");
        return;
    }

    /* Queue the buffer to V4L2 domain */
    CLEAR(vbuf);
    vbuf.type = dev->vdev->buffer_type;
    vbuf.memory = dev->vdev->memory_type;
    vbuf.index = ubuf.index;

    if (ioctl(dev->vdev->fd, VIDIOC_QBUF, &vbuf) < 0) {
        return;
    }

    dev->vdev->qbuf_count++;

    if (settings.show_fps) {
        dev->buffers_processed++;
    }
}

static void uvc_handle_streamon_event(struct v4l2_device *dev)
{
    if (v4l2_reqbufs(dev->vdev, dev->vdev->nbufs) < 0) {
        return;
    }

    if (v4l2_reqbufs(dev, dev->nbufs) < 0) {
        return;
    }

    if (v4l2_qbuf_mmap(dev->vdev) < 0) {
        return;
    }

    /* Start V4L2 capturing now. */
    if (v4l2_video_stream(dev->vdev, STREAM_ON) < 0) {
        return;
    }
}

static void v4l2_device_stream_off(struct v4l2_device *dev)
{
    if (dev->is_streaming) {
        v4l2_video_stream(dev, STREAM_OFF);
    }
    printf("%s: Uninit device\n", dev->device_type_name);
    v4l2_uninit_device(dev);
    printf("%s: Request 0 buffers\n", dev->device_type_name);
    v4l2_reqbufs(dev, 0);
}

static void uvc_handle_streamoff_event(struct v4l2_device *dev)
{
    v4l2_device_stream_off(dev->vdev);
    v4l2_device_stream_off(dev);
    streaming_status_value((dev->is_streaming) ? STREAM_ON : STREAM_OFF);
}

/* ---------------------------------------------------------------------------
 * UVC Request processing
 */
static void dump_uvc_streaming_control(struct uvc_streaming_control *ctrl)
{
    printf("DUMP: uvc_streaming_control: format: %d, frame: %d, frame interval: %d\n",
        ctrl->bFormatIndex,
        ctrl->bFrameIndex,
        ctrl->dwFrameInterval
    );
}

static int uvc_get_frame_format_index(int format_index, enum uvc_frame_format_getter getter)
{
    int index = -1;
    int value;
    int i;

    for (i = 0; i <= last_format_index; i++) {
        if (format_index == -1 || format_index == (int) uvc_frame_format[i].bFormatIndex) {

            switch (getter) {
                case FORMAT_INDEX_MIN:
                case FORMAT_INDEX_MAX:
                    value = uvc_frame_format[i].bFormatIndex;
                    break;

                case FRAME_INDEX_MIN:
                case FRAME_INDEX_MAX:
                    value = uvc_frame_format[i].bFrameIndex;
                    break;
            }
            if (index == -1) {
                index = value;

            } else {
                switch (getter) {
                    case FORMAT_INDEX_MIN:
                    case FRAME_INDEX_MIN:
                        if (value < index) {
                            index = value;
                        }
                        break;

                    case FORMAT_INDEX_MAX:
                    case FRAME_INDEX_MAX:
                        if (value > index) {
                            index = value;
                        }
                        break;
                }
            }
        }
    }
    return index;
}

static int uvc_get_frame_format(struct uvc_frame_format ** frame_format,
    unsigned int iFormat, unsigned int iFrame)
{
    int i;
    for (i = 0; i <= last_format_index; i++) {
        if (uvc_frame_format[i].bFormatIndex == iFormat &&
            uvc_frame_format[i].bFrameIndex == iFrame
        ) {
            *frame_format = &uvc_frame_format[i];
            return 0;
        }
    }
    return -1;
}

static void uvc_dump_frame_format(struct uvc_frame_format * frame_format, const char * title)
{
    printf("%s: format: %d, frame: %d, resolution: %dx%d, frame_interval: %d,  bitrate: [%d, %d]\n",
        title,
        frame_format->bFormatIndex,
        frame_format->bFrameIndex,
        frame_format->wWidth,
        frame_format->wHeight,
        frame_format->dwDefaultFrameInterval,
        frame_format->dwMinBitRate,
        frame_format->dwMaxBitRate
    );
}

static void uvc_fill_streaming_control(struct v4l2_device *dev, struct uvc_streaming_control *ctrl,
    enum stream_control_action action, int iformat, int iframe)
{
    int format_first;
    int format_last;
    int frame_first;
    int frame_last;
    int format_frame_first;
    int format_frame_last;
    unsigned int frame_interval;

    switch (action) {
    case STREAM_CONTROL_INIT:
        printf("UVC: Streaming control: action: INIT\n");
        break;

    case STREAM_CONTROL_MIN:
        printf("UVC: Streaming control: action: GET MIN\n");
        break;

    case STREAM_CONTROL_MAX:
        printf("UVC: Streaming control: action: GET MAX\n");
        break;

    case STREAM_CONTROL_SET:
        printf("UVC: Streaming control: action: SET, format: %d, frame: %d\n", iformat, iframe);
        break;

    }

    format_first = uvc_get_frame_format_index(-1, FORMAT_INDEX_MIN);
    format_last = uvc_get_frame_format_index(-1, FORMAT_INDEX_MAX);

    frame_first = uvc_get_frame_format_index(-1, FRAME_INDEX_MIN);
    frame_last = uvc_get_frame_format_index(-1, FRAME_INDEX_MAX);

    if (action == STREAM_CONTROL_MIN) {
        iformat = format_first;
        iframe = frame_first;

    } else if (action == STREAM_CONTROL_MAX) {
        iformat = format_last;
        iframe = frame_last;

    } else {
        iformat = clamp(iformat, format_first, format_last);

        format_frame_first = uvc_get_frame_format_index(iformat, FRAME_INDEX_MIN);
        format_frame_last = uvc_get_frame_format_index(iformat, FRAME_INDEX_MAX);

        iframe = clamp(iframe, format_frame_first, format_frame_last);
    }

    struct uvc_frame_format * frame_format;
    uvc_get_frame_format(&frame_format, iformat, iframe);

    uvc_dump_frame_format(frame_format, "FRAME");

    if (frame_format->dwDefaultFrameInterval >= 100000) {
        frame_interval = frame_format->dwDefaultFrameInterval;
    } else {
        frame_interval = 400000;
    }

    memset(ctrl, 0, sizeof *ctrl);
    ctrl->bmHint = 1; 
    ctrl->bFormatIndex = iformat;
    ctrl->bFrameIndex = iframe;
    ctrl->dwMaxVideoFrameSize = get_frame_size(frame_format->video_format, frame_format->wWidth, frame_format->wHeight);
    ctrl->dwMaxPayloadTransferSize = streaming_maxpacket;
    ctrl->dwFrameInterval = frame_interval;
    ctrl->bmFramingInfo = 3;
    ctrl->bMinVersion = format_first;
    ctrl->bMaxVersion = format_last;
    ctrl->bPreferedVersion = format_last;

    dump_uvc_streaming_control(ctrl);

    if (dev->control == UVC_VS_COMMIT_CONTROL && action == STREAM_CONTROL_SET) {
        v4l2_apply_format(dev->vdev, frame_format->video_format, frame_format->wWidth, frame_format->wHeight);
        v4l2_apply_format(dev, frame_format->video_format, frame_format->wWidth, frame_format->wHeight);
    }
}

static void uvc_interface_control(unsigned int interface, struct v4l2_device *dev,
    uint8_t req, uint8_t cs, uint8_t len, struct uvc_request_data *resp)
{
    int i;
    bool found = false;
    const char * request_code_name = uvc_request_code_name(req);
    const char * interface_name = (interface == UVC_VC_INPUT_TERMINAL) ? "INPUT_TERMINAL" : "PROCESSING_UNIT";

    for (i = 0; i < control_mapping_size; i++) {
        if (control_mapping[i].type == interface && control_mapping[i].uvc == cs) {
            found = true;
            break;
        }
    }
 
    if (!found) {
        printf("UVC: %s - %s - %02x - UNSUPPORTED\n", interface_name, request_code_name, cs);
        resp->length = -EL2HLT;
        dev->request_error_code = REQEC_INVALID_CONTROL;
        return;
    } 

    if (!control_mapping[i].enabled) {
        printf("UVC: %s - %s - %s - DISABLED\n", interface_name, request_code_name,
            control_mapping[i].uvc_name);
        resp->length = -EL2HLT;
        dev->request_error_code = REQEC_INVALID_CONTROL;
        return;
    }

    printf("UVC: %s - %s - %s\n", interface_name, request_code_name, control_mapping[i].uvc_name);

    switch (req) {
    case UVC_SET_CUR:
        resp->data[0] = 0x0;
        resp->length = len;
        dev->control_interface = interface;
        dev->control_type = cs;
        dev->request_error_code = REQEC_NO_ERROR;
        break;

    case UVC_GET_MIN:
        resp->length = 4;
        memcpy(&resp->data[0], &control_mapping[i].minimum, resp->length);
        dev->request_error_code = REQEC_NO_ERROR;
        break;

    case UVC_GET_MAX:
        resp->length = 4;
        memcpy(&resp->data[0], &control_mapping[i].maximum, resp->length);
        dev->request_error_code = REQEC_NO_ERROR;
        break;

    case UVC_GET_CUR:
        resp->length = 4;
        memcpy(&resp->data[0], &control_mapping[i].value, resp->length);
        dev->request_error_code = REQEC_NO_ERROR;
        break;

    case UVC_GET_INFO:
        resp->data[0] = (uint8_t)(UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET);
        resp->length = 1;
        dev->request_error_code = REQEC_NO_ERROR;
        break;

    case UVC_GET_DEF:
        resp->length = 4;
        memcpy(&resp->data[0], &control_mapping[i].default_value, resp->length);
        dev->request_error_code = REQEC_NO_ERROR;
        break;

    case UVC_GET_RES:
        resp->length = 4;
        memcpy(&resp->data[0], &control_mapping[i].step, resp->length);
        dev->request_error_code = REQEC_NO_ERROR;
        break;

    default:
        resp->length = -EL2HLT;
        dev->request_error_code = REQEC_INVALID_REQUEST;
        break;

    }
    return;
}

static void uvc_events_process_streaming(struct v4l2_device *dev, uint8_t req, uint8_t cs,
    struct uvc_request_data *resp)
{
    printf("UVC: Streaming request CS: %s, REQ: %s\n", uvc_vs_interface_control_name(cs),
        uvc_request_code_name(req));

    if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL) {
        return;
    }

    struct uvc_streaming_control * ctrl = (struct uvc_streaming_control *) &resp->data;
    struct uvc_streaming_control * target = (cs == UVC_VS_PROBE_CONTROL) ? &dev->probe : &dev->commit;

    int ctrl_length = sizeof * ctrl;
    resp->length = ctrl_length;

    switch (req) {
    case UVC_SET_CUR:
        dev->control = cs;
        resp->length = ctrl_length;
        break;

    case UVC_GET_MAX:
        uvc_fill_streaming_control(dev, ctrl, STREAM_CONTROL_MAX, 0, 0);
        break;

    case UVC_GET_CUR:
        memcpy(ctrl, target, ctrl_length);
        break;

    case UVC_GET_MIN:
    case UVC_GET_DEF:
        uvc_fill_streaming_control(dev, ctrl, STREAM_CONTROL_MIN, 0, 0);
        break;

    case UVC_GET_RES:
        CLEAR(ctrl);
        break;

    case UVC_GET_LEN:
        resp->data[0] = 0x00;
        resp->data[1] = ctrl_length;
        resp->length = 2;
        break;

    case UVC_GET_INFO:
        resp->data[0] = (uint8_t)(UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET);
        resp->length = 1;
        break;
    }
}

static void uvc_events_process_class(struct v4l2_device *dev, struct usb_ctrlrequest *ctrl,
    struct uvc_request_data *resp)
{
    uint8_t type = ctrl->wIndex & 0xff;
    uint8_t interface = ctrl->wIndex >> 8;
    uint8_t control = ctrl->wValue >> 8;
    uint8_t length = ctrl->wLength;

    if ((ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_INTERFACE) {
        return;
    }

    switch (type) {
    case UVC_INTF_CONTROL:
        switch (interface) {
            case 0:
                if (control == UVC_VC_REQUEST_ERROR_CODE_CONTROL) {
                    resp->data[0] = dev->request_error_code;
                    resp->length = 1;
                }
                break;

            case 1:
                uvc_interface_control(UVC_VC_INPUT_TERMINAL, dev, ctrl->bRequest, control, length, resp);
                break;

            case 2:
                uvc_interface_control(UVC_VC_PROCESSING_UNIT, dev, ctrl->bRequest, control, length, resp);
                break;

            default:
                break;
        }
        break;

    case UVC_INTF_STREAMING:
        uvc_events_process_streaming(dev, ctrl->bRequest, control, resp);
        break;

    default:
        break;
    }
}

static void uvc_events_process_setup(struct v4l2_device *dev, struct usb_ctrlrequest *ctrl,
    struct uvc_request_data *resp)
{
    dev->control = 0;
    if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS) {
        uvc_events_process_class(dev, ctrl, resp);
    }
}

static void uvc_events_process_data_control(struct v4l2_device *dev, struct uvc_request_data *data,
    struct uvc_streaming_control *target)
{
    struct uvc_streaming_control * ctrl = (struct uvc_streaming_control *) &data->data;
    unsigned int iformat = (unsigned int) ctrl->bFormatIndex;
    unsigned int iframe = (unsigned int) ctrl->bFrameIndex;

    uvc_fill_streaming_control(dev, target, STREAM_CONTROL_SET, iformat, iframe);
}

static void uvc_events_process_data(struct v4l2_device *dev, struct uvc_request_data *data)
{
    int i;
    printf("UVC: Control %s, length: %d\n", uvc_vs_interface_control_name(dev->control), data->length);

    switch (dev->control) {
    case UVC_VS_PROBE_CONTROL:
        uvc_events_process_data_control(dev, data, &dev->probe);
        break;

    case UVC_VS_COMMIT_CONTROL:
        uvc_events_process_data_control(dev, data, &dev->commit);
        break;

    case UVC_VS_CONTROL_UNDEFINED:
        if (data->length > 0 && data->length <= 4) {
            for (i = 0; i < control_mapping_size; i++) {
                if (control_mapping[i].type == dev->control_interface &&
                    control_mapping[i].uvc == dev->control_type &&
                    control_mapping[i].enabled
                ) {
                    control_mapping[i].value = 0x00000000;
                    control_mapping[i].length = data->length;
                    memcpy(&control_mapping[i].value, data->data, data->length);
                    v4l2_set_ctrl(dev->vdev, control_mapping[i]);
                }
            }
        }
        break;

    default:
        printf("UVC: Setting unknown control, length = %d\n", data->length);
        break;
    }
}

static void uvc_events_process(struct v4l2_device *dev)
{
    struct v4l2_event v4l2_event;
    struct uvc_event *uvc_event = (void *)&v4l2_event.u.data;
    struct uvc_request_data resp;

    if (ioctl(dev->fd, VIDIOC_DQEVENT, &v4l2_event) < 0) {
        printf("VIDIOC_DQEVENT failed: %s (%d)\n", strerror(errno), errno);
        return;
    }

    CLEAR(resp);
    resp.length = -EL2HLT;

    switch (v4l2_event.type) {
    case UVC_EVENT_CONNECT:
        break;

    case UVC_EVENT_DISCONNECT:
        uvc_shutdown_requested = true;
        printf("UVC: Possible USB shutdown requested from Host, seen via UVC_EVENT_DISCONNECT\n");
        break;

    case UVC_EVENT_SETUP:
        uvc_events_process_setup(dev, &uvc_event->req, &resp);

        if (ioctl(dev->fd, UVCIOC_SEND_RESPONSE, &resp) < 0) {
           printf("UVCIOC_SEND_RESPONSE failed: %s (%d)\n", strerror(errno), errno);
        }
        break;

    case UVC_EVENT_DATA:
        uvc_events_process_data(dev, &uvc_event->data);
        break;

    case UVC_EVENT_STREAMON:
        uvc_handle_streamon_event(dev);
        break;

    case UVC_EVENT_STREAMOFF:
        uvc_handle_streamoff_event(dev);
        break;
    
    default:
        break;
    }
}

static void uvc_events_init(struct v4l2_device *dev)
{
    struct v4l2_event_subscription sub;

    uvc_fill_streaming_control(dev, &dev->probe, STREAM_CONTROL_INIT, 0, 0);
    uvc_fill_streaming_control(dev, &dev->commit, STREAM_CONTROL_INIT, 0, 0);

    CLEAR(sub);
    sub.type = UVC_EVENT_SETUP;
    ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_DATA;
    ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_STREAMON;
    ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_STREAMOFF;
    ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
}

/* ---------------------------------------------------------------------------
 * main
 */

static void processing_loop_video(struct v4l2_device * udev, struct v4l2_device * vdev)
{
    struct timeval tv;
    struct timeval video_tv;
    int activity;
    fd_set fdsv, fdsu;
    int nfds;

    while (!terminate) {
        FD_ZERO(&fdsv);
        FD_ZERO(&fdsu);

        /* We want both setup and data events on UVC interface.. */
        FD_SET(udev->fd, &fdsu);

        fd_set efds = fdsu;
        fd_set dfds = fdsu;

        /* yield CPU to other processes and avoid spinlock when camera is not being used */
        // fix from - https://github.com/kinweilee/v4l2-mmal-uvc/blob/master/v4l2-mmal-uvc.c
        // rcarmo - https://github.com/peterbay/uvc-gadget/pull/6
        nanosleep ((const struct timespec[]) { {0, 1000000L} }, NULL);

        /* Timeout. */
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        // don't block vdev device if streaming is off
        if (vdev->is_streaming) {
            /* ..but only data events on V4L2 interface */
            FD_SET(vdev->fd, &fdsv);
		
            nfds = max(vdev->fd, udev->fd);
            activity = select(nfds + 1, &fdsv, &dfds, &efds, &tv);

            if (activity == 0) {
                printf("select timeout\n");
                break;
            }

        } else {
            activity = select(udev->fd + 1, NULL, &dfds, &efds, NULL);

        }

        if (activity == -1) {
            printf("select error %d, %s\n", errno, strerror(errno));
            if (EINTR == errno) {
                continue;
            }
            break;
        }

        if (FD_ISSET(udev->fd, &efds)) {
            uvc_events_process(udev);
        }
        
        if (vdev->is_streaming) {

            if (FD_ISSET(udev->fd, &dfds)) {
                uvc_video_process(udev);

                if (settings.show_fps) {
                    gettimeofday(&video_tv, 0);
                    double now = (video_tv.tv_sec + (video_tv.tv_usec * 1e-6)) * 1000;
                    if (now - udev->last_time_video_process >= 1000) {
                        printf("FPS: %d\n", udev->buffers_processed);
                        udev->buffers_processed = 0;
                        udev->last_time_video_process = now;
                    }
                }
            }

            if (FD_ISSET(vdev->fd, &fdsv)) {
                v4l2_process_data(vdev);
            }
        }
    }
}

int init()
{
    struct v4l2_device *udev;
    struct v4l2_device *vdev;

    streaming_status_enable();

    /* Open the UVC device. */
    udev = v4l2_open(settings.uvc_devname, DEVICE_TYPE_UVC);
    if (udev == NULL) {
        return 1;
    }

    udev->nbufs = settings.nbufs;
    udev->memory_type = V4L2_MEMORY_USERPTR;

    /* Open the V4L2 device. */
    vdev = v4l2_open(settings.v4l2_devname, DEVICE_TYPE_V4L2);
    if (vdev == NULL) {
        return 1;
    }

    v4l2_get_available_formats(vdev);
    v4l2_get_controls(vdev);

    vdev->nbufs = settings.nbufs;
    vdev->memory_type = V4L2_MEMORY_MMAP;

    udev->vdev = vdev;
    vdev->udev = udev;
    
    /* Init UVC events. */
    uvc_events_init(udev);

    processing_loop_video(udev, vdev);
 
    printf("\n*** UVC GADGET SHUTDOWN ***\n");

    v4l2_device_stream_off(vdev);
    v4l2_device_stream_off(udev);
    v4l2_close(vdev);
    v4l2_close(udev);

    printf("*** UVC GADGET EXIT ***\n");
    return 0;
}

static int find_text_pos(const char *s, const char *f)
{
  char *p = strstr(s, f);
  return (p) ? p - s : 0;
}

static int configfs_read_value(const char * path)
{
    char buf[20];
    int fd;
    int ret;

    fd = open(path, O_RDONLY); 
    if (fd == -1) {
        return -ENOENT;
    }
    ret = read(fd, buf, 20);
    close(fd);

    if (ret < 0 || ret > 10) {
        return -ENODATA;
    }
    buf[ret] = '\0';
    return strtol(buf, NULL, 10);
}

static void set_uvc_format_index(enum usb_device_speed usb_speed, int video_format,
    unsigned int bFormatIndex)
{
    int i;
    for (i = 0; i <= last_format_index; i++) {
        if (uvc_frame_format[i].usb_speed == usb_speed &&
            uvc_frame_format[i].video_format == video_format
        ) {
            uvc_frame_format[i].bFormatIndex = bFormatIndex;
        }
    }
}

static void set_uvc_format_value(const char * key_word, unsigned int index, int value)
{
    if (!strncmp(key_word, "dwDefaultFrameInterval", 22)) {
        uvc_frame_format[index].dwDefaultFrameInterval = value;

    } else if (!strncmp(key_word, "dwMaxVideoFrameBufferSize", 25)) {
        uvc_frame_format[index].dwMaxVideoFrameBufferSize = value;

    } else if (!strncmp(key_word, "dwMaxBitRate", 12)) {
        uvc_frame_format[index].dwMaxBitRate = value;

    } else if (!strncmp(key_word, "dwMinBitRate", 12)) {
        uvc_frame_format[index].dwMinBitRate = value;

    } else if (!strncmp(key_word, "wHeight", 7)) {
        uvc_frame_format[index].wHeight = value;

    } else if (!strncmp(key_word, "wWidth", 6)) {
        uvc_frame_format[index].wWidth = value;

    } else if (!strncmp(key_word, "bmCapabilities", 14)) {
        uvc_frame_format[index].bmCapabilities = value;

    } else if (!strncmp(key_word, "bFrameIndex", 11)) {
        uvc_frame_format[index].bFrameIndex = value;

    }
}

static int configfs_usb_speed(const char * speed)
{
    if (!strncmp(speed, "fs", 2)) {
        return USB_SPEED_FULL;

    } else if (!strncmp(speed, "hs", 2)) {
        return USB_SPEED_HIGH;

    } else if (!strncmp(speed, "ss", 2)) {
        return USB_SPEED_SUPER;

    }
    return USB_SPEED_UNKNOWN;
}

static int configfs_video_format(const char * format)
{
    if (!strncmp(format, "m", 1)) {
        return V4L2_PIX_FMT_MJPEG;

    } else if (!strncmp(format, "u", 1)) {
        return V4L2_PIX_FMT_YUYV;

    }
    return 0;
}

static void configfs_fill_formats(const char * path, const char * part)
{
    int index = 0;
    int value = 0;
    enum usb_device_speed usb_speed;
    int video_format;
    const char * format_name;
    char * copy = strdup(part);
    char * token = strtok(copy, "/");
    char * array[10];
    
    while (token != NULL)
    {
        array[index++] = token;
        token = strtok (NULL, "/");
    }

    if (index > 3) {
        format_name = array[3];

        usb_speed = configfs_usb_speed(array[0]);
        if (usb_speed == USB_SPEED_UNKNOWN) {
            printf("CONFIGFS: Unsupported USB speed: (%s) %s\n", array[0], path);
            goto free;
        }

        video_format = configfs_video_format(array[2]);
        if (video_format == 0) {
            printf("CONFIGFS: Unsupported format: (%s) %s\n", array[2], path);
            goto free;
        }

        value = configfs_read_value(path);
        if (value < 0) {
            goto free;
        }

        if (!strncmp(array[index - 1], "bFormatIndex", 12)) {
            set_uvc_format_index(usb_speed, video_format, value);
            goto free;
        }

        if (index != 5) {
            goto free;
        }

        if (
            uvc_frame_format[last_format_index].usb_speed != usb_speed ||
            uvc_frame_format[last_format_index].video_format != video_format ||
            strncmp(uvc_frame_format[last_format_index].format_name, format_name, strlen(format_name))
        ) {
            if (uvc_frame_format[last_format_index].defined) {
                last_format_index++;
                
                // too much defined formats
                if (last_format_index > 29) {
                    goto free;
                }
            }

            uvc_frame_format[last_format_index].usb_speed = usb_speed;
            uvc_frame_format[last_format_index].video_format = video_format;
            uvc_frame_format[last_format_index].format_name = strdup(format_name);
            uvc_frame_format[last_format_index].defined = true;
        }

        set_uvc_format_value(array[index - 1], last_format_index, value);
    }

free:
    free(copy);
}

static void configfs_fill_streaming_params(const char* path, const char * part)
{
    int value = configfs_read_value(path);

    /*
     * streaming_maxburst	0..15 (ss only)
     * streaming_maxpacket	1..1023 (fs), 1..3072 (hs/ss)
	 * streaming_interval	1..16
     */

    if (!strncmp(part, "maxburst", 8)) {
        streaming_maxburst = clamp(value, 0, 15);

    } else if (!strncmp(part, "maxpacket", 9)) {
        streaming_maxpacket = clamp(value, 1, 3072);

    } else if (!strncmp(part, "interval", 8)) {
        streaming_interval = clamp(value, 1, 16);

    }
}

static int configfs_path_check(const char* fpath, const struct stat * sb, int tflag)
{
    int uvc = find_text_pos(fpath, "/uvc");
    int streaming = find_text_pos(fpath, "streaming/class/");
    int streaming_params = find_text_pos(fpath, "/streaming_");
    (void)(tflag); /* avoid warning: unused parameter 'tflag' */

    if (!S_ISDIR(sb->st_mode)) {
        if (streaming && uvc) {
            configfs_fill_formats(fpath, fpath + streaming + 16);

        } else if (streaming_params) {
            configfs_fill_streaming_params(fpath, fpath + streaming_params + 11);

        }
    }
	return 0;
}

static int configfs_get_uvc_settings()
{
    int i;
    const char * configfs_path = "/sys/kernel/config/usb_gadget";

    printf("CONFIGFS: Initial path: %s\n", configfs_path);

    if(ftw(configfs_path, configfs_path_check, 20) == -1) {
        return -1;
    }

    if (!uvc_frame_format[0].defined) {
        return -1;
    }

    for (i = 0; i <= last_format_index; i++) {
        uvc_dump_frame_format(&uvc_frame_format[i], "CONFIGFS: UVC");
    }

    printf("CONFIGFS: STREAMING maxburst: %d\n", streaming_maxburst);
    printf("CONFIGFS: STREAMING maxpacket: %d\n", streaming_maxpacket);
    printf("CONFIGFS: STREAMING interval: %d\n", streaming_interval);

    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s [options]\n", argv0);
    fprintf(stderr, "Available options are\n");
    fprintf(stderr, " -h          Print this help screen and exit\n");
    fprintf(stderr, " -l          Use onboard led0 for streaming status indication\n");
    fprintf(stderr, " -n          Number of Video buffers (b/w 2 and 32)\n");
    fprintf(stderr, " -p          GPIO pin number for streaming status indication\n");
    fprintf(stderr, " -u device   UVC Video Output device\n");
    fprintf(stderr, " -v device   V4L2 Video Capture device\n");
    fprintf(stderr, " -x          show fps information\n");
}

static void show_settings()
{
    printf("SETTINGS: Number of buffers requested: %d\n", settings.nbufs);
    printf("SETTINGS: Show FPS: %s\n", (settings.show_fps) ? "ENABLED" : "DISABLED");
    if (settings.streaming_status_pin) {
        printf("SETTINGS: GPIO pin for streaming status: %s\n", settings.streaming_status_pin);
    } else {
        printf("SETTINGS: GPIO pin for streaming status: not set\n");
    }
    printf("SETTINGS: Onboard led0 used for streaming status: %s\n",
        (settings.streaming_status_onboard_enabled) ? "ENABLED" : "DISABLED"
    );
    printf("SETTINGS: UVC device name: %s\n", settings.uvc_devname);
    printf("SETTINGS: V4L2 device name: %s\n", settings.v4l2_devname);
}

int main(int argc, char *argv[])
{
    int ret;
    int opt;

    struct sigaction action;
    CLEAR(action);
    action.sa_handler = term;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    ret = configfs_get_uvc_settings();
    if (ret < 0) {
        printf("ERROR: configfs settings for uvc gadget not found!");
        return 1;
    }

    while ((opt = getopt(argc, argv, "bdlf:hi:m:n:o:p:r:s:t:u:v:x")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 1;

        case 'l':
            settings.streaming_status_onboard = true;
            break;

        case 'n':
            if (atoi(optarg) < 2 || atoi(optarg) > 32) {
                fprintf(stderr, "ERROR: Number of Video buffers value out of range\n");
                goto err;
            }
            settings.nbufs = atoi(optarg);
            break;

        case 'p':
            settings.streaming_status_pin = optarg;
            break;

        case 'u':
            settings.uvc_devname = optarg;
            break;

        case 'v':
            settings.v4l2_devname = optarg;
            break;

        case 'x':
            settings.show_fps = true;
            break;

        default:
            printf("Invalid option '-%c'\n", opt);
            goto err;
        }
    }

    show_settings();
    return init();

err:
    usage(argv[0]);
    return 1;
}
