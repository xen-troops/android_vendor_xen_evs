/*
 * Copyright (C) 2022 The Android Open Source Project
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
#ifndef CPP_EVS_SAMPLEDRIVER_AIDL_INCLUDE_VIDEOCAPTUREZEROCOPY_H
#define CPP_EVS_SAMPLEDRIVER_AIDL_INCLUDE_VIDEOCAPTUREZEROCOPY_H

#include <aidl/android/hardware/automotive/evs/BufferDesc.h>
#include <android/hardware_buffer.h>
#include <ui/GraphicBuffer.h>

#include <linux/videodev2.h>

#include <atomic>
#include <functional>
#include <set>
#include <thread>

#define V4L2_PIX_FMT_XR24 0x34325258

typedef v4l2_buffer imageBuffer;

    struct BufferRecord {
        buffer_handle_t handle;
        bool inUse;

        explicit BufferRecord(buffer_handle_t h) : handle(h), inUse(false){};
    };


class VideoCaptureZeroCopy final {

    using VideoCaptureCallback = std::function<void(VideoCaptureZeroCopy*, imageBuffer*, void*)>;

public:
    bool open(const char* deviceName, const int32_t width = 0, const int32_t height = 0);
    void close();

    bool startStream(const std::vector<BufferRecord>& buffers, VideoCaptureCallback callback = nullptr);
    void stopStream();

    // Valid only after open()
    __u32 getWidth() { return mWidth; };
    __u32 getHeight() { return mHeight; };
    __u32 getStride() { return mStride; };
    __u32 getV4LFormat() { return mV4LFormat; };
    static uint32_t getDefaultBufferCount() { return sDefaultBufferCount; };
    static uint32_t getDefaultWidth() { return sDefaultWidth; };
    static uint32_t getDefaultHeight() { return sDefaultHeight; };
    static uint32_t getDefaultFormat() { return sDefaultFormat; };
    static uint64_t getDefaultUsage() { return sDefaultUsage; };

    bool isFrameReady() { return !mFrames.empty(); }
    void markFrameConsumed(int id) { returnFrame(id); }

    bool isOpen() { return mDeviceFd >= 0; }

    int setParameter(struct v4l2_control& control);
    int getParameter(struct v4l2_control& control);
    std::set<uint32_t> enumerateCameraControls();
    bool registerBuffers(std::vector<BufferRecord>& buffers);
    bool unRegisterBuffers();

private:
    void collectFrames();
    bool returnFrame(int id);

    int mDeviceFd = -1;

    int mNumBuffers = 0;
    std::unique_ptr<v4l2_buffer[]> mBufferInfos = nullptr;
    std::unique_ptr<void*[]> mPixelBuffers = nullptr;

    __u32 mWidth = 0;
    __u32 mHeight = 0;
    __u32 mStride = 0;
    __u32 mV4LFormat = 0;

    std::function<void(VideoCaptureZeroCopy*, imageBuffer*, void*)> mCallback;

    std::thread mCaptureThread;  // The thread we'll use to dispatch frames
    std::atomic<int> mRunMode;   // Used to signal the frame loop (see RunModes below)
    std::set<int> mFrames;       // Set of available frame buffers

    // Careful changing these -- we're using bit-wise ops to manipulate these
    enum RunModes {
        STOPPED = 0,
        RUN = 1,
        STOPPING = 2,
    };

    std::vector<buffer_handle_t> mRegisteredBuffers;
    std::atomic<bool> mBuffersRegistered = false;
    mutable std::mutex mAccessLock;
    constexpr static uint32_t sDefaultBufferCount = 2;
    constexpr static uint32_t sDefaultWidth = 1936;
    constexpr static uint32_t sDefaultHeight = 1552;
    constexpr static uint32_t sDefaultStride = 2048;
    constexpr static uint32_t sDefaultFormat = HAL_PIXEL_FORMAT_BGRA_8888;
    constexpr static uint64_t sDefaultUsage = GRALLOC_USAGE_HW_CAMERA_READ | GRALLOC_USAGE_HW_CAMERA_WRITE | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_FB | GRALLOC_USAGE_SW_READ_RARELY | GRALLOC_USAGE_SW_WRITE_OFTEN;
    constexpr static uint32_t sDefaultV4LFormat = V4L2_PIX_FMT_XR24;  // Default V4L2 format
    constexpr static uint32_t sBPPforDefaultFormat = 4;  // Bytes per pixel for default format
};

#endif  // CPP_EVS_SAMPLEDRIVER_AIDL_INCLUDE_VIDEOCAPTUREZEROCOPY_H
