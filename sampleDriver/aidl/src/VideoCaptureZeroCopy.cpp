/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_TAG "EvsVideoCaptureZeroCopy"

#include "VideoCaptureZeroCopy.h"

#include <aidl/android/hardware/automotive/evs/BufferDesc.h>
#include <aidl/android/hardware/automotive/evs/EvsEventDesc.h>
#include <aidl/android/hardware/automotive/evs/EvsEventType.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android-base/logging.h>
#include <cutils/native_handle.h>
#include <ui/GraphicBufferAllocator.h>
#include <aidl/android/hardware/graphics/common/HardwareBufferDescription.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <android/hardware_buffer.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/SystemClock.h>

#include <android-base/logging.h>

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cassert>
#include <iomanip>

namespace
{

    using aidl::android::hardware::automotive::evs::BufferDesc;
    using aidl::android::hardware::automotive::evs::EvsEventDesc;
    using aidl::android::hardware::automotive::evs::EvsEventType;

} // namespace

// NOTE:  This developmental code does not properly clean up resources in case of failure
//        during the resource setup phase.  Of particular note is the potential to leak
//        the file descriptor.  This must be fixed before using this code for anything but
//        experimentation.
bool VideoCaptureZeroCopy::open(const char *deviceName, const int32_t width, const int32_t height)
{
    LOG(DEBUG) << __FUNCTION__;
    std::unique_lock<std::mutex> lock(mAccessLock);
    // If we want a polling interface for getting frames, we would use O_NONBLOCK
    mDeviceFd = ::open(deviceName, O_RDWR | O_NONBLOCK, 0);
    if (mDeviceFd < 0)
    {
        PLOG(ERROR) << "failed to open device " << deviceName;
        return false;
    }

    v4l2_capability caps;
    {
        int result = ioctl(mDeviceFd, VIDIOC_QUERYCAP, &caps);
        if (result < 0)
        {
            PLOG(ERROR) << "failed to get device caps for " << deviceName;
            return false;
        }
    }

    // Report device properties
    LOG(INFO) << "Open Device: " << deviceName << " (fd = " << mDeviceFd << ")";
    LOG(DEBUG) << "  Driver: " << caps.driver;
    LOG(DEBUG) << "  Card: " << caps.card;
    LOG(DEBUG) << "  Version: " << ((caps.version >> 16) & 0xFF) << "."
               << ((caps.version >> 8) & 0xFF) << "." << (caps.version & 0xFF);
    LOG(DEBUG) << "  All Caps: " << std::hex << std::setw(8) << caps.capabilities;
    LOG(DEBUG) << "  Dev Caps: " << std::hex << caps.device_caps;

    // Enumerate the available capture formats (if any)
    LOG(DEBUG) << "Supported capture formats:";
    v4l2_fmtdesc formatDescriptions;
    formatDescriptions.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (int i = 0; true; i++)
    {
        formatDescriptions.index = i;
        if (ioctl(mDeviceFd, VIDIOC_ENUM_FMT, &formatDescriptions) == 0)
        {
            LOG(DEBUG) << "  " << std::setw(2) << i << ": " << formatDescriptions.description << " "
                       << std::hex << std::setw(8) << formatDescriptions.pixelformat << " "
                       << std::hex << formatDescriptions.flags;
        }
        else
        {
            // No more formats available
            break;
        }
    }

    // Verify we can use this device for video capture
    if (!(caps.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(caps.capabilities & V4L2_CAP_STREAMING))
    {
        // Can't do streaming capture.
        PLOG(ERROR) << "Streaming capture not supported by " << deviceName;
        return false;
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = 30;

    if (ioctl(mDeviceFd,  VIDIOC_S_PARM, &parm) < 0) {
         PLOG(ERROR) << "FPS SET FAIL VIDIOC_S_PARM: " << strerror(errno);
    }

    // Set our desired output format
    v4l2_format format;
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.pixelformat = sDefaultV4LFormat;
    format.fmt.pix.width = sDefaultWidth;
    format.fmt.pix.height = sDefaultHeight;
    format.fmt.pix.field       = V4L2_FIELD_NONE;
    format.fmt.pix.bytesperline = sDefaultStride*sBPPforDefaultFormat;
    LOG(INFO) << "Requesting format: " << ((char *)&format.fmt.pix.pixelformat)[0]
              << ((char *)&format.fmt.pix.pixelformat)[1] << ((char *)&format.fmt.pix.pixelformat)[2]
              << ((char *)&format.fmt.pix.pixelformat)[3] << "(" << std::hex << std::setw(8)
              << format.fmt.pix.pixelformat << ")";

    LOG(INFO) << "Requesting output format:  "
                  << "fmt=0x" << std::hex << format.fmt.pix.pixelformat << ", " << std::dec
                  << format.fmt.pix.width << " x " << format.fmt.pix.height
                  << ", pitch=" << format.fmt.pix.bytesperline;

    if (ioctl(mDeviceFd, VIDIOC_S_FMT, &format) < 0)
    {
        PLOG(ERROR) << "VIDIOC_S_FMT failed";
    }

    // Report the current output format
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(mDeviceFd, VIDIOC_G_FMT, &format) == 0)
    {
        mV4LFormat = format.fmt.pix.pixelformat;
        mWidth = format.fmt.pix.width;
        mHeight = format.fmt.pix.height;
        mStride = format.fmt.pix.bytesperline / sBPPforDefaultFormat;

        LOG(INFO) << "Current output format:  "
                  << "fmt=0x" << std::hex << format.fmt.pix.pixelformat << ", " << std::dec
                  << format.fmt.pix.width << " x " << format.fmt.pix.height
                  << ", pitch=" << format.fmt.pix.bytesperline;
    }
    else
    {
        PLOG(ERROR) << "VIDIOC_G_FMT failed";
        return false;
    }

    // Make sure we're initialized to the STOPPED state
    mRunMode = STOPPED;
    mFrames.clear();

    // Ready to go!
    return true;
}

void VideoCaptureZeroCopy::close()
{
    LOG(DEBUG) << __FUNCTION__;
    // Stream should be stopped first!
    assert(mRunMode == STOPPED);

    if (isOpen())
    {
        LOG(DEBUG) << "closing video device file handle " << mDeviceFd;
        std::unique_lock<std::mutex> lock(mAccessLock);
        ::close(mDeviceFd);
        mDeviceFd = -1;
    }
}

static bool getDmaBuf(const native_handle_t* handle, int* outFd, size_t* outSize) {
    if (handle == nullptr || handle->numFds < 1) {
        LOG(ERROR) << "getDmaBuf: bad handle, numFds="
                   << (handle ? handle->numFds : -1);
        return false;
    }

    int bestFd = -1;
    off_t bestSize = 0;

    // Several fds may be packed in the handle (buffer + metadata).
    // Pick the largest one — the real pixel buffer dwarfs metadata fds.
    for (int i = 0; i < handle->numFds; ++i) {
        int fd = handle->data[i];
        if (fd < 0) {
            continue;
        }
        off_t sz = lseek(fd, 0, SEEK_END);
        if (sz <= 0) {
            // not a regular/seekable dma-buf, or empty — skip
            continue;
        }
        if (sz > bestSize) {
            bestSize = sz;
            bestFd = fd;
        }
    }

    if (bestFd < 0) {
        LOG(ERROR) << "getDmaBuf: no usable dma-buf fd in handle";
        return false;
    }

    *outFd = bestFd;
    *outSize = static_cast<size_t>(bestSize);
    return true;
}

bool VideoCaptureZeroCopy::startStream(const std::vector<BufferRecord>& buffers, VideoCaptureCallback callback)
{
    LOG(INFO) << "Starting streaming from video device "
              << mDeviceFd;

    // Set the state of our background thread
    int prevRunMode = mRunMode.fetch_or(RUN);
    if (prevRunMode & RUN)
    {
        // The background thread is already running, so we can't start a new stream
        LOG(ERROR) << "Already in RUN state, so we can't start a new streaming thread";
        return false;
    }

    std::unique_lock<std::mutex> lock(mAccessLock);
    LOG(INFO) << "Registered " << buffers.size() << " buffers for streaming.";
    // Tell the L4V2 driver to prepare our streaming buffers
    v4l2_requestbuffers bufrequest;
    bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufrequest.memory = V4L2_MEMORY_DMABUF;
    bufrequest.count = buffers.size();
    if (ioctl(mDeviceFd, VIDIOC_REQBUFS, &bufrequest) < 0)
    {
        PLOG(ERROR) << "VIDIOC_REQBUFS failed";
        return false;
    }
    LOG(INFO) << "Requested " << bufrequest.count << " buffers for streaming.";
    mNumBuffers = bufrequest.count;
    mBufferInfos = std::make_unique<v4l2_buffer[]>(mNumBuffers);
    mPixelBuffers = std::make_unique<void *[]>(mNumBuffers);

    for (int i = 0; i < mNumBuffers; ++i)
    {

        int    dmabuf_fd = -1;
        size_t dmabuf_sz = 0;

        if (!getDmaBuf(buffers[i].handle, &dmabuf_fd, &dmabuf_sz)) {
            LOG(INFO) << "Unable to get DMA buffer from the native handle";
            return false;
        }
        LOG(INFO) << "DMA buffer fd = " << dmabuf_fd << " size = " << std::dec << dmabuf_sz;

        memset(&mBufferInfos[i], 0, sizeof(v4l2_buffer));

        mBufferInfos[i].type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        mBufferInfos[i].memory = V4L2_MEMORY_DMABUF;
        mBufferInfos[i].index = i;
        mBufferInfos[i].length = sBPPforDefaultFormat * sDefaultHeight * sDefaultStride;

        LOG(INFO) << "Buffer " << i << " length = " << std::dec
                  << mBufferInfos[i].length;
        if (ioctl(mDeviceFd, VIDIOC_QUERYBUF, &mBufferInfos[i]) < 0)
        {
            PLOG(ERROR) << "VIDIOC_QUERYBUF failed";
            return false;
        }

        LOG(INFO) << "Buffer description:";
        LOG(INFO) << "  offset: " << mBufferInfos[i].m.offset;
        LOG(INFO) << "  length: " << mBufferInfos[i].length;

        mBufferInfos[i].type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        mBufferInfos[i].memory = V4L2_MEMORY_DMABUF;
        mBufferInfos[i].index  = i;
        mBufferInfos[i].m.fd   = dmabuf_fd;
        mBufferInfos[i].length = dmabuf_sz;

        if (ioctl(mDeviceFd, VIDIOC_QBUF, &mBufferInfos[i]) < 0) {
            PLOG(ERROR) << "VIDIOC_QBUF failed";
            return false;
        }
        LOG(DEBUG) << "VIDIOC_QBUF buffer id = " << i;
    }

    // Start the video stream
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(mDeviceFd, VIDIOC_STREAMON, &type) < 0)
    {
        PLOG(ERROR) << "VIDIOC_STREAMON failed";
        return false;
    }

    // Remember who to tell about new frames as they arrive
    mCallback = callback;

    // Fire up a thread to receive and dispatch the video frames
    mCaptureThread = std::thread([this]()
                                 { collectFrames(); });

    LOG(DEBUG) << "Stream started.";
    return true;
}

