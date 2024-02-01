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

#include "bufferCopy.h"

#include <aidl/android/hardware/graphics/common/HardwareBufferDescription.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <android/hardware_buffer.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/SystemClock.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <cutils/properties.h>

namespace {

using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::aidl::android::hardware::graphics::common::HardwareBufferDescription;
using ::android::base::Error;
using ::android::base::Result;
using ::ndk::ScopedAStatus;
using easywsclient::WebSocket;

// Arbitrary limit on number of graphics buffers allowed to be allocated
// Safeguards against unreasonable resource consumption and provides a testable limit
static const unsigned MAX_BUFFERS_IN_FLIGHT = 100;

}  // namespace

namespace aidl::android::hardware::automotive::evs::implementation {

EvsStreamingCamera::EvsStreamingCamera(const char* deviceName, 
                                       std::unique_ptr<ConfigManager::CameraInfo>& camInfo,
                                       const Stream* requestedStreamCfg)
   : mFramesAllowed(0),
     mFramesInUse(0),
     mCameraInfo(camInfo),
     mUri(),
     mIsActive(false),
     ws(NULL) {

    (void)mCameraInfo;
    mFramesAllowed = 0;
    mFramesInUse = 0;
    mDescription.id = deviceName;

    if (camInfo) {
        uint8_t* ptr = reinterpret_cast<uint8_t*>(camInfo->characteristics);
        const size_t len = get_camera_metadata_size(camInfo->characteristics);
        mDescription.metadata.insert(mDescription.metadata.end(), ptr, ptr + len);
    }

    // Default output buffer format.
    // mFormat = HAL_PIXEL_FORMAT_BGRA_8888;
    mFormat = HAL_PIXEL_FORMAT_RGBA_8888;

    // How we expect to use the gralloc buffers we'll exchange with our client
    mUsage  = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_SW_READ_RARELY | GRALLOC_USAGE_SW_WRITE_OFTEN;

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
    if (camInfo != nullptr && requestedStreamCfg != nullptr) {
        LOG(INFO) << "Requested stream configuration:";
        LOG(INFO) << "  width = " << requestedStreamCfg->width;
        LOG(INFO) << "  height = " << requestedStreamCfg->height;
        LOG(INFO) << "  format = " << static_cast<int>(requestedStreamCfg->format);
        // Validate a given stream configuration.  If there is no exact match,
        // this will try to find the best match based on:
        // 1) same output format
        // 2) the largest resolution that is smaller that a given configuration.
        int32_t streamId = -1, area = INT_MIN;
        for (const auto& [id, cfg] : camInfo->streamConfigurations) {
            if (cfg.format == requestedStreamCfg->format) {
                if (cfg.width == requestedStreamCfg->width &&
                    cfg.height == requestedStreamCfg->height) {
                    // Find exact match.
                    streamId = id;
                    break;
                } else if (cfg.width < requestedStreamCfg->width &&
                           cfg.height < requestedStreamCfg->height &&
                           cfg.width * cfg.height > area) {
                    streamId = id;
                    area = cfg.width * cfg.height;
                }
            }
        }

        LOG(INFO) << "Avaialble stream configuration size = " << camInfo->streamConfigurations.size();
        if (camInfo->streamConfigurations.size() > 0) {
            // use 0 as default stream config
            mStride = static_cast<int32_t>(camInfo->streamConfigurations[0].width);
            mWidth = static_cast<int32_t>(camInfo->streamConfigurations[0].width);
            mHeight = static_cast<int32_t>(camInfo->streamConfigurations[0].height);
            mFormat = static_cast<int32_t>(camInfo->streamConfigurations[0].format);
        }
    
        LOG(INFO) << "Selected video stream configuration for " << mUri<< ":";
        LOG(INFO) << "  width = " << mWidth;
        LOG(INFO) << "  height = " << mHeight;
        LOG(INFO) << "  format = " << mFormat;
    }
}

EvsStreamingCamera::~EvsStreamingCamera() {
    LOG(DEBUG) << "EvsStreamingCamera being destroyed";

    shutdown();
}

std::shared_ptr<EvsStreamingCamera> EvsStreamingCamera::Create(const char* deviceName) {
    LOG(DEBUG) << __FUNCTION__;

    std::unique_ptr<ConfigManager::CameraInfo> nullCamInfo = nullptr;
    return Create(deviceName, nullCamInfo);
}

std::shared_ptr<EvsStreamingCamera> EvsStreamingCamera::Create(const char* deviceName,
                                      std::unique_ptr<ConfigManager::CameraInfo>& camInfo,
                                      const Stream* requestedStreamCfg) {
    LOG(INFO) << "Create " << deviceName;

    std::shared_ptr<EvsStreamingCamera> evsStreamingCamera =
       ndk::SharedRefBase::make<EvsStreamingCamera>(deviceName, camInfo, requestedStreamCfg);
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
        ::android::GraphicBufferAllocator& alloc(::android::GraphicBufferAllocator::get());
        for (auto&& rec : mBuffers) {
            if (rec.inUse) { LOG(WARNING) << "Releasing buffer despite remote ownership"; }
            alloc.free(rec.handle);
            rec.handle = nullptr;
        }
        mBuffers.clear();
    }
}

