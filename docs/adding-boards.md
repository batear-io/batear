# Adding a New Board

Batear uses a board abstraction layer — all board-specific GPIO and hardware traits are defined in one place (`main/pin_config.h`), selected by a Kconfig option. Here's how to port to a new board.

## Step 1: Kconfig — register the board

Add an entry under `BATEAR_BOARD` in `main/Kconfig.projbuild`:

```kconfig
config BATEAR_BOARD_MY_NEW_BOARD
    bool "My New Board (ESP32 + SX1276)"
```

## Step 2: pin_config.h — define all hardware constants

Add an `#elif` block in `main/pin_config.h`. Every board must define **all** of these macros:

```c
#elif defined(CONFIG_BATEAR_BOARD_MY_NEW_BOARD)

#define BOARD_IDF_TARGET    "esp32"     // chip for set-target
#define BOARD_FLASH_SIZE    "4MB"       // for documentation

// I2S microphone
#define PIN_I2S_BCLK     26
#define PIN_I2S_WS       25
#define PIN_I2S_DIN      34

// LoRa SPI
#define PIN_LORA_SCK      5
#define PIN_LORA_MISO    19
#define PIN_LORA_MOSI    27
#define PIN_LORA_CS      18
#define PIN_LORA_DIO1    26
#define PIN_LORA_RST     23
#define PIN_LORA_BUSY    -1            // SX1276 has no BUSY pin

// Gateway peripherals
#define PIN_OLED_SDA      4
#define PIN_OLED_SCL     15
#define PIN_OLED_RST     16
#define PIN_LED           2
#define PIN_VEXT         -1
#define BOARD_HAS_VEXT    0            // no Vext power control
#define BOARD_HAS_OLED    1

// LoRa RF traits
#define BOARD_LORA_TCXO_V       0.0f   // no TCXO
#define BOARD_LORA_DIO2_AS_RF   false  // no DIO2 RF switch

// Battery monitor — set to 0 if the board has no VBAT divider.
// When set to 1 you must also define PIN_VBAT_ADC (ADC1 channel pin),
// PIN_VBAT_CTRL (active-low gate for the divider), and
// BOARD_VBAT_DIVIDER_RATIO (float, VBAT / V_at_ADC).
#define BOARD_HAS_VBAT          0
```

## Step 3: sdkconfig files — board + flash size

Copy `sdkconfig.detector` and `sdkconfig.gateway`, change the board and flash lines:

```ini
CONFIG_BATEAR_BOARD_MY_NEW_BOARD=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
```

## Step 4: Build — use the correct chip target

`set-target` must match `BOARD_IDF_TARGET` (ESP-IDF only accepts chip names, not board names):

```bash
idf.py -B build_detector \
  -DSDKCONFIG=build_detector/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.detector" \
  set-target esp32          # ← matches BOARD_IDF_TARGET
```

## Step 5: If the LoRa chip is different

If the board uses SX1276/SX1278 instead of SX1262, you'll need to update `lora_task.cpp` and `gateway_task.cpp` to instantiate the correct RadioLib class. Use `#ifdef` on your board macro for conditional compilation.

!!! tip "Ethernet boards"
    For boards with Ethernet (e.g. LILYGO T-ETH-Lite S3), define `PIN_ETH_*` pins and `BOARD_HAS_ETH 1` instead of `PIN_LORA_*` pins. See the T-ETH-Lite S3 block in `pin_config.h` for a working example. These boards use the `WIRED_DETECTOR` role and a separate `sdkconfig.wired_detector`.

## Checklist

- [ ] `Kconfig.projbuild` — board entry added
- [ ] `pin_config.h` — `#elif` block with all `PIN_*` and `BOARD_*` macros (including `BOARD_HAS_VBAT`)
- [ ] `sdkconfig.*` — board + flash size + role set
- [ ] `set-target` matches the chip (`esp32`, `esp32s3`, `esp32c3`, ...)
- [ ] Board row added to the build table in docs
- [ ] Build + flash tested on hardware
