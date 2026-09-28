cmake_minimum_required(VERSION 3.20)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/arm-none-eabi.cmake)
project(slitscan C ASM)

set(CMAKE_C_STANDARD 11)
set(FW_VERSION "2.0.0-m3.3")

set(DRIVERS ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers)
set(HAL ${DRIVERS}/STM32H7xx_HAL_Driver)

set(HAL_SOURCES
    ${HAL}/Src/stm32h7xx_hal.c
    ${HAL}/Src/stm32h7xx_hal_adc.c
    ${HAL}/Src/stm32h7xx_hal_adc_ex.c
    ${HAL}/Src/stm32h7xx_hal_cortex.c
    ${HAL}/Src/stm32h7xx_hal_dma.c
    ${HAL}/Src/stm32h7xx_hal_dma_ex.c
    ${HAL}/Src/stm32h7xx_hal_mdma.c
    ${HAL}/Src/stm32h7xx_hal_exti.c
    ${HAL}/Src/stm32h7xx_hal_flash.c
    ${HAL}/Src/stm32h7xx_hal_flash_ex.c
    ${HAL}/Src/stm32h7xx_hal_gpio.c
    ${HAL}/Src/stm32h7xx_hal_hsem.c
    ${HAL}/Src/stm32h7xx_hal_pwr.c
    ${HAL}/Src/stm32h7xx_hal_pwr_ex.c
    ${HAL}/Src/stm32h7xx_hal_rcc.c
    ${HAL}/Src/stm32h7xx_hal_rcc_ex.c
    ${HAL}/Src/stm32h7xx_hal_rtc.c
    ${HAL}/Src/stm32h7xx_hal_rtc_ex.c
    ${HAL}/Src/stm32h7xx_hal_sd.c
    ${HAL}/Src/stm32h7xx_hal_sd_ex.c
    ${HAL}/Src/stm32h7xx_hal_sdram.c
    ${HAL}/Src/stm32h7xx_hal_spi.c
    ${HAL}/Src/stm32h7xx_hal_spi_ex.c
    ${HAL}/Src/stm32h7xx_ll_delayblock.c
    ${HAL}/Src/stm32h7xx_ll_fmc.c
    ${HAL}/Src/stm32h7xx_ll_sdmmc.c
)

set(FATFS_SOURCES
    lib/fatfs/ff.c
    lib/fatfs/ffunicode.c
)

add_executable(slitscan
    startup/startup_stm32h743xx.s
    src/main.c
    src/board.c
    src/sdram.c
    src/rtclock.c
    src/sdcard.c
    src/diskio.c
    src/log.c
    src/lcd.c
    src/font7x10.c
    src/touch.c
    src/sensor.c
    src/syscalls.c
    src/stm32h7xx_it.c
    src/system_stm32h7xx.c
    ${HAL_SOURCES}
    ${FATFS_SOURCES}
)

target_include_directories(slitscan PRIVATE
    inc
    src
    lib/fatfs
    ${HAL}/Inc
    ${DRIVERS}/CMSIS/Device/ST/STM32H7xx/Include
    ${DRIVERS}/CMSIS/Include
)

target_compile_definitions(slitscan PRIVATE
    STM32H743xx
    USE_HAL_DRIVER
    FW_VERSION="${FW_VERSION}"
)

set(CPU_FLAGS -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)

target_compile_options(slitscan PRIVATE
    ${CPU_FLAGS}
    -Og -g3 -Wall -ffunction-sections -fdata-sections
)

target_link_options(slitscan PRIVATE
    ${CPU_FLAGS}
    -T${CMAKE_CURRENT_SOURCE_DIR}/linker/STM32H743XIHX_FLASH.ld
    --specs=nano.specs --specs=nosys.specs
    -Wl,--gc-sections -Wl,-Map=slitscan.map -Wl,--print-memory-usage
    -Wl,--no-warn-rwx-segments
)

set_target_properties(slitscan PROPERTIES SUFFIX ".elf")

add_custom_command(TARGET slitscan POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:slitscan> slitscan.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:slitscan> slitscan.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:slitscan>
)
