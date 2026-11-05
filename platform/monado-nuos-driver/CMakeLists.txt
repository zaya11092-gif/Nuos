# Not standalone-buildable: this file is copied into a patched Monado tree by
# build-monado.sh, which then appends this directory to
# src/xrt/drivers/CMakeLists.txt (via patches/nuos_posed-drivers-cmake.patch)
# and configures with -DXRT_BUILD_DRIVER_NUOS_POSED=ON.

add_library(drv_nuos_posed STATIC nuos_posed.c)
target_include_directories(drv_nuos_posed
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/xrt
    ${CMAKE_SOURCE_DIR}/src/xrt/include
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_BINARY_DIR}/src
)
target_link_libraries(drv_nuos_posed PRIVATE aux_util xrt-interfaces rt)
set_property(TARGET drv_nuos_posed PROPERTY POSITION_INDEPENDENT_CODE ON)
add_definitions(-DXRT_BUILD_DRIVER_NUOS_POSED=1)
