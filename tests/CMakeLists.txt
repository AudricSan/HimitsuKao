add_executable(himitsukao_tests test-main.cpp test-identity.inc test-api.inc test-security.inc test-ui.inc)
target_link_libraries(himitsukao_tests PRIVATE himitsukao_core)
target_compile_definitions(
  himitsukao_tests
  PRIVATE
    HIMITSUKAO_TEST_MODEL="${CMAKE_SOURCE_DIR}/data/models/face_detection_yunet_2023mar.onnx"
    HIMITSUKAO_TEST_SFACE_MODEL="${CMAKE_SOURCE_DIR}/data/models/face_recognition_sface_2021dec.onnx"
)
add_test(NAME himitsukao_tests COMMAND himitsukao_tests)

# Dev tool: the phone page with fake faces, no OBS needed.
add_executable(himitsukao_devserver dev-server.cpp)
target_link_libraries(himitsukao_devserver PRIVATE himitsukao_core)