// Methods from ::aidl::android::hardware::automotive::evs::IEvsCamera follow.
ScopedAStatus EvsStreamingCamera::getCameraInfo(aidlevs::CameraDesc* _aidl_return) {
    LOG(DEBUG) << __FUNCTION__;

    // Send back our self description
    *_aidl_return = mDescription;
    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::setMaxFramesInFlight(int32_t bufferCount) {
    LOG(INFO) << __FUNCTION__ << " bufferCount " << bufferCount;
    std::lock_guard<std::mutex> lock(mAccessLock);
    
    // We cannot function without at least one video buffer to send data
    if (bufferCount < 1) {
        LOG(ERROR) << "Ignoring setMaxFramesInFlight with less than one buffer requested";
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::INVALID_ARG));
    }

    // Update our internal state
    if (setAvailableFrames_Locked(bufferCount)) {
        return ScopedAStatus::ok();
    } else {
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::BUFFER_NOT_AVAILABLE));
    }
}

ScopedAStatus EvsStreamingCamera::startVideoStream(const std::shared_ptr<IEvsCameraStream>& stream)  {
    LOG(INFO) << __FUNCTION__;
    std::lock_guard<std::mutex> lock(mAccessLock);

    // If the client never indicated otherwise, configure ourselves for a single streaming buffer
    if (mFramesAllowed < 1) {
        if (!setAvailableFrames_Locked(1)) {
            LOG(ERROR) << "Failed to start stream because we couldn't get a graphics buffer";
            return ScopedAStatus::fromServiceSpecificError(
                    static_cast<int>(EvsResult::BUFFER_NOT_AVAILABLE));
        }
    }

    if (mIsActive) {
        LOG(ERROR) << "Ignoring startVideoStream call when a stream is already running.";
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::STREAM_ALREADY_RUNNING));
    }

    if (stream == nullptr) {
        LOG(ERROR) << "Null stream";
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::STREAM_ALREADY_RUNNING));
    }

    if (mStream) {
        LOG(ERROR) << "Ignoring startVideoStream call when a stream is already running.";
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::STREAM_ALREADY_RUNNING));
    }
    // Record the user's callback for use when we have a frame ready
    mStream = stream;

    ws = WebSocket::from_url(mUri);
    if (ws == nullptr) {
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::BUFFER_NOT_AVAILABLE));
    }

    mIsActive = true;
    
    mThread = std::thread ([this]() {
        LOG(INFO) << "Start poll";
        while ((ws->getReadyState() != WebSocket::CLOSED) && (true == mIsActive)) {
            ws->poll();
            ws->dispatchBinary([this] (const std::vector<uint8_t>& v) { this->forwardFrame(v); });
        }
        ws->close();
        ws->poll();
        delete ws;
        LOG(INFO) << "Stop poll";
    });

    LOG(DEBUG) << "Result OK. Start thread " << mThread.get_id();
    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::stopVideoStream()  {
    LOG(INFO) << __FUNCTION__;

    mIsActive = false;
    if (mThread.joinable()) { mThread.join(); }

    if (mStream) {
        // V1.1 client is waiting on STREAM_STOPPED event.
        std::unique_lock <std::mutex> lock(mAccessLock);

        EvsEventDesc event;
        event.aType = EvsEventType::STREAM_STOPPED;
        auto result = mStream->notify(event);
        if (!result.isOk()) {
            LOG(ERROR) << "Error delivering end of stream event";
        }

        // Drop our reference to the client's stream receiver
        mStream     = nullptr;
    }

    return ScopedAStatus::ok();
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
            return;
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
    buffer_handle_t memHandle = mBuffers[idx].handle;
    BufferDesc bufferDesc = {
        .buffer = {
            .description = {
                .width = mWidth,
                .height = mHeight,
                .layers = 1,
                .format = static_cast<::aidl::android::hardware::graphics::common::PixelFormat>(mFormat),
                .usage = static_cast<::aidl::android::hardware::graphics::common::BufferUsage>(mUsage),
                .stride = mStride,
             },
            .handle = ::android::dupToAidl(memHandle),
         },
        .bufferId = static_cast<int32_t>(idx),
        .deviceId = mDescription.id,
        .timestamp = static_cast<int64_t>(::android::elapsedRealtimeNano() * 1e+3),
    };

    // Lock our output buffer for writing
    // TODO(b/145459970): Sometimes, physical camera device maps a buffer
    // into the address that is about to be unmapped by another device; this
    // causes SEGV_MAPPER.
    void* targetPixels = nullptr;
    ::android::GraphicBufferMapper& mapper = ::android::GraphicBufferMapper::get();
    ::android::status_t result = mapper.lock(memHandle, GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_NEVER,
                                  ::android::Rect(bufferDesc.buffer.description.width, 
                                                  bufferDesc.buffer.description.height),
                                  (void **)&targetPixels);

    // If we failed to lock the pixel buffer, we're about to crash, but log it first
    if (!targetPixels) {
        // TODO(b/145457727): When EvsHidlTest::CameraToDisplayRoundTrip
        // test case was repeatedly executed, EVS occasionally fails to map
        // a buffer.
        LOG(ERROR) << "Camera failed to gain access to image buffer for writing - "
                    << " status: " << ::android::statusToString(result)
                    << " , error: " << strerror(errno);
    }

    // HAL_PIXEL_FORMAT_RGBA_8888 default output format
    // Transfer the camera image into the output buffer
    int total = 0;
    const unsigned dstStride = bufferDesc.buffer.description.stride * 4/*bytesPerPixel*/;
    const unsigned numRows   = bufferDesc.buffer.description.height;
    uint8_t* dst = reinterpret_cast<uint8_t*>(targetPixels);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(v.data());
    for (unsigned r = 0; r < numRows; ++r) {
        memcpy(dst, src, dstStride);
        // Moves to the next row
        src += dstStride;
        dst += dstStride;
        total += dstStride;
    }
    LOG(DEBUG) << "Total copy " << total;

    // Unlock the output buffer
    mapper.unlock(memHandle);

    // Issue the (asynchronous) callback to the client -- can't be holding
    // the lock
    bool flag = false;
    if (mStream) {
        std::vector<BufferDesc> frames;
        frames.push_back(std::move(bufferDesc));
        flag = mStream->deliverFrame(frames).isOk();
    }

    if (flag) {
        LOG(DEBUG) << "Delivered " << memHandle << " as id " << bufferDesc.bufferId;
    } else {
        // This can happen if the client dies and is likely unrecoverable.
        // To avoid consuming resources generating failing calls, we stop sending
        // frames.  Note, however, that the stream remains in the "STREAMING" state
        // until cleaned up on the main thread.
        LOG(ERROR) << "Frame delivery call failed in the transport layer.";

        // Since we didn't actually deliver it, mark the frame as available
        std::lock_guard<std::mutex> lock(mAccessLock);
        mBuffers[idx].inUse = false;
        --mFramesInUse;
    }

    if (mDumpFrame) {
        // Construct a target filename with the device identifier
        std::string filename = std::string("EvsCarla");
        std::replace(filename.begin(), filename.end(), '/', '_');
        filename = mDumpPath + filename + "_" + std::to_string(mFrameCounter) + ".bin";

        ::android::base::unique_fd fd(
            open(filename.data(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP));
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

ScopedAStatus EvsStreamingCamera::getPhysicalCameraInfo([[maybe_unused]] const std::string& id,
                                                        CameraDesc* _aidl_return) {
    LOG(DEBUG) << __FUNCTION__;

    // This method works exactly same as getCameraInfo_1_1() in EVS HW module.
    *_aidl_return = mDescription;
    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::doneWithFrame(const std::vector<BufferDesc>& buffers)  {
    LOG(DEBUG) << __FUNCTION__;

    for (auto&& buffer : buffers) {
        doneWithFrame_impl(buffer);
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::pauseVideoStream() {
    return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::NOT_SUPPORTED));
}

ScopedAStatus EvsStreamingCamera::resumeVideoStream() {
    return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::NOT_SUPPORTED));
}

ScopedAStatus EvsStreamingCamera::setPrimaryClient() {
    /* Because EVS HW module reference implementation expects a single client at
     * a time, this returns a success code always.
     */
    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::forcePrimaryClient(const std::shared_ptr<IEvsDisplay>&) {
    /* Because EVS HW module reference implementation expects a single client at
     * a time, this returns a success code always.
     */
    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::unsetPrimaryClient() {
    /* Because EVS HW module reference implementation expects a single client at
     * a time, there is no chance that this is called by the secondary client and
     * therefore returns a success code always.
     */
    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::getParameterList(std::vector<CameraParam>* _aidl_return) {
    LOG(DEBUG) << __FUNCTION__;
    if (mCameraInfo) {
        _aidl_return->resize(mCameraInfo->controls.size());
        auto idx = 0;
        for (auto& [name, range] : mCameraInfo->controls) {
            (*_aidl_return)[idx++] = name;
        }
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::getIntParameterRange(CameraParam id, ParameterRange* _aidl_return) {
    LOG(DEBUG) << __FUNCTION__;
    if (!mCameraInfo) {
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::NOT_SUPPORTED));
    }

    auto it = mCameraInfo->controls.find(id);
    if (it == mCameraInfo->controls.end()) {
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::NOT_SUPPORTED));
    }

    _aidl_return->min = std::get<0>(it->second);
    _aidl_return->max = std::get<1>(it->second);
    _aidl_return->step = std::get<2>(it->second);

    return ScopedAStatus::ok();

}

ScopedAStatus EvsStreamingCamera::setIntParameter(CameraParam id, int32_t value,
                                                  std::vector<int32_t>* effectiveValue) {
    LOG(DEBUG) << __FUNCTION__;

    mIntParams.insert_or_assign(static_cast<uint32_t>(id), value);
    (*effectiveValue)[0] = value;

    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::getIntParameter(CameraParam id, std::vector<int32_t>* value) {
    LOG(DEBUG) << __FUNCTION__;

    if (auto search = mIntParams.find(static_cast<uint32_t>(id)); search != mIntParams.end()) {
        (*value)[0] = search->second;
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::setExtendedInfo(int32_t opaqueIdentifier,
                                                  const std::vector<uint8_t>& opaqueValue) {
    mExtInfo.insert_or_assign(opaqueIdentifier, opaqueValue);
    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::getExtendedInfo(int32_t opaqueIdentifier,
                                                  std::vector<uint8_t>* opaqueValue) {
    LOG(DEBUG) << __FUNCTION__;
    const auto it = mExtInfo.find(opaqueIdentifier);
    if (it == mExtInfo.end()) {
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::INVALID_ARG));
    } else {
        *opaqueValue = mExtInfo[opaqueIdentifier];
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsStreamingCamera::importExternalBuffers(const std::vector<BufferDesc>& buffers,
                                                        int32_t* _aidl_return) {
    LOG(DEBUG) << __FUNCTION__;

    size_t numBuffersToAdd = buffers.size();
    if (numBuffersToAdd < 1) {
        LOG(DEBUG) << "No buffers to add.";
        *_aidl_return = 0;
        return ScopedAStatus::ok();
    }

    {
        std::scoped_lock<std::mutex> lock(mAccessLock);

        if (numBuffersToAdd > (MAX_BUFFERS_IN_FLIGHT - mFramesAllowed)) {
            numBuffersToAdd -= (MAX_BUFFERS_IN_FLIGHT - mFramesAllowed);
            LOG(WARNING) << "Exceed the limit on number of buffers.  "
                         << numBuffersToAdd << " buffers will be added only.";
        }

        ::android::GraphicBufferMapper& mapper = ::android::GraphicBufferMapper::get();
        const auto before = mFramesAllowed;
        for (size_t i = 0; i < numBuffersToAdd; ++i) {
            // TODO: reject if external buffer is configured differently.
            auto& b = buffers[i];
            const HardwareBufferDescription& description = b.buffer.description;

            // Import a buffer to add
            buffer_handle_t memHandle = nullptr;
            ::android::status_t result = mapper.importBuffer(::android::dupFromAidl(b.buffer.handle),
                                                  description.width,
                                                  description.height,
                                                  1,
                                                  static_cast<::android::PixelFormat>(description.format),
                                                  static_cast<uint64_t>(description.usage),
                                                  description.stride,
                                                  &memHandle);
            if (result != ::android::NO_ERROR || memHandle == nullptr) {
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

        *_aidl_return = mFramesAllowed - before;
        return ScopedAStatus::ok();
    }
}

EvsResult EvsStreamingCamera::doneWithFrame_impl(const BufferDesc& bufferDesc) {
    LOG(DEBUG) << __FUNCTION__;
    if (static_cast<uint32_t>(bufferDesc.bufferId) >= mBuffers.size()) {
        LOG(WARNING) << "Ignoring doneWithFrame called with invalid id " << bufferDesc.bufferId
                     << " (max is " << mBuffers.size() - 1 << ")";
        return EvsResult::OK;
    }

    // Mark this buffer as available
    {
        std::lock_guard<std::mutex> lock(mAccessLock);
        mBuffers[bufferDesc.bufferId].inUse = false;
        --mFramesInUse;

        // If this frame's index is high in the array, try to move it down
        // to improve locality after mFramesAllowed has been reduced.
        if (static_cast<uint32_t>(bufferDesc.bufferId) >= mFramesAllowed) {
            // Find an empty slot lower in the array (which should always exist in this case)
            bool found = false;
            for (auto&& rec : mBuffers) {
                if (!rec.handle) {
                    rec.handle = mBuffers[bufferDesc.bufferId].handle;
                    mBuffers[bufferDesc.bufferId].handle = nullptr;
                    found = true;
                    break;
                }
            }

            if (!found) {
                LOG(WARNING) << "No empty slot!";
            }
        }
    }

    return EvsResult::OK;
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
            bool found = false;
            for (auto&& rec : mBuffers) {
                if (!rec.handle) {
                    rec.handle = mBuffers[bufferId].handle;
                    mBuffers[bufferId].handle = nullptr;
                    found = true;
                    break;
                }
            }

            if (!found) {
                LOG(WARNING) << "No empty slot!";
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
        auto needed = bufferCount - mFramesAllowed;
        LOG(INFO) << "Allocating " << needed << " buffers for camera frames";

        auto added = increaseAvailableFrames_Locked(needed);
        if (added != needed) {
            // If we didn't add all the frames we needed, then roll back to the previous state
            LOG(ERROR) << "Rolling back to previous frame queue size";
            decreaseAvailableFrames_Locked(added);
            return false;
        }
    } else if (mFramesAllowed > bufferCount) {
        // A decrease is required
        auto framesToRelease = mFramesAllowed - bufferCount;
        LOG(INFO) << "Returning " << framesToRelease << " camera frame buffers";

        auto released = decreaseAvailableFrames_Locked(framesToRelease);
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
    ::android::GraphicBufferAllocator& alloc(::android::GraphicBufferAllocator::get());

    unsigned added = 0;
    while (added < numToAdd) {
        uint32_t pixelsPerLine = mStride;
        buffer_handle_t memHandle = nullptr;
        ::android::status_t result = alloc.allocate(mWidth, 
                                         mHeight,
                                         mFormat, 
                                         1 /*layerCount*/,
                                         mUsage,
                                         &memHandle, 
                                         &pixelsPerLine,
                                         0,
                                         "CarlaSim");
        if (result != ::android::NO_ERROR) {
            LOG(ERROR) << "Error " << result << " allocating " << mWidth << " x " << mHeight << " graphics buffer";
            break;
        }
        if (memHandle == nullptr) {
            LOG(ERROR) << "We didn't get a buffer handle back from the allocator";
            break;
        }        
        if (mStride) {
            if (mStride != static_cast<int32_t>(pixelsPerLine)) {
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

        ++mFramesAllowed;
        ++added;
    }

    return added;
}

unsigned EvsStreamingCamera::decreaseAvailableFrames_Locked(unsigned numToRemove) {
    LOG(DEBUG) << __FUNCTION__;
    // Acquire the graphics buffer allocator
    ::android::GraphicBufferAllocator &alloc(::android::GraphicBufferAllocator::get());

    unsigned removed = 0;
    for (auto&& rec : mBuffers) {
        // Is this record not in use, but holding a buffer that we can free?
        if ((rec.inUse == false) && (rec.handle != nullptr)) {
            // Release buffer and update the record so we can recognize it as "empty"
            alloc.free(rec.handle);
            rec.handle = nullptr;

            --mFramesAllowed;
            ++removed;

            if (removed == numToRemove) {
                break;
            }
        }
    }

    return removed;
}

Result<void> EvsStreamingCamera::startDumpFrames(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        return Error(::android::BAD_VALUE) << "Cannot access " << path;
    } else if (!(info.st_mode & S_IFDIR)) {
        return Error(::android::BAD_VALUE) << path << " is not a directory";
    }

    mDumpPath = path;
    mDumpFrame = true;

    return {};
}

Result<void> EvsStreamingCamera::stopDumpFrames() {
    if (!mDumpFrame) {
        return Error(::android::INVALID_OPERATION) << "Device is not dumping frames";
    }

    mDumpFrame = false;
    return {};
}

}  // namespace aidl::android::hardware::automotive::evs::implementation
