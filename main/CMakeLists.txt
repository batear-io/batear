set(SRCS "main.cpp" "EspIdfHal.cpp" "lorawan_provision.c")

if(CONFIG_BATEAR_ROLE_DETECTOR)
    list(APPEND SRCS "audio_task.c" "audio_processor.c" "lora_task.cpp")
endif()

if(CONFIG_BATEAR_ROLE_GATEWAY)
    list(APPEND SRCS "oled.c" "gateway_task.cpp")
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
        espressif__esp-dsp
        jgromes__radiolib
        mbedtls
        nvs_flash
)

# Suppress RadioLib upstream warnings about overloaded virtuals (C++ only)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-overloaded-virtual>)
