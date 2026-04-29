cmake_minimum_required(VERSION 3.20)
project(kpnvme LANGUAGES C)

set(KERNEL_VERSION "6.13.9")
set(KERNEL_DIR "${CMAKE_SOURCE_DIR}/linux-${KERNEL_VERSION}")
set(BUILD_DIR "${CMAKE_SOURCE_DIR}/kernel-build")
set(ROOTFS_DIR "${CMAKE_SOURCE_DIR}/rootfs")
set(NPROC 0)

cmake_host_system_information(RESULT NPROC QUERY NUMBER_OF_LOGICAL_CORES)

add_custom_target(kernel
    COMMAND ${CMAKE_COMMAND} -E env NIX_CFLAGS_COMPILE= make -C ${KERNEL_DIR} -j${NPROC} bzImage
    COMMENT "Building kernel bzImage"
    USES_TERMINAL
)

add_custom_target(modules
    COMMAND ${CMAKE_COMMAND} -E env NIX_CFLAGS_COMPILE= make -C ${KERNEL_DIR} -j${NPROC} modules
    COMMENT "Building kernel modules"
    USES_TERMINAL
)

add_custom_target(driver
    COMMAND ${CMAKE_COMMAND} -E env NIX_CFLAGS_COMPILE= make -C ${KERNEL_DIR} M=${CMAKE_SOURCE_DIR} modules
    COMMENT "Building kpnvme driver"
    USES_TERMINAL
)

add_custom_target(driver-clean
    COMMAND make -C ${KERNEL_DIR} M=${CMAKE_SOURCE_DIR} clean
    COMMENT "Cleaning kpnvme driver"
    USES_TERMINAL
)

add_custom_target(rootfs
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/pack-rootfs.sh ${CMAKE_SOURCE_DIR} ${ROOTFS_DIR} ${BUILD_DIR}
    COMMENT "Packing rootfs"
    USES_TERMINAL
)

add_custom_target(nvme-img
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUILD_DIR}
    COMMAND sh -c "test -f ${BUILD_DIR}/nvme.img || qemu-img create -f raw ${BUILD_DIR}/nvme.img 1G"
    COMMENT "Creating NVMe disk image"
)

add_custom_target(run
    COMMAND qemu-system-x86_64
        -kernel ${KERNEL_DIR}/arch/x86/boot/bzImage
        -initrd ${BUILD_DIR}/rootfs.cpio.gz
        -append "console=tty0 console=ttyS0 earlyprintk=serial loglevel=8"
        -cpu host
        -smp 6
        -m 4G
        -enable-kvm
        -M q35
        -vga std
        -display gtk
        -serial mon:stdio
        -chardev file,id=log,path=${BUILD_DIR}/kernel.log
        -serial chardev:log
        -device nvme,serial=deadbeef,drive=nvme0
        -drive id=nvme0,if=none,format=raw,file=${BUILD_DIR}/nvme.img
    COMMENT "Starting QEMU"
    USES_TERMINAL
    DEPENDS nvme-img
)

add_custom_target(dev
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target driver
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target rootfs
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target run
    COMMENT "Build driver + pack rootfs + run QEMU"
    USES_TERMINAL
)

add_custom_target(build-and-run
    DEPENDS kernel nvme-img
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target driver
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target rootfs
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target run
    COMMENT "Build kernel + driver + rootfs, then run QEMU"
    USES_TERMINAL
)

file(GLOB_RECURSE KPNVME_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/include/*.h"
)
file(GLOB_RECURSE KERNEL_NVME_SOURCES
    "${KERNEL_DIR}/drivers/nvme/*.c"
    "${KERNEL_DIR}/drivers/nvme/*.h"
    "${KERNEL_DIR}/include/linux/nvme*.h"
    "${KERNEL_DIR}/include/uapi/linux/nvme*.h"
)

add_library(kernel_sources EXCLUDE_FROM_ALL ${KPNVME_SOURCES} ${KERNEL_NVME_SOURCES}
        )
target_include_directories(kernel_sources PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${KERNEL_DIR}/include
    ${KERNEL_DIR}/include/uapi
    ${KERNEL_DIR}/include/generated
    ${KERNEL_DIR}/include/generated/uapi
    ${KERNEL_DIR}/arch/x86/include
    ${KERNEL_DIR}/arch/x86/include/generated
    ${KERNEL_DIR}/arch/x86/include/generated/uapi
    ${KERNEL_DIR}/arch/x86/include/uapi
)
target_compile_definitions(kernel_sources PRIVATE
    __KERNEL__
    MODULE
    KBUILD_MODNAME="kpnvme"
    CONFIG_X86_64
    __x86_64__
)
set_target_properties(kernel_sources PROPERTIES LINKER_LANGUAGE C)
