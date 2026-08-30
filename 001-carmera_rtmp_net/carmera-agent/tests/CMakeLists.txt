# Unit tests. They always run against the SIM backend so they can execute
# headlessly on any machine (no camera / no GStreamer required).
set(TEST_SOURCES
  run_all.cpp
  backoff_tests.cpp
  camera_tests.cpp
  pipeline_tests.cpp
  rtsp_tests.cpp
  reconnect_tests.cpp
  config_tests.cpp
)

# Reuse the SIM backend sources from the app (deterministic, headless).
set(APP_SIM_SOURCES
  ${PROJECT_SOURCE_DIR}/src/config/config.cpp
  ${PROJECT_SOURCE_DIR}/src/common/stream_controller.cpp
  ${PROJECT_SOURCE_DIR}/src/camera/camera_manager_sim.cpp
  ${PROJECT_SOURCE_DIR}/src/pipeline/video_pipeline_sim.cpp
  ${PROJECT_SOURCE_DIR}/src/rtsp/rtsp_publisher_sim.cpp
)

add_executable(camera-agent-tests ${TEST_SOURCES} ${APP_SIM_SOURCES})

target_include_directories(camera-agent-tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(camera-agent-tests PRIVATE /W4 /EHsc)
else()
  target_compile_options(camera-agent-tests PRIVATE -Wall -Wextra)
endif()
target_link_libraries(camera-agent-tests PRIVATE spdlog::spdlog)
target_compile_definitions(camera-agent-tests PRIVATE
  CAMERA_AGENT_BACKEND_SIM
  CAMERA_AGENT_BACKEND_NAME="sim"
  CAMERA_AGENT_VERSION="0.1.0")

add_test(NAME camera-agent-tests COMMAND camera-agent-tests)
