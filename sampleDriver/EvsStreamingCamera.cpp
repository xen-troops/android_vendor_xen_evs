/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "EvsStreamingCamera.h"
#include "EvsEnumerator.h"
#include "bufferCopy.h"


#include <sys/types.h>
#include <sys/stat.h>

#include <android/hardware_buffer.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/SystemClock.h>

#include <cutils/properties.h>
#include <string>

namespace android {
namespace hardware {
namespace automotive {
namespace evs {
namespace V1_1 {
namespace implementation {

// Arbitrary limit on number of graphics buffers allowed to be allocated
// Safeguards against unreasonable resource consumption and provides a testable limit
static const unsigned MAX_BUFFERS_IN_FLIGHT = 100;

using easywsclient::WebSocket;

EvsStreamingCamera::EvsStreamingCamera(const char *deviceName, 
                                       unique_ptr<ConfigManager::CameraInfo> &camInfo,
                                       const Stream *streamCfg) 
   : mFramesAllowed(0),
     mFramesInUse(0),
     mCameraInfo(camInfo),
     mUri(),
     mIsActive(false),
     ws(NULL) {

    (void)mCameraInfo;
    mFramesAllowed = 0;
    mFramesInUse = 0;
    mDescription.v1.cameraId = deviceName;

    if (camInfo != nullptr) {
        mDescription.metadata.setToExternal((uint8_t *)camInfo->characteristics, get_camera_metadata_size(camInfo->characteristics));
    }

    // Default output buffer format.
    // mFormat = HAL_PIXEL_FORMAT_BGRA_8888;
    mFormat = HAL_PIXEL_FORMAT_RGBA_8888;

    // How we expect to use the gralloc buffers we'll exchange with our client
    mUsage  = GRALLOC_USAGE_HW_TEXTURE     |
              GRALLOC_USAGE_SW_READ_RARELY |
              GRALLOC_USAGE_SW_WRITE_OFTEN;

    char propValue[PROPERTY_VALUE_MAX] = {};
    property_get("persist.vendor.evs.uri", propValue, "ws://192.168.122.1");
    mUri = propValue;

    if (!std::string(deviceName).compare("CarlaSimFront")) {
        mUri += ":8887";
    } else if (!std::string(deviceName).compare("CarlaSimReverse")) {
        mUri += ":8889";
    } else if (!std::string(deviceName).compare("CarlaSimLeft")) {
        mUri += ":8890";
    } else if (!std::string(deviceName).compare("CarlaSimRight")) {
        mUri += ":8891";
    }

    LOG(DEBUG) << "EvsStreamingCamera instantiated " << deviceName << " " << mUri;
    if (camInfo != nullptr && streamCfg != nullptr) {
        // Validate a given stream configuration.  If there is no exact match,
        // this will try to find the best match based on:
        // 1) same output format
        // 2) the largest resolution that is smaller that a given configuration.
        int32_t streamId = -1, area = INT_MIN;
        for (auto& [id, cfg] : camInfo->streamConfigurations) {
            // RawConfiguration has id, width, height, format, direction, and
            // fps.
            if (cfg[3] == static_cast<uint32_t>(streamCfg->format)) {
                if (cfg[1] == streamCfg->width &&
                    cfg[2] == streamCfg->height) {
                    // Find exact match.
                    streamId = id;
                    break;
                } else if (streamCfg->width  > cfg[1] &&
                           streamCfg->height > cfg[2] &&
                           cfg[1] * cfg[2] > area) {
                    streamId = id;
                    area = cfg[1] * cfg[2];
                }
            }

        }

        if (streamId >= 0) {
            LOG(INFO) << "Try to open a video with "
                      << "width: " << camInfo->streamConfigurations[streamId][1]
                      << ", height: " << camInfo->streamConfigurations[streamId][2]
                      << ", format: " << camInfo->streamConfigurations[streamId][3];
        }
    }

}

EvsStreamingCamera::~EvsStreamingCamera() {
    LOG(DEBUG) << "EvsStreamingCamera being destroyed";

    shutdown();
}

sp<EvsStreamingCamera> EvsStreamingCamera::Create(const char *deviceName) {
    LOG(DEBUG) << __FUNCTION__;

    unique_ptr<ConfigManager::CameraInfo> nullCamInfo = nullptr;
    return Create(deviceName, nullCamInfo);
}

sp<EvsStreamingCamera> EvsStreamingCamera::Create(const char *deviceName,
                                      unique_ptr<ConfigManager::CameraInfo> &camInfo,
                                      const Stream *requestedStreamCfg) {
    LOG(INFO) << "Create " << deviceName;

    (void)requestedStreamCfg;
    sp<EvsStreamingCamera> evsStreamingCamera = new EvsStreamingCamera(deviceName, camInfo, requestedStreamCfg);
    if (evsStreamingCamera == nullptr) {
        return nullptr;
    }

    return evsStreamingCamera;
}

//
// This gets called if another caller "steals" ownership of the camera
//
void EvsStreamingCamera::shutdown()
{
    LOG(DEBUG) << "EvsStreamingCamera shutdown";

    // Make sure our output stream is cleaned up
    // (It really should be already)
    stopVideoStream();

    // Note:  Since stopVideoStream is blocking, no other threads can now be running
    // Drop all the graphics buffers we've been using
    if (mBuffers.size() > 0) {
        GraphicBufferAllocator& alloc(GraphicBufferAllocator::get());
        for (auto&& rec : mBuffers) {
            if (rec.inUse) { LOG(WARNING) << "Releasing buffer despite remote ownership"; }
            alloc.free(rec.handle);
            rec.handle = nullptr;
        }
        mBuffers.clear();
    }
}

// Methods from ::android::hardware::automotive::evs::V1_0::IEvsCamera follow.
Return<void> EvsStreamingCamera::getCameraInfo(getCameraInfo_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;

    // Send back our self description
    _hidl_cb(mDescription.v1);
    return Void();
}

Return<EvsResult> EvsStreamingCamera::setMaxFramesInFlight(uint32_t bufferCount) {
    LOG(DEBUG) << __FUNCTION__ << " bufferCount " << bufferCount;
    std::lock_guard<std::mutex> lock(mAccessLock);
    
    // We cannot function without at least one video buffer to send data
    if (bufferCount < 1) {
        LOG(ERROR) << "Ignoring setMaxFramesInFlight with less than one buffer requested";
        return EvsResult::INVALID_ARG;
    }

    // Update our internal state
    if (setAvailableFrames_Locked(bufferCount)) {
        return EvsResult::OK;
    } else {
        return EvsResult::BUFFER_NOT_AVAILABLE;
    }

    return EvsResult::OK;
}

Return<EvsResult> EvsStreamingCamera::startVideoStream(const sp<IEvsCameraStream_1_0>& stream)  {
    LOG(DEBUG) << __FUNCTION__ << "#startVideoStream 1_0";

    // If the client never indicated otherwise, configure ourselves for a single streaming buffer
    if (mFramesAllowed < 1) {
        if (!setAvailableFrames_Locked(1)) {
            LOG(ERROR) << "Failed to start stream because we couldn't get a graphics buffer";
            return EvsResult::BUFFER_NOT_AVAILABLE;
        }
    }

    if (mIsActive) {
        LOG(ERROR) << "STREAM_ALREADY_RUNNING";
        return EvsResult::STREAM_ALREADY_RUNNING; 
    }

    mIsActive = true;

    // Record the user's callback for use when we have a frame ready
    mStream = stream;
    mStream_1_1 = IEvsCameraStream_1_1::castFrom(mStream).withDefault(nullptr);
    
    mThread = std::thread ([this]() {
        LOG(DEBUG) << "Start poll";
        ws = WebSocket::from_url(mUri);

        while ((ws->getReadyState() != WebSocket::CLOSED) && (true == mIsActive)) {
            ws->poll();
            ws->dispatchBinary([this] (const std::vector<uint8_t>& v) { this->forwardFrame(v); });
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        LOG(DEBUG) << "Stop poll";
        ws->close();
    });

    LOG(DEBUG) << "Result OK";
    return EvsResult::OK;
}

Return<void> EvsStreamingCamera::stopVideoStream()  {
    LOG(DEBUG) << __FUNCTION__;

    mIsActive = false;
    if (mThread.joinable()) { mThread.join(); }

    if (mStream_1_1 != nullptr) {
        // V1.1 client is waiting on STREAM_STOPPED event.
        std::unique_lock <std::mutex> lock(mAccessLock);

        EvsEventDesc event;
        event.aType = EvsEventType::STREAM_STOPPED;
        auto result = mStream_1_1->notify(event);
        if (!result.isOk()) {
            LOG(ERROR) << "Error delivering end of stream event";
        }

        // Drop our reference to the client's stream receiver
        mStream_1_1 = nullptr;
        mStream     = nullptr;
    } else if (mStream != nullptr) {
        std::unique_lock <std::mutex> lock(mAccessLock);

        // Send one last NULL frame to signal the actual end of stream
        BufferDesc_1_0 nullBuff = {};
        auto result = mStream->deliverFrame(nullBuff);
        if (!result.isOk()) {
            LOG(ERROR) << "Error delivering end of stream marker";
        }

        // Drop our reference to the client's stream receiver
        mStream = nullptr;
    }

    return Void();
}

// This is the async callback from the video camera that tells us a frame is ready
void EvsStreamingCamera::forwardFrame(const std::vector<uint8_t>& v) {
    LOG(DEBUG) << "forwardFrame " << v.size() << " mFramesInUse " 
               << mFramesInUse << " mFramesAllowed " << mFramesAllowed;

    bool readyForFrame = false;
    size_t idx = 0;

    // Lock scope for updating shared state
    {
        std::lock_guard<std::mutex> lock(mAccessLock);

        // Are we allowed to issue another buffer?
        if (mFramesInUse >= mFramesAllowed) {
            // Can't do anything right now -- skip this frame
            LOG(WARNING) << "Skipped a frame because too many are in flight";
        } else {
            // Identify an available buffer to fill
            for (idx = 0; idx < mBuffers.size(); idx++) {
                if (!mBuffers[idx].inUse) {
                    if (mBuffers[idx].handle != nullptr) {
                        LOG(DEBUG) << "Found an available record, so stop looking";
                        break;
                    }
                }
            }
            if (idx >= mBuffers.size()) {
                // This shouldn't happen since we already checked mFramesInUse vs mFramesAllowed
                LOG(ERROR) << "Failed to find an available buffer slot";
            } else {
                LOG(DEBUG) << "We're going to make the frame busy";
                mBuffers[idx].inUse = true;
                mFramesInUse++;
                readyForFrame = true;
            }
        }
    }

    // Assemble the buffer description we'll transmit below
    BufferDesc_1_1 bufDesc_1_1 = {};
    AHardwareBuffer_Desc* pDesc = reinterpret_cast<AHardwareBuffer_Desc *>(&bufDesc_1_1.buffer.description);
    pDesc->width  = mWidth;
    pDesc->height = mHeight;
    pDesc->layers = 1;
    pDesc->format = mFormat;
    pDesc->usage  = mUsage;
    pDesc->stride = mStride;
    bufDesc_1_1.buffer.nativeHandle = mBuffers[idx].handle;
    bufDesc_1_1.bufferId = idx;
    bufDesc_1_1.deviceId = mDescription.v1.cameraId;
    // timestamp in microseconds.
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    bufDesc_1_1.timestamp = ts.tv_sec * 1000000000 + ts.tv_nsec;;

    // Lock our output buffer for writing
    // TODO(b/145459970): Sometimes, physical camera device maps a buffer
    // into the address that is about to be unmapped by another device; this
    // causes SEGV_MAPPER.
    void *targetPixels = nullptr;
    GraphicBufferMapper& mapper = GraphicBufferMapper::get();
    status_t result = mapper.lock(bufDesc_1_1.buffer.nativeHandle,
                                  GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_NEVER,
                                  android::Rect(pDesc->width, pDesc->height),
                                  (void **)&targetPixels);

    // If we failed to lock the pixel buffer, we're about to crash, but log it first
    if (!targetPixels) {
        // TODO(b/145457727): When EvsHidlTest::CameraToDisplayRoundTrip
        // test case was repeatedly executed, EVS occasionally fails to map
        // a buffer.
        LOG(ERROR) << "Camera failed to gain access to image buffer for writing - "
                    << " status: " << statusToString(result)
                    << " , error: " << strerror(errno);
    }

    // HAL_PIXEL_FORMAT_RGBA_8888 default output format
    // Transfer the camera image into the output buffer
    int total = 0;
    const unsigned dstStride = pDesc->stride * 4/*bytesPerPixel*/;
    const unsigned numRows   = pDesc->height;
    uint8_t* dst = reinterpret_cast<uint8_t*>(targetPixels);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(v.data());
    for (auto r = 0; r < numRows; ++r) {
        memcpy(dst, src, dstStride);
        // Moves to the next row
        src += dstStride;
        dst += dstStride;
        total += dstStride;

       // Carla GRBA to Android RGBA
       // for (int offset = 0; offset < dstStride - 4; offset +=4) {
       //     auto temp = dst[offset];
       //     dst[offset] = dst[offset + 2];
       //     dst[offset + 2] = temp;
       // }
    }
    LOG(ERROR) << "Total copy " << total;

    // Unlock the output buffer
    mapper.unlock(bufDesc_1_1.buffer.nativeHandle);

    // Issue the (asynchronous) callback to the client -- can't be holding
    // the lock
    bool flag = false;
    if (mStream_1_1 != nullptr) {
        hidl_vec<BufferDesc_1_1> frames;
        frames.resize(1);
        frames[0] = bufDesc_1_1;
        auto result = mStream_1_1->deliverFrame_1_1(frames);
        flag = result.isOk();
    } else {
        BufferDesc_1_0 bufDesc_1_0 = {
            pDesc->width,
            pDesc->height,
            pDesc->stride,
            bufDesc_1_1.pixelSize,
            static_cast<uint32_t>(pDesc->format),
            static_cast<uint32_t>(pDesc->usage),
            bufDesc_1_1.bufferId,
            bufDesc_1_1.buffer.nativeHandle
        };

        auto result = mStream->deliverFrame(bufDesc_1_0);
        flag = result.isOk();
    }

    if (flag) {
        LOG(DEBUG) << "Delivered " << bufDesc_1_1.buffer.nativeHandle.getNativeHandle()
                    << " as id " << bufDesc_1_1.bufferId;
    } else {
        // This can happen if the client dies and is likely unrecoverable.
        // To avoid consuming resources generating failing calls, we stop sending
        // frames.  Note, however, that the stream remains in the "STREAMING" state
        // until cleaned up on the main thread.
        LOG(ERROR) << "Frame delivery call failed in the transport layer.";

        // Since we didn't actually deliver it, mark the frame as available
        std::lock_guard<std::mutex> lock(mAccessLock);
        mBuffers[idx].inUse = false;

        mFramesInUse--;
    }

    if (mDumpFrame) {
        // Construct a target filename with the device identifier
        std::string filename = std::string("EvsCarla");
        std::replace(filename.begin(), filename.end(), '/', '_');
        filename = mDumpPath + filename + "_" + std::to_string(mFrameCounter) + ".bin";

        android::base::unique_fd fd(open(filename.c_str(),
                                         O_WRONLY | O_CREAT,
                                         S_IRUSR | S_IWUSR | S_IRGRP));
        LOG(ERROR) << filename << ", " << fd;
        if (fd == -1) {
            PLOG(ERROR) << "Failed to open a file, " << filename;
        } else {
            auto width = mWidth;
            auto height = mHeight;
            auto len = write(fd.get(), &width, sizeof(width));
            len += write(fd.get(), &height, sizeof(height));
            len += write(fd.get(), &mStride, sizeof(mStride));
            len += write(fd.get(), &mFormat, sizeof(mFormat));
            len += write(fd.get(), v.data(), v.size());
            LOG(INFO) << len << " bytes are written to " << filename;
        }
    }
    // Increse a frame counter
    ++mFrameCounter;

}

Return<void> EvsStreamingCamera::doneWithFrame(const BufferDesc_1_0& buffer)  {
    LOG(DEBUG) << __FUNCTION__;
    doneWithFrame_impl(buffer.bufferId, buffer.memHandle);

    return Void();
}

Return<int32_t> EvsStreamingCamera::getExtendedInfo(uint32_t /*opaqueIdentifier*/)  {
    LOG(DEBUG) << __FUNCTION__;
    // Return zero by default as required by the spec
    return 0;
}

Return<EvsResult> EvsStreamingCamera::setExtendedInfo(uint32_t /*opaqueIdentifier*/,
                                                int32_t  /*opaqueValue*/)  {
    LOG(DEBUG) << __FUNCTION__;
   
    return EvsResult::INVALID_ARG;
}

// Methods from ::android::hardware::automotive::evs::V1_1::IEvsCamera follow.
Return<void> EvsStreamingCamera::getCameraInfo_1_1(getCameraInfo_1_1_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;

    // Send back our self description
    _hidl_cb(mDescription);
    return Void();
}

Return<void> EvsStreamingCamera::getPhysicalCameraInfo(const hidl_string& id,
                                                 getPhysicalCameraInfo_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;

    // This method works exactly same as getCameraInfo_1_1() in EVS HW module.
    (void)id;
    _hidl_cb(mDescription);
    return Void();
}

Return<EvsResult> EvsStreamingCamera::doneWithFrame_1_1(const hidl_vec<BufferDesc_1_1>& buffers)  {
    LOG(DEBUG) << __FUNCTION__;

    for (auto&& buffer : buffers) {
        doneWithFrame_impl(buffer.bufferId, buffer.buffer.nativeHandle);
    }

    return EvsResult::OK;
}

Return<EvsResult> EvsStreamingCamera::pauseVideoStream() {
    return EvsResult::UNDERLYING_SERVICE_ERROR;
}

Return<EvsResult> EvsStreamingCamera::resumeVideoStream() {
    return EvsResult::UNDERLYING_SERVICE_ERROR;
}

Return<EvsResult> EvsStreamingCamera::setMaster() {
    /* Because EVS HW module reference implementation expects a single client at
     * a time, this returns a success code always.
     */
    return EvsResult::OK;
}

Return<EvsResult> EvsStreamingCamera::forceMaster(const sp<IEvsDisplay_1_0>&) {
    /* Because EVS HW module reference implementation expects a single client at
     * a time, this returns a success code always.
     */
    return EvsResult::OK;
}

Return<EvsResult> EvsStreamingCamera::unsetMaster() {
    /* Because EVS HW module reference implementation expects a single client at
     * a time, there is no chance that this is called by the secondary client and
     * therefore returns a success code always.
     */
    return EvsResult::OK;
}

Return<void> EvsStreamingCamera::getParameterList(getParameterList_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;
    (void)_hidl_cb;
    return Void();
}

Return<void> EvsStreamingCamera::getIntParameterRange(CameraParam id, getIntParameterRange_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;
    (void)id;
    (void)_hidl_cb;
    return Void();
}

Return<void> EvsStreamingCamera::setIntParameter(CameraParam id, int32_t value, setIntParameter_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;
    (void)id;
    (void)value;
    (void)_hidl_cb;
    return Void();
}

Return<void> EvsStreamingCamera::getIntParameter(CameraParam id, getIntParameter_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;
    (void)id;
    (void)_hidl_cb;
    return Void();
}

Return<EvsResult> EvsStreamingCamera::setExtendedInfo_1_1(uint32_t opaqueIdentifier, const hidl_vec<uint8_t>& opaqueValue) {
    LOG(DEBUG) << __FUNCTION__;
    (void)opaqueIdentifier;
    (void)opaqueValue;
    return EvsResult::OK;
}

Return<void> EvsStreamingCamera::getExtendedInfo_1_1(uint32_t opaqueIdentifier, getExtendedInfo_1_1_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;
    (void)opaqueIdentifier;
    (void)_hidl_cb;
    return Void();
}

Return<void>
EvsStreamingCamera::importExternalBuffers(const hidl_vec<BufferDesc_1_1>& buffers, importExternalBuffers_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;

    auto numBuffersToAdd = buffers.size();
    if (numBuffersToAdd < 1) {
        LOG(DEBUG) << "No buffers to add.";
        _hidl_cb(EvsResult::OK, mFramesAllowed);
        return {};
    }

    {
        std::scoped_lock<std::mutex> lock(mAccessLock);

        if (numBuffersToAdd > (MAX_BUFFERS_IN_FLIGHT - mFramesAllowed)) {
            numBuffersToAdd -= (MAX_BUFFERS_IN_FLIGHT - mFramesAllowed);
            LOG(WARNING) << "Exceed the limit on number of buffers.  "
                         << numBuffersToAdd << " buffers will be added only.";
        }

        GraphicBufferMapper& mapper = GraphicBufferMapper::get();
        const auto before = mFramesAllowed;
        for (auto i = 0; i < numBuffersToAdd; ++i) {
            // TODO: reject if external buffer is configured differently.
            auto& b = buffers[i];
            const AHardwareBuffer_Desc* pDesc =
                reinterpret_cast<const AHardwareBuffer_Desc *>(&b.buffer.description);

            // Import a buffer to add
            buffer_handle_t memHandle = nullptr;
            status_t result = mapper.importBuffer(b.buffer.nativeHandle,
                                                  pDesc->width,
                                                  pDesc->height,
                                                  1,
                                                  pDesc->format,
                                                  pDesc->usage,
                                                  pDesc->stride,
                                                  &memHandle);
            if (result != android::NO_ERROR || !memHandle) {
                LOG(WARNING) << "Failed to import a buffer " << b.bufferId;
                continue;
            }

            auto stored = false;
            for (auto&& rec : mBuffers) {
                if (rec.handle == nullptr) {
                    // Use this existing entry
                    rec.handle = memHandle;
                    rec.inUse = false;

                    stored = true;
                    break;
                }
            }

            if (!stored) {
                // Add a BufferRecord wrapping this handle to our set of available buffers
                mBuffers.emplace_back(memHandle);
            }

            ++mFramesAllowed;
        }

        _hidl_cb(EvsResult::OK, mFramesAllowed - before);
        return {};
    }
}

EvsResult EvsStreamingCamera::doneWithFrame_impl(const uint32_t bufferId, const buffer_handle_t memHandle) {
    LOG(DEBUG) << __FUNCTION__;
    std::lock_guard <std::mutex> lock(mAccessLock);

    if (memHandle == nullptr) {
        LOG(ERROR) << "Ignoring doneWithFrame called with null handle";
    } else if (bufferId >= mBuffers.size()) {
        LOG(ERROR) << "Ignoring doneWithFrame called with invalid bufferId " << bufferId
                    << " (max is " << mBuffers.size() - 1 << ")";
    } else if (!mBuffers[bufferId].inUse) {
        LOG(ERROR) << "Ignoring doneWithFrame called on frame " << bufferId
                    << " which is already free";
    } else {
        // Mark the frame as available
        mBuffers[bufferId].inUse = false;
        mFramesInUse--;

        // If this frame's index is high in the array, try to move it down
        // to improve locality after mFramesAllowed has been reduced.
        if (bufferId >= mFramesAllowed) {
            // Find an empty slot lower in the array (which should always exist in this case)
            for (auto&& rec : mBuffers) {
                if (rec.handle == nullptr) {
                    rec.handle = mBuffers[bufferId].handle;
                    mBuffers[bufferId].handle = nullptr;
                    break;
                }
            }
        }
    }

    return EvsResult::OK;
}

bool EvsStreamingCamera::setAvailableFrames_Locked(unsigned bufferCount) {
    LOG(DEBUG) << __FUNCTION__;

    if (bufferCount < 1) {
        LOG(ERROR) << "Ignoring request to set buffer count to zero";
        return false;
    }
    if (bufferCount > MAX_BUFFERS_IN_FLIGHT) {
        LOG(ERROR) << "Rejecting buffer request in excess of internal limit";
        return false;
    }

    // Is an increase required?
    if (mFramesAllowed < bufferCount) {
        // An increase is required
        unsigned needed = bufferCount - mFramesAllowed;
        LOG(INFO) << "Allocating " << needed << " buffers for camera frames";

        unsigned added = increaseAvailableFrames_Locked(needed);
        if (added != needed) {
            // If we didn't add all the frames we needed, then roll back to the previous state
            LOG(ERROR) << "Rolling back to previous frame queue size";
            decreaseAvailableFrames_Locked(added);
            return false;
        }
    } else if (mFramesAllowed > bufferCount) {
        // A decrease is required
        unsigned framesToRelease = mFramesAllowed - bufferCount;
        LOG(INFO) << "Returning " << framesToRelease << " camera frame buffers";

        unsigned released = decreaseAvailableFrames_Locked(framesToRelease);
        if (released != framesToRelease) {
            // This shouldn't happen with a properly behaving client because the client
            // should only make this call after returning sufficient outstanding buffers
            // to allow a clean resize.
            LOG(ERROR) << "Buffer queue shrink failed -- too many buffers currently in use?";
        }
    }

    return true;
}

unsigned EvsStreamingCamera::increaseAvailableFrames_Locked(unsigned numToAdd) {
    LOG(DEBUG) << __FUNCTION__;
    // Acquire the graphics buffer allocator
    GraphicBufferAllocator& alloc(GraphicBufferAllocator::get());

    unsigned added = 0;
    while (added < numToAdd) {
        unsigned pixelsPerLine = mStride;
        buffer_handle_t memHandle = nullptr;
        status_t result = alloc.allocate(mWidth, 
                                         mHeight,
                                         mFormat, 
                                         1 /*layerCount*/,
                                         mUsage,
                                         &memHandle, 
                                         &pixelsPerLine,
                                         "CarlaSim");
        if (result != NO_ERROR) {
            LOG(ERROR) << "Error " << result << " allocating " << mWidth << " x " << mHeight << " graphics buffer";
            break;
        }
        if (!memHandle) {
            LOG(ERROR) << "We didn't get a buffer handle back from the allocator";
            break;
        }        
        if (mStride) {
            if (mStride != pixelsPerLine) {
                LOG(ERROR) << "We did not expect to get buffers with different strides! " << pixelsPerLine;
            }
        } else {
            // Gralloc defines stride in terms of pixels per line
            mStride = pixelsPerLine;
        }

        // Find a place to store the new buffer
        bool stored = false;
        for (auto&& rec : mBuffers) {
            if (rec.handle == nullptr) {
                // Use this existing entry
                rec.handle = memHandle;
                rec.inUse = false;
                stored = true;
                break;
            }
        }
        if (!stored) {
            // Add a BufferRecord wrapping this handle to our set of available buffers
            mBuffers.emplace_back(memHandle);
        }

        mFramesAllowed++;
        added++;
    }

    return added;
}

unsigned EvsStreamingCamera::decreaseAvailableFrames_Locked(unsigned numToRemove) {
    LOG(DEBUG) << __FUNCTION__;
    // Acquire the graphics buffer allocator
    GraphicBufferAllocator &alloc(GraphicBufferAllocator::get());

    unsigned removed = 0;

    for (auto&& rec : mBuffers) {
        // Is this record not in use, but holding a buffer that we can free?
        if ((rec.inUse == false) && (rec.handle != nullptr)) {
            // Release buffer and update the record so we can recognize it as "empty"
            alloc.free(rec.handle);
            rec.handle = nullptr;

            mFramesAllowed--;
            removed++;

            if (removed == numToRemove) {
                break;
            }
        }
    }

    return removed;
}

using android::base::Result;
using android::base::Error;
Result<void> EvsStreamingCamera::startDumpFrames(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        return Error(BAD_VALUE) << "Cannot access " << path;
    } else if (!(info.st_mode & S_IFDIR)) {
        return Error(BAD_VALUE) << path << " is not a directory";
    }

    mDumpPath = path;
    mDumpFrame = true;

    return {};
}

Result<void> EvsStreamingCamera::stopDumpFrames() {
    if (!mDumpFrame) {
        return Error(INVALID_OPERATION) << "Device is not dumping frames";
    }

    mDumpFrame = false;
    return {};
}


} // namespace implementation
} // namespace V1_1
} // namespace evs
} // namespace automotive
} // namespace hardware
} // namespace android
