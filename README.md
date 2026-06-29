# Summary

This project provides an Android Exterior View System (EVS) implementation using the `rcar-vivid` camera driver on the Renesas V4H board.

It implements the Android EVS AIDL HAL as described in the official documentation:  
https://source.android.com/docs/automotive/camera-hal  
for Android 16.


### Components

The codebase is organized into the following components:
- **EVS Manager**
- **EVS App**
- **Sample Driver**: Implements `EvsCamera` and `EvsDisplay`.

---

## Requirements

- Renesas V4H board  
- 4 × IMX623 cameras  
- CR52 camera firmware  

---

## Configuration

- Format and device definitions:  
  `sampleDriver/aidl/resources/evs_aidl_hal_configuration.xml`

- Application configuration:  
  `apps/default/res/config.json`

---

## Testing

`evs_app`, `evsmanager`, and `evs_hal` are started automatically during boot.  
To verify their state, use:

```sh
ps -A | grep evs
```

### Example output:

```
automotive_evs 134     1   10900248   6444 binder_thread_read  0 S evsmanagerd-xt
graphics       401     1   11242864  29080 binder_thread_read  0 S android.hardware.automotive.evs-xt
automotive_evs 2409    1   11230008  28508 futex_wait_queue_me 0 S evs_app-xt
```

### Vehicle Property Subscription

`evs_app` subscribes to the following vehicle properties:
- `VehicleProperty::GEAR_SELECTION`
- `VehicleProperty::TURN_SIGNAL_STATE`

To test camera preview functionality:

#### Show reverse camera (all 4 views):

```sh
adb shell lshal debug android.hardware.automotive.vehicle@2.0::IVehicle/default --debughal --setint 0x11400400 2 0
```

This should display all 4 camera views, assuming "reverse" is set for each in `config.json`.

#### Switch back to drive mode:

```sh
adb shell lshal debug android.hardware.automotive.vehicle@2.0::IVehicle/default --debughal --setint 0x11400400 4 0
```

More info:  
[VehicleGear.java](https://cs.android.com/android/platform/superproject/main/+/main:packages/services/Car/car-lib/src/android/car/VehicleGear.java;l=26?q=VehicleGear)

#### Show right camera:

```sh
adb shell lshal debug android.hardware.automotive.vehicle@2.0::IVehicle/default --debughal --setint 0x11400408 1 0
```

#### Show left camera:

```sh
adb shell lshal debug android.hardware.automotive.vehicle@2.0::IVehicle/default --debughal --setint 0x11400408 2 0
```

#### Reset turn signal:

```sh
adb shell lshal debug android.hardware.automotive.vehicle@2.0::IVehicle/default --debughal --setint 0x11400408 0 0
```

More info:  
[VehicleTurnSignal.java](https://cs.android.com/android/platform/superproject/main/+/main:packages/services/Car/car-lib/src/android/car/hardware/property/VehicleTurnSignal.java;l=36?q=VehicleTurnSignal)

---

## Implementation Details

Current implementation contains zerocopy and nonzerocopy HAL builds. By default, it is zerocopy. To test non-zerocopy, comment out in Android.bp:

```
"-DUSE_ZEROCOPY",
```

The original HAL, application, and manager were modified to support the `rcar-vivid` driver and enable zerocopy/non-zerocopy buffer handling.

Originally, buffer copies were performed inside the HAL, which negatively impacted performance. With the current zerocopy implementation, the EVS services use approximately **30% CPU** for 4-camera preview.

The `evs_hal` allocates buffers from a dedicated memory area shared with CR52. This area is limited in size. Because of that, in the current implementation `importBuffers()` does **not** use external buffers — it allocates its own instead.

**Note: Interaction with zerocopy HAL may lead to out-of-memory allocations from shared memory (if clients are holding buffers, for example), since the size of it is limited. External buffering is also not supported in the original way, as described above. All these problems are not present in non-zerocopy HAL.**

---

## Known issues

* No multicamera supported for now
* Domain reboot/destroy may fail
* Start/stop streaming may produce a lot of error messages
* Camera device open may take a lot of time ~10+sec