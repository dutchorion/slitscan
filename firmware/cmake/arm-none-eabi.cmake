# CMake toolchain file for arm-none-eabi-gcc (Arm GNU Toolchain)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Find the toolchain on PATH, or in the default Windows install location.
find_program(ARM_GCC arm-none-eabi-gcc
    PATHS "C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin")
if(NOT ARM_GCC)
    message(FATAL_ERROR "arm-none-eabi-gcc not found; add the Arm GNU Toolchain bin folder to PATH")
endif()
get_filename_component(ARM_BIN "${ARM_GCC}" DIRECTORY)

set(CMAKE_C_COMPILER   "${ARM_BIN}/arm-none-eabi-gcc.exe")
set(CMAKE_ASM_COMPILER "${ARM_BIN}/arm-none-eabi-gcc.exe")
set(CMAKE_OBJCOPY      "${ARM_BIN}/arm-none-eabi-objcopy.exe")
set(CMAKE_SIZE         "${ARM_BIN}/arm-none-eabi-size.exe")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
