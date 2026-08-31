cmake_minimum_required(VERSION 3.16)
project(camera-agent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CAMERA_AGENT_BUILD_TESTS "Build the test suite" ON)
set(CAMERA_AGENT_BACKEND "auto" CACHE STRING "Pipeline backend: gstreamer|sim|auto")

add_compile_definitions(CAMERA_AGENT_VERSION="0.1.0")

# ---- spdlog (mature logging library, fetched from GitHub) -------------------
include(FetchContent)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.14.1
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(spdlog)

# ---- GStreamer detection (only needed for the real backend) -----------------
# On MSVC we locate the official GStreamer "devel" package directly (no
# pkg-config required). On other toolchains (MinGW) we use pkg-config.
#
# GSTREAMER_ROOT auto-detects among common install locations so a plain
# `cmake -S .` works out of the box; override with -DGSTREAMER_ROOT=... if
# your install lives elsewhere.
set(HAVE_GSTREAMER OFF)

set(_gst_candidates
  "$ENV{GSTREAMER_ROOT}"
  "C:/Program Files/gstreamer/1.0/msvc_x86_64"
  "C:/gstreamer/1.0/msvc_x86_64"
  "D:/Software/gstreamer/1.0/msvc_x86_64")
set(GSTREAMER_ROOT "" CACHE PATH "GStreamer 1.0 install root (official MSVC devel package)")
if(NOT GSTREAMER_ROOT)
  foreach(_c ${_gst_candidates})
    if(_c AND IS_DIRECTORY "${_c}")
      set(GSTREAMER_ROOT "${_c}")
      break()
    endif()
  endforeach()
endif()
if(NOT GSTREAMER_ROOT)
  set(GSTREAMER_ROOT "C:/Program Files/gstreamer/1.0/msvc_x86_64")
endif()

if(CAMERA_AGENT_BACKEND STREQUAL "gstreamer" OR CAMERA_AGENT_BACKEND STREQUAL "auto")
  if(MSVC)
    find_path(_gst_inc gst/gst.h
      PATHS "${GSTREAMER_ROOT}/include/gstreamer-1.0" NO_DEFAULT_PATH)
    find_path(_glib_inc glib.h
      PATHS "${GSTREAMER_ROOT}/include/glib-2.0" NO_DEFAULT_PATH)
    find_path(_glibcfg_inc glibconfig.h
      PATHS "${GSTREAMER_ROOT}/lib/glib-2.0/include" NO_DEFAULT_PATH)
    if(_gst_inc AND _glib_inc AND _glibcfg_inc)
      set(HAVE_GSTREAMER ON)
      set(GST_INCLUDE_DIRS "${_gst_inc}" "${_glib_inc}" "${_glibcfg_inc}")
      set(GST_LIBRARY_DIR "${GSTREAMER_ROOT}/lib")
      # Required import libraries (MSVC .lib names from the devel package).
      foreach(_l gstreamer-1.0 gstapp-1.0 gstrtspserver-1.0 gstvideo-1.0
                  gstbase-1.0 gobject-2.0 glib-2.0 gio-2.0)
        find_library(_lib_${_l} NAMES ${_l} PATHS "${GSTREAMER_ROOT}/lib" NO_DEFAULT_PATH)
        if(_lib_${_l})
          list(APPEND GST_LIBRARIES "${_lib_${_l}}")
        else()
          message(WARNING "GStreamer import library not found: ${_l}")
        endif()
      endforeach()
      # Optional transitive dependencies.
      foreach(_l intl ffi z)
        find_library(_lib_${_l} NAMES ${_l} PATHS "${GSTREAMER_ROOT}/lib" NO_DEFAULT_PATH)
        if(_lib_${_l})
          list(APPEND GST_LIBRARIES "${_lib_${_l}}")
        endif()
      endforeach()
    endif()
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(GST QUIET
        gstreamer-1.0>=1.14
        gstreamer-app-1.0
        gstreamer-rtsp-1.0
        gstreamer-video-1.0)
      if(GST_FOUND)
        set(HAVE_GSTREAMER ON)
      endif()
    endif()
  endif()
endif()

# ---- Resolve backend --------------------------------------------------------
if(CAMERA_AGENT_BACKEND STREQUAL "auto")
  if(HAVE_GSTREAMER)
    set(CAMERA_AGENT_BACKEND "gstreamer")
  else()
    set(CAMERA_AGENT_BACKEND "sim")
  endif()
endif()

message(STATUS "Camera-Agent backend selected: ${CAMERA_AGENT_BACKEND}")
message(STATUS "GStreamer root: ${GSTREAMER_ROOT}")

add_subdirectory(src)

if(CAMERA_AGENT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
