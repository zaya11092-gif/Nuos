cmake_minimum_required(VERSION 3.16)
project(nuospvr CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)

find_package(Threads REQUIRED)

add_executable(nuospvr-server
  src/pvr_server.cpp
  src/win_capture.cpp
)

target_include_directories(nuospvr-server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${SWSCALE_INCLUDE_DIRS}
  ${X11_INCLUDE_DIR}
)

target_link_libraries(nuospvr-server PRIVATE
  ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${SWSCALE_LIBRARIES}
  ${X11_LIBRARIES}
  Threads::Threads
)
