set(SOURCES
  main.cpp
  config/config.cpp
  common/stream_controller.cpp
)

if(CAMERA_AGENT_BACKEND STREQUAL "gstreamer")
  list(APPEND SOURCES
    camera/camera_manager_gst.cpp
    pipeline/video_pipeline_gst.cpp
    rtsp/rtsp_publisher_gst.cpp
  )
else()
  list(APPEND SOURCES
    camera/camera_manager_sim.cpp
    pipeline/video_pipeline_sim.cpp
    rtsp/rtsp_publisher_sim.cpp
  )
endif()

add_executable(camera-agent ${SOURCES})

target_include_directories(camera-agent PRIVATE
  ${PROJECT_SOURCE_DIR}/include)

if(MSVC)
  target_compile_options(camera-agent PRIVATE /W4 /EHsc)
else()
  target_compile_options(camera-agent PRIVATE -Wall -Wextra)
endif()

target_link_libraries(camera-agent PRIVATE spdlog::spdlog)

if(CAMERA_AGENT_BACKEND STREQUAL "gstreamer")
  target_include_directories(camera-agent PRIVATE ${GST_INCLUDE_DIRS})
  target_link_libraries(camera-agent PRIVATE ${GST_LIBRARIES})
  target_compile_definitions(camera-agent PRIVATE
    CAMERA_AGENT_BACKEND_GSTREAMER
    CAMERA_AGENT_BACKEND_NAME="gstreamer")
else()
  target_compile_definitions(camera-agent PRIVATE
    CAMERA_AGENT_BACKEND_SIM
    CAMERA_AGENT_BACKEND_NAME="sim")
endif()
