# Build & Flash

!!! tip "Just want to flash? Use the [Web Flasher](https://docs.batear.io/flasher/){ target="_blank" }"
    No toolchain installation needed — flash Detector or Gateway firmware directly from Chrome/Edge in under 60 seconds. The instructions below are for building from source with custom configuration.

The same codebase builds either role. Each role has its own build directory so both can coexist.

## 1. Clone & find your serial port

```bash
git clone https://github.com/batear-io/batear.git
cd batear
ls /dev/cu.usb*          # note your port, e.g. /dev/cu.usbserial-3
```

## 2. Build & flash the Detector

```bash
# First time only — set chip target (esp32s3 for Heltec V3/V4)
idf.py -B build_detector \
  -DSDKCONFIG=build_detector/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.detector" \
  set-target esp32s3

# Build
idf.py -B build_detector \
  -DSDKCONFIG=build_detector/sdkconfig \
  build

# Flash & monitor (replace PORT)
idf.py -B build_detector \
  -DSDKCONFIG=build_detector/sdkconfig \
  -p PORT flash monitor
```

## 3. Build & flash the Gateway

```bash
# First time only — set chip target (esp32s3 for Heltec V3/V4)
idf.py -B build_gateway \
  -DSDKCONFIG=build_gateway/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.gateway" \
  set-target esp32s3

# Build
idf.py -B build_gateway \
  -DSDKCONFIG=build_gateway/sdkconfig \
  build

# Flash & monitor (replace PORT)
idf.py -B build_gateway \
  -DSDKCONFIG=build_gateway/sdkconfig \
  -p PORT flash monitor
```

## 4. Build & flash the Wired Detector

The wired detector uses a LILYGO T-ETH-Lite S3 board with W5500 Ethernet. It sends alerts directly over MQTT — no gateway required.

```bash
# First time only — set chip target (esp32s3 for T-ETH-Lite S3)
idf.py -B build_wired_detector \
  -DSDKCONFIG=build_wired_detector/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.wired_detector" \
  set-target esp32s3

# Build
idf.py -B build_wired_detector \
  -DSDKCONFIG=build_wired_detector/sdkconfig \
  build

# Flash & monitor (replace PORT)
idf.py -B build_wired_detector \
  -DSDKCONFIG=build_wired_detector/sdkconfig \
  -p PORT flash monitor
```

## After first setup

Code changes only need rebuild + flash (no `set-target`):

```bash
# Detector
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig build
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig -p PORT flash monitor

# Gateway
idf.py -B build_gateway -DSDKCONFIG=build_gateway/sdkconfig build
idf.py -B build_gateway -DSDKCONFIG=build_gateway/sdkconfig -p PORT flash monitor

# Wired Detector
idf.py -B build_wired_detector -DSDKCONFIG=build_wired_detector/sdkconfig build
idf.py -B build_wired_detector -DSDKCONFIG=build_wired_detector/sdkconfig -p PORT flash monitor
```

!!! tip
    Replace `PORT` with your actual serial port (e.g. `/dev/cu.usbserial-3` on macOS, `/dev/ttyUSB0` on Linux).