void VideoCaptureZeroCopy::stopStream()
{
    // Tell the background thread to stop
    std::unique_lock<std::mutex> lock(mAccessLock);
    LOG(DEBUG) << __FUNCTION__;
    int prevRunMode = mRunMode.fetch_or(STOPPING);
    if (prevRunMode == STOPPED)
    {
        // The background thread wasn't running, so set the flag back to STOPPED
        mRunMode = STOPPED;
    }
    else if (prevRunMode & STOPPING)
    {
        LOG(ERROR) << "stopStream called while stream is already stopping.  "
                   << "Reentrancy is not supported!";
        return;
    }
    else
    {
        // Block until the background thread is stopped
        if (mCaptureThread.joinable())
        {
            mCaptureThread.join();
        }

        // Stop the underlying video stream (automatically empties the buffer queue)
        const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(mDeviceFd, VIDIOC_STREAMOFF, &type) < 0)
        {
            PLOG(ERROR) << "VIDIOC_STREAMOFF failed";
        }

        LOG(DEBUG) << "Capture thread stopped.";
    }

    // Tell the L4V2 driver to release our streaming buffers
    v4l2_requestbuffers bufrequest;
    bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufrequest.memory = V4L2_MEMORY_DMABUF;
    bufrequest.count = 0;
    ioctl(mDeviceFd, VIDIOC_REQBUFS, &bufrequest);

    for (int i = 0; i < mNumBuffers; ++i)
    {
        mPixelBuffers[i] = nullptr;
        // Clear the buffer info
        memset(&mBufferInfos[i], 0, sizeof(v4l2_buffer));
    }

    // Drop our reference to the frame delivery callback interface
    mCallback = nullptr;

    // Release capture buffers
    mNumBuffers = 0;
    mBufferInfos = nullptr;
    mPixelBuffers = nullptr;
}

