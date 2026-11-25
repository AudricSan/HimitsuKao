cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(HIMITSUKAO_BUILD_TESTS "Build the unit tests (run with ctest)" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/opencv.cmake")

# nlohmann/json (MIT, header-only) for the known-people database.
include(FetchContent)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

# Pure C++ core (no OBS headers): tracking, detection, recognition, people database. Shared by the plugin and the tests.
add_library(
  himitsukao_core
  STATIC
  src/core/face-tracker.cpp
  src/core/face-detector.cpp
  src/core/face-recognizer.cpp
  src/core/face-database.cpp
  src/core/identity-manager.cpp
  src/core/image-utils.cpp
  src/core/himitsukao-service.cpp
)
target_include_directories(himitsukao_core PUBLIC src/core)
target_compile_features(himitsukao_core PUBLIC cxx_std_17)
target_link_libraries(
  himitsukao_core
  PUBLIC opencv_core opencv_imgproc opencv_imgcodecs opencv_dnn opencv_objdetect nlohmann_json::nlohmann_json
)
set_target_properties(himitsukao_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs himitsukao_core)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp src/himitsukao-filter.cpp src/himitsukao-filter.h src/plugin-service.cpp src/plugin-service.h)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(HIMITSUKAO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
