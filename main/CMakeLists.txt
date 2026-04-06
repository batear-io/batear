# ESP-MQTT: built-in "mqtt" on IDF 5.x, managed "espressif__mqtt" on IDF 6.x+
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(MQTT_COMPONENT espressif__mqtt)
else()
    set(MQTT_COMPONENT mqtt)
endif()

set(SRCS "main.cpp" "EspIdfHal.cpp" "lorawan_provision.c" "config_console.c")

if(CONFIG_BATEAR_ROLE_DETECTOR)
    list(APPEND SRCS "audio_task.c" "audio_processor.c" "lora_task.cpp")
endif()

if(CONFIG_BATEAR_ROLE_GATEWAY)
    list(APPEND SRCS "oled.c" "gateway_task.cpp" "mqtt_task.cpp")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_i2s
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        ${MQTT_COMPONENT}
        espressif__esp-dsp
        jgromes__radiolib
        mbedtls
        nvs_flash
        console
)

# Suppress RadioLib upstream warnings about overloaded virtuals (C++ only)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-overloaded-virtual>)
