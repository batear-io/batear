# ESP-MQTT: built-in "mqtt" on IDF 5.x, managed "espressif__mqtt" on IDF 6.x+
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(MQTT_COMPONENT espressif__mqtt)
    set(W5500_COMPONENT espressif__w5500)
else()
    set(MQTT_COMPONENT mqtt)
    set(W5500_COMPONENT "")
endif()

set(SRCS "main.cpp" "lorawan_provision.c" "config_console.c")

# RadioLib HAL only needed for LoRa roles
if(CONFIG_BATEAR_ROLE_DETECTOR OR CONFIG_BATEAR_ROLE_GATEWAY)
    list(APPEND SRCS "EspIdfHal.cpp")
endif()

if(CONFIG_BATEAR_ROLE_DETECTOR)
    list(APPEND SRCS "audio_task.c" "audio_processor.c" "lora_task.cpp" "battery.c")
endif()

if(CONFIG_BATEAR_ROLE_GATEWAY)
    list(APPEND SRCS "oled.c" "gateway_task.cpp" "mqtt_task.cpp")
endif()

if(CONFIG_BATEAR_ROLE_WIRED_DETECTOR)
    list(APPEND SRCS "audio_task.c" "audio_processor.c" "eth_mqtt_task.cpp" "http_api.cpp"
                     "tf_recorder.c" "manual_capture.c" "ntp_time.c")
endif()

set(PRIV_DEPS
    esp_driver_i2s
    esp_driver_i2c
    esp_driver_spi
    esp_driver_gpio
    esp_adc
    esp_timer
    esp_wifi
    esp_netif
    esp_event
    esp_eth
    ${MQTT_COMPONENT}
    espressif__esp-dsp
    mbedtls
    nvs_flash
    console
    app_update
    esp_app_format
    esp_http_server
    # TF recorder deps (Wired Detector). Listed unconditionally because IDF's
    # initial dependency-graph expansion of idf_component_register doesn't
    # have CONFIG_X defined yet. The linker GCs unused symbols when the
    # recorder is disabled / for non-wired roles.
    fatfs sdmmc esp_driver_sdmmc esp_ringbuf vfs
)

if(CONFIG_BATEAR_ROLE_DETECTOR OR CONFIG_BATEAR_ROLE_GATEWAY)
    list(APPEND PRIV_DEPS jgromes__radiolib)
endif()

if(CONFIG_BATEAR_ROLE_WIRED_DETECTOR AND W5500_COMPONENT)
    list(APPEND PRIV_DEPS ${W5500_COMPONENT})
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${PRIV_DEPS}
)

# Suppress RadioLib upstream warnings about overloaded virtuals (C++ only)
if(CONFIG_BATEAR_ROLE_DETECTOR OR CONFIG_BATEAR_ROLE_GATEWAY)
    target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-overloaded-virtual>)
endif()