bool VideoCaptureZeroCopy::returnFrame(int id)
{
    std::unique_lock<std::mutex> lock(mAccessLock);
    if (mFrames.find(id) == mFrames.end())
    {
        LOG(WARNING) << "Invalid request to return a buffer " << id << " is ignored.";
        return false;
    }

    // Requeue the buffer to capture the next available frame
    if (mRunMode == RUN && ioctl(mDeviceFd, VIDIOC_QBUF, &mBufferInfos[id]) < 0)
    {
        PLOG(ERROR) << "VIDIOC_QBUF failed";
        return false;
    }

    // Remove ID of returned buffer from the set
    mFrames.erase(id);

    return true;
}

// This runs on a background thread to receive and dispatch video frames
void VideoCaptureZeroCopy::collectFrames()
{
    // Run until our atomic signal is cleared

    while (mRunMode == RUN)
    {
        struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_DMABUF};
        {

            LOG(VERBOSE) << "VIDIOC_DQBUF";
            int ret = 0;
            while (true)
            {
                std::unique_lock<std::mutex> lock(mAccessLock);
                ret = ioctl(mDeviceFd, VIDIOC_DQBUF, &buf);
                if (ret == 0)
                {
                    break;
                }

                if (errno == EAGAIN)
                {
                    // No buffer ready, retry after short sleep or continue immediately
                    lock.unlock();
                    usleep(5000); // 5ms backoff
                    if (mRunMode != RUN)
                    {
                        LOG(DEBUG) << "VideoCaptureZeroCopy thread ending";
                        mRunMode = STOPPED;
                        return;
                    }
                    lock.lock();
                    continue;
                }
                if (ret != 0)
                {
                    // Real error
                    PLOG(ERROR) << "VIDIOC_DQBUF failed";
                    return;
                }
            }
            LOG(VERBOSE) << "VIDIOC_DQBUF id=" << buf.index;
            mFrames.insert(buf.index);

            // Update a frame metadata
            mBufferInfos[buf.index] = buf;
        }
        // If a callback was requested per frame, do that now
        if (mCallback)
        {
            mCallback(this, &mBufferInfos[buf.index], mPixelBuffers[buf.index]);
        }
    }

    // Mark ourselves stopped
    LOG(DEBUG) << "VideoCaptureZeroCopy thread ending";
    mRunMode = STOPPED;
}

