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

//#define LOG_TAG "EvsV4lCameraZeroCopy"

#include "EvsV4lCameraZeroCopy.h"

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
#include <chrono>

namespace {

using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::aidl::android::hardware::graphics::common::HardwareBufferDescription;
using ::android::base::Error;
using ::android::base::Result;
using ::ndk::ScopedAStatus;

// Arbitrary limit on number of graphics buffers allowed to be allocated
// Safeguards against unreasonable resource consumption and provides a testable limit
constexpr unsigned kMaxBuffersInFlight = 6;

}  // namespace

namespace aidl::android::hardware::automotive::evs::implementation {

EvsV4lCameraZeroCopy::EvsV4lCameraZeroCopy(const char* deviceName,
                           std::unique_ptr<ConfigManager::CameraInfo>& camInfo) :
      mFramesAllowed(0), mFramesInUse(0), mCameraInfo(camInfo) {
    LOG(DEBUG) << "EvsV4lCameraZeroCopy instantiated";

    mDescription.id = deviceName;
    if (camInfo) {
        uint8_t* ptr = reinterpret_cast<uint8_t*>(camInfo->characteristics);
        const size_t len = get_camera_metadata_size(camInfo->characteristics);
        mDescription.metadata.insert(mDescription.metadata.end(), ptr, ptr + len);
    }

    // Default output buffer format.
    mFormat = mVideo.getDefaultFormat();

    // How we expect to use the gralloc buffers we'll exchange with our client
    mUsage = VideoCaptureZeroCopy::getDefaultUsage();
}

EvsV4lCameraZeroCopy::~EvsV4lCameraZeroCopy() {
    LOG(DEBUG) << "EvsV4lCameraZeroCopy being destroyed";
    shutdown();
}

// This gets called if another caller "steals" ownership of the camera
void EvsV4lCameraZeroCopy::shutdown() {
    LOG(DEBUG) << "EvsV4lCameraZeroCopy shutdown";

    // Make sure our output stream is cleaned up
    // (It really should be already)
    stopVideoStream();

    // Note:  Since stopVideoStream is blocking, no other threads can now be running

    // Close our video capture device
    mVideo.close();

    // Drop all the graphics buffers we've been using
    if (mBuffers.size() > 0) {
        ::android::GraphicBufferAllocator& alloc(::android::GraphicBufferAllocator::get());
        ::android::GraphicBufferMapper &mapper = ::android::GraphicBufferMapper::get();
        for (auto&& rec : mBuffers) {
            if (rec.handle == nullptr) {
                LOG(WARNING) << "Skipping release of null buffer handle";
                continue;
            }
            if (rec.inUse) {
                LOG(WARNING) << "Releasing buffer despite remote ownership";
            }
            auto result = mapper.unlock(rec.handle);
            if (result != ::android::OK)
            {
                LOG(ERROR) << "Failed to unlock buffer " << rec.handle << ": "
                           << ::android::statusToString(result);
            }
            alloc.free(rec.handle);
            rec.handle = nullptr;
        }
        mBuffers.clear();
    }
}

// Methods from ::aidl::android::hardware::automotive::evs::IEvsCamera follow.
ScopedAStatus EvsV4lCameraZeroCopy::getCameraInfo(CameraDesc* _aidl_return) {
    LOG(INFO) << __FUNCTION__;

    // Send back our self description
    *_aidl_return = mDescription;
    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::setMaxFramesInFlight(int32_t bufferCount) {
    LOG(DEBUG) << __FUNCTION__ << " with bufferCount=" << bufferCount;
    std::lock_guard<std::mutex> lock(mAccessLock);

    // If we've been displaced by another owner of the camera, then we can't do anything else
    if (!mVideo.isOpen()) {
        LOG(WARNING) << "Ignoring setMaxFramesInFlight call when camera has been lost.";
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::OWNERSHIP_LOST));
    }

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

ScopedAStatus EvsV4lCameraZeroCopy::startVideoStream(const std::shared_ptr<IEvsCameraStream>& client) {
    LOG(DEBUG) << __FUNCTION__;
    std::lock_guard<std::mutex> lock(mAccessLock);

    // If we've been displaced by another owner of the camera, then we can't do anything else
    if (!mVideo.isOpen()) {
        LOG(WARNING) << "Ignoring startVideoStream call when camera has been lost.";
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::OWNERSHIP_LOST));
    }

    if (mStream) {
        LOG(ERROR) << "Ignoring startVideoStream call when a stream is already running.";
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::STREAM_ALREADY_RUNNING));
    }

    // If the client never indicated otherwise, configure ourselves for a single streaming buffer
    if (mFramesAllowed < 1) {
        if (!setAvailableFrames_Locked(mVideo.getDefaultBufferCount())) {
            LOG(ERROR) << "Failed to start stream because we couldn't get a graphics buffer";
            return ScopedAStatus::fromServiceSpecificError(
                    static_cast<int>(EvsResult::BUFFER_NOT_AVAILABLE));
        }
    }

    // Choose which image transfer function we need
    // Map from V4L2 to Android graphic buffer format
    const auto videoSrcFormat = mVideo.getV4LFormat();
    LOG(INFO) << "Configuring to accept " << std::string((char*)&videoSrcFormat)
              << " camera data and convert to " << std::hex << mFormat;

    // Record the user's callback for use when we have a frame ready
    mStream = client;

    mVideo.registerBuffers(mBuffers);

    // Set up the video stream with a callback to our member function forwardFrame()
    if (!mVideo.startStream([this](VideoCaptureZeroCopy*, imageBuffer* tgt, void* data) {
            this->forwardFrame(tgt, data);
        })) {
        // No need to hold onto this if we failed to start
        mStream = nullptr;
        LOG(ERROR) << "Underlying camera start stream failed";
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::UNDERLYING_SERVICE_ERROR));
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::stopVideoStream() {
    LOG(DEBUG) << __FUNCTION__;

    // Tell the capture device to stop (and block until it does)
    mVideo.stopStream();
    if (mStream) {
        std::unique_lock<std::mutex> lock(mAccessLock);

        EvsEventDesc event;
        event.aType = EvsEventType::STREAM_STOPPED;
        auto result = mStream->notify(event);
        if (!result.isOk()) {
            LOG(WARNING) << "Error delivering end of stream event";
        }

        // Drop our reference to the client's stream receiver
        mStream = nullptr;
    }
    // Drop all the graphics buffers we've been using
    std::unique_lock<std::mutex> lock(mAccessLock);
    if (mBuffers.size() > 0) {
        ::android::GraphicBufferAllocator& alloc(::android::GraphicBufferAllocator::get());
        ::android::GraphicBufferMapper &mapper = ::android::GraphicBufferMapper::get();
        for (auto&& rec : mBuffers) {
            if (rec.handle == nullptr) {
                LOG(WARNING) << "Skipping release of null buffer handle";
                continue;
            }
            LOG(DEBUG) << "Releasing buffer with handle " << rec.handle;
            if (rec.inUse) {
                LOG(WARNING) << "Can't release buffer due to remote ownership";
                continue;
            }
            auto result = mapper.unlock(rec.handle);
            if (result != ::android::OK)
            {
                LOG(ERROR) << "Failed to unlock buffer " << rec.handle << ": "
                           << ::android::statusToString(result);
            }
            alloc.free(rec.handle);
            rec.handle = nullptr;
        }

    } else {
        LOG(DEBUG) << "No buffers to release";
    }

    mFramesAllowed = 0;

    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::getPhysicalCameraInfo([[maybe_unused]] const std::string& id,
                                                  CameraDesc* _aidl_return) {
    LOG(INFO) << __FUNCTION__;

    // This method works exactly same as getCameraInfo_1_1() in EVS HW module.
    *_aidl_return = mDescription;
    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::doneWithFrame(const std::vector<BufferDesc>& buffers) {
    LOG(INFO) << __FUNCTION__;

    for (const auto& buffer : buffers) {
        doneWithFrame_impl(buffer);
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::pauseVideoStream() {
    return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::NOT_SUPPORTED));
}

ScopedAStatus EvsV4lCameraZeroCopy::resumeVideoStream() {
    return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::NOT_SUPPORTED));
}

