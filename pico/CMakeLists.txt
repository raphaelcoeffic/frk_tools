cmake_minimum_required(VERSION 3.13)

# Needs PICO_SDK_PATH (environment or -D), e.g.
#   cmake -B build -DPICO_SDK_PATH=~/Src/pico-sdk
include(pico_sdk_import.cmake)

project(frkpico C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(frkpico main.c)

# Pin assignment, override with e.g. -DSPORT_UART=1 -DSPORT_TX_PIN=4 -DSPORT_RX_PIN=5
set(SPORT_UART   0 CACHE STRING "UART instance for the S.Port line")
set(SPORT_TX_PIN 0 CACHE STRING "UART TX pin (tied to SPORT_RX_PIN)")
set(SPORT_RX_PIN 1 CACHE STRING "UART RX pin (tied to SPORT_TX_PIN)")
set(POWER_PIN    2 CACHE STRING "receiver power switch, high = on")

target_compile_definitions(frkpico PRIVATE
    SPORT_UART=${SPORT_UART}
    SPORT_TX_PIN=${SPORT_TX_PIN}
    SPORT_RX_PIN=${SPORT_RX_PIN}
    POWER_PIN=${POWER_PIN}
)

target_compile_options(frkpico PRIVATE -Wall -Wextra)
target_link_libraries(frkpico pico_stdlib hardware_uart hardware_irq)

pico_enable_stdio_usb(frkpico 1)
pico_enable_stdio_uart(frkpico 0)
pico_add_extra_outputs(frkpico)
