# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(lzamp_zephyr)

set(LZAMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_include_directories(app PRIVATE ${LZAMP_ROOT}/mailmsg)
target_include_directories(app PRIVATE ${LZAMP_ROOT}/zephyr/drivers)
target_sources(app PRIVATE
  src/main.c
  ${LZAMP_ROOT}/zephyr/drivers/gpio3_polling.c
  ${LZAMP_ROOT}/zephyr/drivers/rk3588_peripheral_clock.c
  ${LZAMP_ROOT}/zephyr/drivers/rk3588_pinctrl.c
  ${LZAMP_ROOT}/zephyr/drivers/rk3588_reset.c
  ${LZAMP_ROOT}/zephyr/drivers/rk3588_uart_polling.c
  ${LZAMP_ROOT}/zephyr/drivers/rk3588_i2c_polling.c
  ${LZAMP_ROOT}/zephyr/drivers/rk3588_spi_polling.c
  ${LZAMP_ROOT}/zephyr/drivers/rk3588_pwm.c
  ${LZAMP_ROOT}/zephyr/drivers/rk3588_saradc.c
  ${LZAMP_ROOT}/zephyr/drivers/rk3588_peripheral_service.c
  ${LZAMP_ROOT}/zephyr/cache-sync.S
  ${LZAMP_ROOT}/mailmsg/mailmsg.c
  ${LZAMP_ROOT}/mailmsg/mailmsg_endpoint.c
)

set(MAILMSG_TEST_SKIP_PRIORITY "" CACHE STRING "Test-only priority left unconsumed")
if(NOT MAILMSG_TEST_SKIP_PRIORITY STREQUAL "")
  target_compile_definitions(app PRIVATE MAILMSG_TEST_SKIP_PRIORITY=${MAILMSG_TEST_SKIP_PRIORITY})
endif()

set(MAILMSG_TEST_SELF_OFF_VALUE "" CACHE STRING "Test-only PING value that makes CPU3 self-off")
if(NOT MAILMSG_TEST_SELF_OFF_VALUE STREQUAL "")
  target_compile_definitions(app PRIVATE MAILMSG_TEST_SELF_OFF_VALUE=${MAILMSG_TEST_SELF_OFF_VALUE})
endif()

option(MAILMSG_ENABLE_STOP_CONTROL "Enable CPU3 STOP_REQUEST handling" OFF)
if(MAILMSG_ENABLE_STOP_CONTROL)
  target_compile_definitions(app PRIVATE MAILMSG_ENABLE_STOP_CONTROL=1)
endif()

option(MAILMSG_TEST_SKIP_SESSION_READY "Withhold SESSION_READY for timeout testing" OFF)
if(MAILMSG_TEST_SKIP_SESSION_READY)
  target_compile_definitions(app PRIVATE MAILMSG_TEST_SKIP_SESSION_READY=1)
endif()

set(LZAMP_PIN_PROFILE "i2c7-gpio" CACHE STRING "Pin profile: i2c7-gpio or spi0")
if(LZAMP_PIN_PROFILE STREQUAL "spi0")
  target_compile_definitions(app PRIVATE LZAMP_BUILD_PIN_PROFILE_SPI0=1)
elseif(NOT LZAMP_PIN_PROFILE STREQUAL "i2c7-gpio")
  message(FATAL_ERROR "LZAMP_PIN_PROFILE must be i2c7-gpio or spi0")
endif()
