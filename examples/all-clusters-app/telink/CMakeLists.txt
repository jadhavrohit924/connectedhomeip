#
#    Copyright (c) 2022-2024 Project CHIP Authors
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#        http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
#

cmake_minimum_required(VERSION 3.13.1)

get_filename_component(CHIP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/connectedhomeip REALPATH)
get_filename_component(TELINK_COMMON ${CHIP_ROOT}/examples/platform/telink REALPATH)
get_filename_component(GEN_DIR ${CHIP_ROOT}/zzz_generated/ REALPATH)
get_filename_component(ALL_CLUSTERS_COMMON_DIR ${CHIP_ROOT}/examples/all-clusters-app/all-clusters-common REALPATH)

include(${TELINK_COMMON}/common.cmake)
include(${CHIP_ROOT}/config/telink/app/enable-gnu-std.cmake)
include(${CHIP_ROOT}/src/app/chip_data_model.cmake)

project(chip-telink-all-clusters-app-example)

target_include_directories(app PRIVATE
                           include
                           ${ALL_CLUSTERS_COMMON_DIR}/include
                           ${GEN_DIR}/app-common
                           ${GEN_DIR}/all-clusters-app
                           ${TELINK_COMMON}/common/include
                           ${TELINK_COMMON}/util/include
                           ${TELINK_COMMON}/zephyr_ext)

target_sources(app PRIVATE
               src/AppTask.cpp
               ${ALL_CLUSTERS_COMMON_DIR}/src/smco-stub.cpp
               ${ALL_CLUSTERS_COMMON_DIR}/src/static-supported-modes-manager.cpp
               ${ALL_CLUSTERS_COMMON_DIR}/src/static-supported-temperature-levels.cpp
               ${ALL_CLUSTERS_COMMON_DIR}/src/bridged-actions-stub.cpp
               ${ALL_CLUSTERS_COMMON_DIR}/src/binding-handler.cpp
               ${ALL_CLUSTERS_COMMON_DIR}/src/air-quality-instance.cpp
               ${ALL_CLUSTERS_COMMON_DIR}/src/chime-instance.cpp
               ${ALL_CLUSTERS_COMMON_DIR}/src/concentration-measurement-instances.cpp
               ${ALL_CLUSTERS_COMMON_DIR}/src/fan-stub.cpp
               ${ALL_CLUSTERS_COMMON_DIR}/src/resource-monitoring-delegates.cpp
               ${TELINK_COMMON}/common/src/mainCommon.cpp
               ${TELINK_COMMON}/common/src/AppTaskCommon.cpp
               ${TELINK_COMMON}/util/src/LEDManager.cpp
               ${TELINK_COMMON}/util/src/ButtonManager.cpp
               ${TELINK_COMMON}/util/src/ThreadUtil.cpp
               ${TELINK_COMMON}/util/src/PWMManager.cpp
               ${TELINK_COMMON}/zephyr_ext/zephyr_key_matrix.c
               ${TELINK_COMMON}/zephyr_ext/zephyr_key_pool.c
               ${TELINK_COMMON}/zephyr_ext/zephyr_led_pool.c
               ${TELINK_COMMON}/zephyr_ext/zephyr_pwm_pool.c
               ${TELINK_COMMON}/zephyr_ext/zephyr_ws2812.c)

chip_configure_data_model(app
    ZAP_FILE ${ALL_CLUSTERS_COMMON_DIR}/all-clusters-app.zap
)

if(CONFIG_BOOTLOADER_MCUBOOT)
  target_sources(app PRIVATE ${TELINK_COMMON}/util/src/OTAUtil.cpp)
endif()
