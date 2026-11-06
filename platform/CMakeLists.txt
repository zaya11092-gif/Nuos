cmake_minimum_required(VERSION 3.16)
project(nuos-platform C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -fno-strict-aliasing)

find_package(PkgConfig REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core videoio imgproc features2d video calib3d)
pkg_check_modules(LIBVA REQUIRED libva)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(AVUTIL REQUIRED libavutil)

find_package(Threads REQUIRED)
set(NUOS_COMMON_LIBS Threads::Threads rt)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../proto
  ${CMAKE_CURRENT_SOURCE_DIR}/src/nuos-tracker
  ${OpenCV_INCLUDE_DIRS}
  ${LIBVA_INCLUDE_DIRS}
  ${DRM_INCLUDE_DIRS}
  ${SWSCALE_INCLUDE_DIRS}
  ${AVUTIL_INCLUDE_DIRS}
)
link_directories(${LIBVA_LIBRARY_DIRS} ${DRM_LIBRARY_DIRS}
                 ${SWSCALE_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS})

# ---------------------------------------------------------------- nuos-tracker
add_executable(nuos-tracker
  src/nuos-tracker/main.cpp
  src/nuos-tracker/v4l2_source.cpp
  src/nuos-tracker/vo_klt.cpp
  src/nuos-tracker/fusion.cpp
  src/nuos-tracker/telem_recv.cpp
  src/nuos-tracker/state_pub.cpp
)
target_link_libraries(nuos-tracker PRIVATE ${OpenCV_LIBS} ${NUOS_COMMON_LIBS})

# ---------------------------------------------------------------- nuos-stream
add_executable(nuos-stream
  src/nuos-stream/main.cpp
  src/nuos-stream/ring_reader.cpp
  src/nuos-stream/va_encoder.cpp
  src/nuos-stream/packetizer.cpp
  src/nuos-stream/udp_send.cpp
  src/nuos-stream/control_server.cpp
)
target_link_libraries(nuos-stream PRIVATE va va-drm drm swscale avutil ${NUOS_COMMON_LIBS})

# ---------------------------------------------------------------- nuos-handbus
add_executable(nuos-frameudp
  src/nuos-frameudp/main.c
  include/nuos_ring.h
)
target_include_directories(nuos-frameudp PRIVATE include)
target_link_libraries(nuos-frameudp PRIVATE rt Threads::Threads)

add_executable(nuos-handbus
  src/nuos-handbus/main.cpp
  src/nuos-handbus/hands_shm.cpp
)
target_link_libraries(nuos-handbus PRIVATE ${NUOS_COMMON_LIBS})

# ---------------------------------------------------------------------- tests
enable_testing()

add_executable(proto_roundtrip_test src/tests/proto_roundtrip_test.cpp)
target_link_libraries(proto_roundtrip_test PRIVATE ${NUOS_COMMON_LIBS})
add_test(NAME proto_roundtrip COMMAND proto_roundtrip_test)

add_executable(ring_roundtrip_test src/tests/ring_roundtrip_test.cpp)
target_link_libraries(ring_roundtrip_test PRIVATE ${NUOS_COMMON_LIBS})
add_test(NAME ring_roundtrip COMMAND ring_roundtrip_test)

add_executable(replay_tracker_test
  src/tests/replay_tracker_test.cpp
  src/nuos-tracker/v4l2_source.cpp
  src/nuos-tracker/vo_klt.cpp
)
target_link_libraries(replay_tracker_test PRIVATE ${OpenCV_LIBS} ${NUOS_COMMON_LIBS})
add_test(NAME replay_tracker COMMAND replay_tracker_test)
set_tests_properties(replay_tracker PROPERTIES
  ENVIRONMENT "NUOS_TEST_FIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}")
