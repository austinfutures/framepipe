cmake_minimum_required(VERSION 3.16)
project(framepipe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  # Optimised but with symbols, so perf can name functions.
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers make perf/bpftrace stack traces reliable without DWARF unwinding.
add_compile_options(-Wall -Wextra -fno-omit-frame-pointer)

find_package(Threads REQUIRED)

# ONNX Runtime: prebuilt release unpacked by scripts/setup_deps.sh into third_party/.
set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH "ONNX Runtime install dir")
find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h
  PATHS "${ONNXRUNTIME_ROOT}/include" "${ONNXRUNTIME_ROOT}/include/onnxruntime" NO_DEFAULT_PATH)
find_library(ORT_LIBRARY onnxruntime PATHS "${ONNXRUNTIME_ROOT}/lib" NO_DEFAULT_PATH)
if(NOT ORT_INCLUDE_DIR OR NOT ORT_LIBRARY)
  message(FATAL_ERROR "ONNX Runtime not found under ${ONNXRUNTIME_ROOT}. Run scripts/setup_deps.sh first.")
endif()
get_filename_component(ORT_LIB_DIR "${ORT_LIBRARY}" DIRECTORY)

# fp_producer: frame source (no ONNX dependency)
add_executable(fp_producer src/producer.cpp src/v4l2_source.cpp)
target_include_directories(fp_producer PRIVATE src)
target_link_libraries(fp_producer PRIVATE Threads::Threads rt)

# fp_consumer: shm consumer + ONNX Runtime inference
add_executable(fp_consumer src/consumer.cpp src/policy_model.cpp)
target_include_directories(fp_consumer PRIVATE src ${ORT_INCLUDE_DIR})
target_link_libraries(fp_consumer PRIVATE ${ORT_LIBRARY} Threads::Threads rt)
# Absolute rpath: the binary finds libonnxruntime.so even under sudo (which strips LD_LIBRARY_PATH).
set_target_properties(fp_consumer PROPERTIES BUILD_RPATH "${ORT_LIB_DIR}")

# fp_ring_test: SPSC ring correctness/throughput
add_executable(fp_ring_test src/ring_test.cpp)
target_include_directories(fp_ring_test PRIVATE src)
target_link_libraries(fp_ring_test PRIVATE Threads::Threads)
