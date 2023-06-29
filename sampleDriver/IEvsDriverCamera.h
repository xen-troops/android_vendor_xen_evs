#ifndef ANDROID_HARDWARE_AUTOMOTIVE_EVS_V1_1_IEVSDRIVERCAMERA_H
#define ANDROID_HARDWARE_AUTOMOTIVE_EVS_V1_1_IEVSDRIVERCAMERA_H

#include <android/hardware/automotive/evs/1.1/types.h>
#include <android-base/result.h>
#include <string>

namespace android {
namespace hardware {
namespace automotive {
namespace evs {
namespace V1_1 {
namespace implementation {

/**
 * Represents a single camera and is the primary interface for capturing images.
 */
class IEvsDriverCamera {
 public:

   virtual ~IEvsDriverCamera() = 0;
   virtual void shutdown() = 0;
   virtual const CameraDesc& getDesc() = 0;

   // Dump captured frames to the filesystem
   virtual android::base::Result<void> startDumpFrames(const std::string& path) = 0;
   virtual android::base::Result<void> stopDumpFrames() = 0;

};

} // namespace implementation
} // namespace V1_1
} // namespace evs
} // namespace automotive
} // namespace hardware
} // namespace android

#endif  // ANDROID_HARDWARE_AUTOMOTIVE_EVS_V1_1_IEVSDRIVERCAMERA_H
