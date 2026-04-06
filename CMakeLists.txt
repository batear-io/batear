# ESP32-S3 + ICS-43434 ambient sound monitor / drone acoustic detection
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)


# RadioLib overloads virtual methods with extra parameters (e.g. setOutputPower,
# getTimeOnAir) rather than overriding them.  ESP-IDF v6.x promotes
# -Woverloaded-virtual to a hard error via -Werror, which breaks the
# third-party library.  Demote it project-wide so only our code's real
# virtual-hiding bugs would be caught by code review instead.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=overloaded-virtual" APPEND)

project(batear)