ScopedAStatus EvsV4lCameraZeroCopy::setPrimaryClient() {
    /* Because EVS HW module reference implementation expects a single client at
     * a time, this returns a success code always.
     */
    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::forcePrimaryClient(const std::shared_ptr<IEvsDisplay>&) {
    /* Because EVS HW module reference implementation expects a single client at
     * a time, this returns a success code always.
     */
    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::unsetPrimaryClient() {
    /* Because EVS HW module reference implementation expects a single client at
     * a time, there is no chance that this is called by the secondary client and
     * therefore returns a success code always.
     */
    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::getParameterList(std::vector<CameraParam>* _aidl_return) {
    if (mCameraInfo) {
        _aidl_return->resize(mCameraInfo->controls.size());
        auto idx = 0;
        for (auto& [name, range] : mCameraInfo->controls) {
            (*_aidl_return)[idx++] = name;
        }
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::getIntParameterRange(CameraParam id, ParameterRange* _aidl_return) {
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

ScopedAStatus EvsV4lCameraZeroCopy::setIntParameter(CameraParam id, int32_t value,
                                            std::vector<int32_t>* effectiveValue) {
    uint32_t v4l2cid = V4L2_CID_BASE;
    if (!convertToV4l2CID(id, v4l2cid)) {
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::INVALID_ARG));
    } else {
        v4l2_control control = {v4l2cid, value};
        if (mVideo.setParameter(control) < 0 || mVideo.getParameter(control) < 0) {
            return ScopedAStatus::fromServiceSpecificError(
                    static_cast<int>(EvsResult::UNDERLYING_SERVICE_ERROR));
        }

        (*effectiveValue)[0] = control.value;
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::getIntParameter(CameraParam id, std::vector<int32_t>* value) {
    uint32_t v4l2cid = V4L2_CID_BASE;
    if (!convertToV4l2CID(id, v4l2cid)) {
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::INVALID_ARG));
    } else {
        v4l2_control control = {v4l2cid, 0};
        if (mVideo.getParameter(control) < 0) {
            return ScopedAStatus::fromServiceSpecificError(
                    static_cast<int>(EvsResult::INVALID_ARG));
        }

        // Report a result
        (*value)[0] = control.value;
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::setExtendedInfo(int32_t opaqueIdentifier,
                                            const std::vector<uint8_t>& opaqueValue) {
    mExtInfo.insert_or_assign(opaqueIdentifier, opaqueValue);
    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::getExtendedInfo(int32_t opaqueIdentifier,
                                            std::vector<uint8_t>* opaqueValue) {
    const auto it = mExtInfo.find(opaqueIdentifier);
    if (it == mExtInfo.end()) {
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::INVALID_ARG));
    } else {
        *opaqueValue = mExtInfo[opaqueIdentifier];
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsV4lCameraZeroCopy::importExternalBuffers(const std::vector<BufferDesc>& buffers,
                                                  int32_t* _aidl_return) {
    LOG(INFO) << __FUNCTION__;

    // If we've been displaced by another owner of the camera, then we can't do anything else
    if (!mVideo.isOpen()) {
        LOG(WARNING) << "Ignoring a request add external buffers "
                     << "when camera has been lost.";
        *_aidl_return = 0;
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::OWNERSHIP_LOST));
    }

    size_t numBuffersToAdd = buffers.size();
    if (numBuffersToAdd < 1) {
        LOG(INFO) << "No buffers to add.";
        *_aidl_return = 0;
        return ScopedAStatus::ok();
    }

    {
        std::lock_guard<std::mutex> lock(mAccessLock);
        ::android::GraphicBufferAllocator& alloc(::android::GraphicBufferAllocator::get());
        for (auto&& rec : buffers) {
            alloc.free(::android::makeFromAidl(rec.buffer.handle));
        }
        *_aidl_return = increaseAvailableFrames_Locked(numBuffersToAdd);
        return ScopedAStatus::ok();
    }
}

EvsResult EvsV4lCameraZeroCopy::doneWithFrame_impl(const BufferDesc& bufferDesc) {
    LOG(VERBOSE) << __FUNCTION__;
    if (!mVideo.isOpen()) {
        LOG(WARNING) << "Ignoring doneWithFrame call when camera has been lost.";
        return EvsResult::OK;
    }

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
        LOG(VERBOSE) << "Buffer id=" << bufferDesc.bufferId <<" is available";
        LOG(VERBOSE) << "mFramesInUse " << mFramesInUse;

        // If this frame's index is high in the array, try to move it down
        // to improve locality after mFramesAllowed has been reduced.
        if (static_cast<uint32_t>(bufferDesc.bufferId) >= mFramesAllowed) {
            // Find an empty slot lower in the array (which should always exist in this case)
            LOG(ERROR) << "Invalid buffer id =" << bufferDesc.bufferId;
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
     mVideo.markFrameConsumed(bufferDesc.bufferId);
    return EvsResult::OK;
}

EvsResult EvsV4lCameraZeroCopy::doneWithFrame_impl(uint32_t bufferId, buffer_handle_t handle) {
    {
    std::lock_guard<std::mutex> lock(mAccessLock);

    // If we've been displaced by another owner of the camera, then we can't do anything else
    if (!mVideo.isOpen()) {
        LOG(WARNING) << "Ignoring doneWithFrame call when camera has been lost.";
        return EvsResult::OK;
    }

    if (handle == nullptr) {
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
        --mFramesInUse;
        LOG(VERBOSE) << "Buffer id=" << bufferId <<" is available";
        LOG(VERBOSE) << "mFramesInUse " << mFramesInUse;
        // If this frame's index is high in the array, try to move it down
        // to improve locality after mFramesAllowed has been reduced.
        if (bufferId >= mFramesAllowed) {
            LOG(ERROR) << "Invalid buffer id =" << bufferId;
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
    }
    mVideo.markFrameConsumed(bufferId);

    return EvsResult::OK;
}

bool EvsV4lCameraZeroCopy::setAvailableFrames_Locked(unsigned bufferCount) {
    if (bufferCount < 1) {
        LOG(ERROR) << "Ignoring request to set buffer count to zero";
        return false;
    }
    if (bufferCount > kMaxBuffersInFlight) {
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

unsigned EvsV4lCameraZeroCopy::increaseAvailableFrames_Locked(unsigned numToAdd) {
    // Acquire the graphics buffer allocator
    ::android::GraphicBufferAllocator& alloc(::android::GraphicBufferAllocator::get());

    unsigned added = 0;
    //if (!mZeroCopy) {
    while (added < numToAdd) {
        unsigned pixelsPerLine = 0;
        buffer_handle_t memHandle = nullptr;
        auto result = alloc.allocate(mVideo.getWidth(), mVideo.getHeight(), mFormat, 1, mUsage,
                                     &memHandle, &pixelsPerLine, 0, "EvsV4lCameraZeroCopy");
        if (result != ::android::NO_ERROR) {
            LOG(ERROR) << "Error " << result << " allocating " << mVideo.getWidth() << " x "
                       << mVideo.getHeight() << " graphics buffer";
            ::android::GraphicBufferAllocator &alloc(::android::GraphicBufferAllocator::get());
            std::string str;

            alloc.dump(str, false);
            LOG(ERROR) << str;
            break;
        }
        if (memHandle == nullptr) {
            LOG(ERROR) << "We didn't get a buffer handle back from the allocator";
            break;
        }
        if (mStride > 0) {
            if (mStride != pixelsPerLine) {
                LOG(ERROR) << "We did not expect to get buffers with different strides!";
            }
        } else {
            // Gralloc defines stride in terms of pixels per line
            mStride = pixelsPerLine;
        }

        // Find a place to store the new buffer
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
            mBuffers.push_back(std::move(BufferRecord(memHandle)));
        }

        ++mFramesAllowed;
        ++added;
    }
    //}

    return added;
}

static void print_fps_every_3sec(uint64_t frame_cnt) {
    using namespace std::chrono;

    thread_local uint64_t last_cnt = 0;
    thread_local auto last_time = steady_clock::now();

    auto now = steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_time).count();

    if (elapsed >= 1) {
        uint64_t frames = frame_cnt - last_cnt;
        double fps = static_cast<double>(frames) / elapsed;

        LOG(VERBOSE) << "FPS:"<<fps <<" " << frames <<" frames in "<<elapsed << " sec";

        last_cnt = frame_cnt;
        last_time = now;
    }
}

unsigned EvsV4lCameraZeroCopy::decreaseAvailableFrames_Locked(unsigned numToRemove) {
    // Acquire the graphics buffer allocator
    ::android::GraphicBufferAllocator& alloc(::android::GraphicBufferAllocator::get());
    ::android::GraphicBufferMapper &mapper = ::android::GraphicBufferMapper::get();

    unsigned removed = 0;

    for (auto&& rec : mBuffers) {
        // Is this record not in use, but holding a buffer that we can free?
        if ( (rec.handle != nullptr)) {
            // Release buffer and update the record so we can recognize it as "empty"
            if ((rec.inUse == false) && (mStream != nullptr)) {
                continue;
            }
            auto result = mapper.unlock(rec.handle);
            if (result != ::android::OK)
            {
                LOG(ERROR) << "Failed to unlock buffer " << rec.handle << ": "
                           << ::android::statusToString(result);
            }
            alloc.free(rec.handle);
            rec.handle = nullptr;
            rec.inUse = false;

            --mFramesAllowed;
            ++removed;

            if (removed == numToRemove) {
                break;
            }
        }
    }
     LOG(INFO) <<   "Removed " << removed << " buffers, "
              << mFramesAllowed << " remaining";
    return removed;
}

// This is the async callback from the video camera that tells us a frame is ready
void EvsV4lCameraZeroCopy::forwardFrame(imageBuffer* pV4lBuff, void* pData) {
    LOG(VERBOSE) << __FUNCTION__;
    bool readyForFrame = false;
    unsigned idx = pV4lBuff->index;

    // Lock scope for updating shared state
    {
        std::lock_guard<std::mutex> lock(mAccessLock);

        // Are we allowed to issue another buffer?
        if (mFramesInUse >= mFramesAllowed) {
            // Can't do anything right now -- skip this frame
            LOG(WARNING) << "Skipped a frame because too many are in flight";
        } else {
            if (idx >= mBuffers.size()) {
                // This shouldn't happen since we already checked mFramesInUse vs mFramesAllowed
                LOG(ERROR) << "Failed to find an available buffer slot";
            } else {
                // We're going to make the frame busy
                mBuffers[idx].inUse = true;
                mFramesInUse++;
                readyForFrame = true;
                LOG(DEBUG) << "Buffer id=" << idx << " is in use, "
                          << mFramesInUse << " frames in use";
            }
        }
    }

    if (mDumpFrame) {
        // Construct a target filename with the device identifier
        std::string filename = std::string(mDescription.id);
        std::replace(filename.begin(), filename.end(), '/', '_');
        filename = mDumpPath + filename + "_" + std::to_string(mFrameCounter) + ".bin";

        ::android::base::unique_fd fd(
                open(filename.data(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP));
        LOG(ERROR) << filename << ", " << fd;
        if (fd == -1) {
            PLOG(ERROR) << "Failed to open a file, " << filename;
        } else {
            auto width = mVideo.getWidth();
            auto height = mVideo.getHeight();
            auto len = write(fd.get(), &width, sizeof(width));
            len += write(fd.get(), &height, sizeof(height));
            len += write(fd.get(), &mStride, sizeof(mStride));
            len += write(fd.get(), &mFormat, sizeof(mFormat));
            len += write(fd.get(), pData, pV4lBuff->length);
            LOG(INFO) << len << " bytes are written to " << filename;
        }
    }

    if (!readyForFrame) {
        // We need to return the video buffer so it can capture a new frame
        LOG(DEBUG) << "Not ready for frame .....";

        mVideo.markFrameConsumed(pV4lBuff->index);
    } else {
      
        using AidlPixelFormat = ::aidl::android::hardware::graphics::common::PixelFormat;

        // Assemble the buffer description we'll transmit below
        buffer_handle_t memHandle = mBuffers[idx].handle;
        BufferDesc bufferDesc = {
                .buffer =
                        {
                                .description =
                                        {
                                                .width = static_cast<int32_t>(mVideo.getWidth()),
                                                .height = static_cast<int32_t>(mVideo.getHeight()),
                                                .layers = 1,
                                                .format = static_cast<AidlPixelFormat>(mVideo.getDefaultFormat()),
                                                .usage = static_cast<BufferUsage>(mUsage),
                                                .stride = static_cast<int32_t>(mVideo.getStride()),
                                        },
                                .handle = ::android::dupToAidl(memHandle),
                        },
                .bufferId = static_cast<int32_t>(idx),
                .deviceId = mDescription.id,
                .timestamp = static_cast<int64_t>(::android::elapsedRealtimeNano() * 1e+3),
        };

        // Issue the (asynchronous) callback to the client -- can't be holding
        // the lock
        auto flag = false;
        if (mStream) {
            std::vector<BufferDesc> frames;
            frames.push_back(std::move(bufferDesc));
            flag = mStream->deliverFrame(frames).isOk();
        }

        if (flag) {
            LOG(INFO) << "Delivered " << memHandle << " as id " << bufferDesc.bufferId;
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
            mVideo.markFrameConsumed(pV4lBuff->index);
        }
    }

    // Increse a frame counter
    print_fps_every_3sec(++mFrameCounter);
}

bool EvsV4lCameraZeroCopy::convertToV4l2CID(CameraParam id, uint32_t& v4l2cid) {
    switch (id) {
        case CameraParam::BRIGHTNESS:
            v4l2cid = V4L2_CID_BRIGHTNESS;
            break;
        case CameraParam::CONTRAST:
            v4l2cid = V4L2_CID_CONTRAST;
            break;
        case CameraParam::AUTO_WHITE_BALANCE:
            v4l2cid = V4L2_CID_AUTO_WHITE_BALANCE;
            break;
        case CameraParam::WHITE_BALANCE_TEMPERATURE:
            v4l2cid = V4L2_CID_WHITE_BALANCE_TEMPERATURE;
            break;
        case CameraParam::SHARPNESS:
            v4l2cid = V4L2_CID_SHARPNESS;
            break;
        case CameraParam::AUTO_EXPOSURE:
            v4l2cid = V4L2_CID_EXPOSURE_AUTO;
            break;
        case CameraParam::ABSOLUTE_EXPOSURE:
            v4l2cid = V4L2_CID_EXPOSURE_ABSOLUTE;
            break;
        case CameraParam::AUTO_FOCUS:
            v4l2cid = V4L2_CID_FOCUS_AUTO;
            break;
        case CameraParam::ABSOLUTE_FOCUS:
            v4l2cid = V4L2_CID_FOCUS_ABSOLUTE;
            break;
        case CameraParam::ABSOLUTE_ZOOM:
            v4l2cid = V4L2_CID_ZOOM_ABSOLUTE;
            break;
        default:
            LOG(ERROR) << "Camera parameter " << static_cast<unsigned>(id) << " is unknown.";
            return false;
    }

    return mCameraControls.find(v4l2cid) != mCameraControls.end();
}

std::shared_ptr<EvsV4lCameraZeroCopy> EvsV4lCameraZeroCopy::Create(const char* deviceName) {
    std::unique_ptr<ConfigManager::CameraInfo> nullCamInfo = nullptr;
    return Create(deviceName, nullCamInfo);
}

std::shared_ptr<EvsV4lCameraZeroCopy> EvsV4lCameraZeroCopy::Create(
        const char* deviceName, std::unique_ptr<ConfigManager::CameraInfo>& camInfo,
        const Stream* requestedStreamCfg) {
    LOG(INFO) << "Create " << deviceName;
    std::shared_ptr<EvsV4lCameraZeroCopy> evsCamera =
            ndk::SharedRefBase::make<EvsV4lCameraZeroCopy>(deviceName, camInfo);
    if (!evsCamera) {
        return nullptr;
    }

    // Initialize the video device
    bool success = false;
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
        for (auto& [id, cfg] : camInfo->streamConfigurations) {
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

        if (streamId >= 0) {
            LOG(INFO) << "Selected video stream configuration:";
            LOG(INFO) << "  width = " << camInfo->streamConfigurations[streamId].width;
            LOG(INFO) << "  height = " << camInfo->streamConfigurations[streamId].height;
            LOG(INFO) << "  format = "
                      << static_cast<int>(camInfo->streamConfigurations[streamId].format);
            success = evsCamera->mVideo.open(deviceName,
                                             camInfo->streamConfigurations[streamId].width,
                                             camInfo->streamConfigurations[streamId].height);
            // Safe to statically cast
            // ::aidl::android::hardware::graphics::common::PixelFormat type to
            // android_pixel_format_t
            evsCamera->mFormat =
                    static_cast<uint32_t>(camInfo->streamConfigurations[streamId].format);
        }
    }

    if (!success) {
        // Create a camera object with the default resolution and format
        // HAL_PIXEL_FORMAT_BGRA_8888.
        LOG(INFO) << "Open a video with default parameters";
        success = evsCamera->mVideo.open(deviceName, VideoCaptureZeroCopy::getDefaultWidth(), VideoCaptureZeroCopy::getDefaultHeight());
        if (!success) {
            LOG(ERROR) << "Failed to open a video stream";
            return nullptr;
        }
    }

    // List available camera parameters
    evsCamera->mCameraControls = evsCamera->mVideo.enumerateCameraControls();

    return evsCamera;
}

Result<void> EvsV4lCameraZeroCopy::startDumpFrames(const std::string& path) {
    struct stat info;
    if (stat(path.data(), &info) != 0) {
        return Error(::android::BAD_VALUE) << "Cannot access " << path;
    } else if (!(info.st_mode & S_IFDIR)) {
        return Error(::android::BAD_VALUE) << path << " is not a directory";
    }

    mDumpPath = path;
    mDumpFrame = true;

    return {};
}

Result<void> EvsV4lCameraZeroCopy::stopDumpFrames() {
    if (!mDumpFrame) {
        return Error(::android::INVALID_OPERATION) << "Device is not dumping frames";
    }

    mDumpFrame = false;
    return {};
}

}  // namespace aidl::android::hardware::automotive::evs::implementation
