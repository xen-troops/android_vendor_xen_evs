Summary
=======
This project is Android's exterior view system (EVS) for Xen virtual machine.

Project implements Android's EVS HAL 1.0 https://source.android.com/devices/automotive/camera-hal.

Sources are based on EVS part of packages/services/Car project https://android.googlesource.com/platform/packages/services/Car/+/refs/heads/master/evs/.

Sources are divided as: EVS manager, evs_app, sample driver (implements EvsCamera, EvsDisplay).

Requirements
============
EVS scans /dev/video* for available cameras. If no suitable device found - message will be put into logcat.

Configuration
=============
evs_app_xt during runtime uses configuration provided in /system/etc/automotive/evs/evs_app_xt.json.
At compile time config is located at app/evs_app_xt.json.
See app/config.json.readme for some explanations of configuration options.

Debug
=====
Module doesn't use any special macroses/functions for debug. Only standard ALOG* macroses are used.

LOG_TAG used: "EvsApp", "EvsSampleDriver" and "EvsManager".