int VideoCaptureZeroCopy::setParameter(v4l2_control &control)
{
    std::unique_lock<std::mutex> lock(mAccessLock);
    int status = ioctl(mDeviceFd, VIDIOC_S_CTRL, &control);
    if (status < 0)
    {
        PLOG(ERROR) << "Failed to program a parameter value "
                    << "id = " << std::hex << control.id;
    }

    return status;
}

int VideoCaptureZeroCopy::getParameter(v4l2_control &control)
{
    std::unique_lock<std::mutex> lock(mAccessLock);
    int status = ioctl(mDeviceFd, VIDIOC_G_CTRL, &control);
    if (status < 0)
    {
        PLOG(ERROR) << "Failed to read a parameter value"
                    << " fd = " << std::hex << mDeviceFd << " id = " << control.id;
    }

    return status;
}

std::set<uint32_t> VideoCaptureZeroCopy::enumerateCameraControls()
{
    std::unique_lock<std::mutex> lock(mAccessLock);
    // Retrieve available camera controls
    struct v4l2_queryctrl ctrl = {.id = V4L2_CTRL_FLAG_NEXT_CTRL};

    std::set<uint32_t> ctrlIDs;
    while (0 == ioctl(mDeviceFd, VIDIOC_QUERYCTRL, &ctrl))
    {
        if (!(ctrl.flags & V4L2_CTRL_FLAG_DISABLED))
        {
            ctrlIDs.insert(ctrl.id);
        }

        ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }

    if (errno != EINVAL)
    {
        PLOG(WARNING) << "Failed to run VIDIOC_QUERYCTRL";
    }

    return std::move(ctrlIDs);
}
