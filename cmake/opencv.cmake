# Builds a minimal static OpenCV (core, imgproc, dnn, objdetect and their dependencies) once, at configure time,
# into .deps/, then exposes it through find_package. The OpenCV build is a separate CMake invocation so that
# none of our compiler settings (warnings as errors, ...) leak into it.

include_guard(GLOBAL)

set(HIMITSUKAO_OPENCV_VERSION 4.10.0)
set(_ocv_root "${CMAKE_SOURCE_DIR}/.deps/opencv")
set(_ocv_archive "${_ocv_root}/opencv-${HIMITSUKAO_OPENCV_VERSION}.zip")
set(_ocv_source "${_ocv_root}/opencv-${HIMITSUKAO_OPENCV_VERSION}")
set(_ocv_build "${_ocv_root}/build-${CMAKE_SYSTEM_PROCESSOR}")
set(_ocv_install "${_ocv_root}/install-${CMAKE_SYSTEM_PROCESSOR}")

file(GLOB_RECURSE _ocv_config_files "${_ocv_install}/OpenCVConfig.cmake")

if(NOT _ocv_config_files)
  message(STATUS "HimitsuKao: building OpenCV ${HIMITSUKAO_OPENCV_VERSION} (one-time, takes a while)")

  if(NOT EXISTS "${_ocv_source}/CMakeLists.txt")
    if(NOT EXISTS "${_ocv_archive}")
      file(
        DOWNLOAD
        "https://github.com/opencv/opencv/archive/refs/tags/${HIMITSUKAO_OPENCV_VERSION}.zip"
        "${_ocv_archive}"
        SHOW_PROGRESS
        STATUS _ocv_dl
      )
      list(GET _ocv_dl 0 _ocv_dl_code)
      if(NOT _ocv_dl_code EQUAL 0)
        file(REMOVE "${_ocv_archive}")
        message(FATAL_ERROR "Could not download OpenCV: ${_ocv_dl}")
      endif()
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_ocv_archive}" DESTINATION "${_ocv_root}")
  endif()

  set(_ocv_modules core imgproc flann features2d calib3d dnn objdetect)
  list(JOIN _ocv_modules "," _ocv_build_list)

  set(
    _ocv_args
    -DBUILD_LIST=${_ocv_build_list}
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_WITH_STATIC_CRT=OFF
    -DBUILD_TESTS=OFF
    -DBUILD_PERF_TESTS=OFF
    -DBUILD_EXAMPLES=OFF
    -DBUILD_DOCS=OFF
    -DBUILD_APPS=OFF
    -DBUILD_JAVA=OFF
    -DBUILD_opencv_apps=OFF
    -DBUILD_opencv_java_bindings_generator=OFF
    -DBUILD_opencv_js=OFF
    -DBUILD_opencv_python2=OFF
    -DBUILD_opencv_python3=OFF
    -DBUILD_opencv_python_bindings_generator=OFF
    -DBUILD_PROTOBUF=ON
    -DWITH_PROTOBUF=ON
    -DBUILD_ZLIB=ON
    -DBUILD_ITT=OFF
    -DWITH_ITT=OFF
    -DWITH_IPP=OFF
    -DWITH_OPENCL=OFF
    -DWITH_CUDA=OFF
    -DWITH_FFMPEG=OFF
    -DWITH_QUIRC=OFF
    -DWITH_EIGEN=OFF
    -DWITH_LAPACK=OFF
    -DWITH_OPENEXR=OFF
    -DWITH_JPEG=OFF
    -DWITH_PNG=OFF
    -DWITH_TIFF=OFF
    -DWITH_WEBP=OFF
    -DWITH_JASPER=OFF
    -DWITH_OPENJPEG=OFF
    -DWITH_WIN32UI=OFF
    -DWITH_MSMF=OFF
    -DWITH_DSHOW=OFF
    -DWITH_OBSENSOR=OFF
    -DWITH_ADE=OFF
    -DCMAKE_INSTALL_PREFIX=${_ocv_install}
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  )

  set(_ocv_platform "")
  if(CMAKE_GENERATOR_PLATFORM)
    set(_ocv_platform -A ${CMAKE_GENERATOR_PLATFORM})
  endif()
  if(NOT CMAKE_CONFIGURATION_TYPES)
    list(APPEND _ocv_args -DCMAKE_BUILD_TYPE=Release)
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -S "${_ocv_source}" -B "${_ocv_build}" -G "${CMAKE_GENERATOR}" ${_ocv_platform}
      ${_ocv_args}
    RESULT_VARIABLE _ocv_result
  )
  if(NOT _ocv_result EQUAL 0)
    message(FATAL_ERROR "OpenCV configure failed")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_ocv_build}" --config Release --target install --parallel
    RESULT_VARIABLE _ocv_result
  )
  if(NOT _ocv_result EQUAL 0)
    message(FATAL_ERROR "OpenCV build failed")
  endif()

  file(GLOB_RECURSE _ocv_config_files "${_ocv_install}/OpenCVConfig.cmake")
endif()

list(GET _ocv_config_files 0 _ocv_config_file)
get_filename_component(OpenCV_DIR "${_ocv_config_file}" DIRECTORY)
set(OpenCV_STATIC ON)
find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc dnn objdetect NO_DEFAULT_PATH)

# Only the Release flavour of OpenCV is built; use it for every configuration (same /MD runtime).
foreach(_t opencv_core opencv_imgproc opencv_dnn opencv_objdetect opencv_calib3d opencv_features2d opencv_flann)
  if(TARGET ${_t})
    set_target_properties(
      ${_t}
      PROPERTIES
        MAP_IMPORTED_CONFIG_DEBUG Release
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        MAP_IMPORTED_CONFIG_MINSIZEREL Release
    )
  endif()
endforeach()
