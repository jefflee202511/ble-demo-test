cmake_minimum_required(VERSION 3.16)
project(ble_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# NativeBLE source location: clone into native/NativeBLE or pass -DNATIVEBLE_DIR=...
set(NATIVEBLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/NativeBLE" CACHE PATH "NativeBLE source dir")

add_subdirectory(${NATIVEBLE_DIR} ${CMAKE_BINARY_DIR}/nativeble)

# Locate NativeBleController.h automatically
file(GLOB_RECURSE NATIVEBLE_HEADER "${NATIVEBLE_DIR}/*NativeBleController.h")
if(NOT NATIVEBLE_HEADER)
  message(FATAL_ERROR "NativeBleController.h not found under ${NATIVEBLE_DIR}. Check the NativeBLE source location.")
endif()
list(GET NATIVEBLE_HEADER 0 NATIVEBLE_HEADER)
get_filename_component(NATIVEBLE_INCLUDE_DIR "${NATIVEBLE_HEADER}" DIRECTORY)
message(STATUS "NativeBLE header dir: ${NATIVEBLE_INCLUDE_DIR}")

# Detect the NativeBLE library target name (override: -DNATIVEBLE_TARGET=name)
set(NATIVEBLE_TARGET "" CACHE STRING "NativeBLE CMake target")
if(NOT NATIVEBLE_TARGET)
  foreach(cand nativeble-static nativeble_static NativeBLE-static nativeble NativeBLE)
    if(TARGET ${cand})
      set(NATIVEBLE_TARGET ${cand})
      break()
    endif()
  endforeach()
endif()
if(NOT NATIVEBLE_TARGET)
  message(FATAL_ERROR "NativeBLE target not found. Pass the add_library name from NativeBLE/CMakeLists.txt via -DNATIVEBLE_TARGET=")
endif()
message(STATUS "NativeBLE target: ${NATIVEBLE_TARGET}")

# FFI check: ble_ffi.json (written by hand) must match ble_bridge.h.
# check_ffi.js compares the names and generates ffi_check.cpp, whose
# static_asserts make the compiler compare the types. A mismatch fails the build.
find_program(NODE_EXECUTABLE node)
if(NOT NODE_EXECUTABLE)
  message(FATAL_ERROR "node was not found. It is needed to run check_ffi.js.")
endif()

set(FFI_JSON      ${CMAKE_CURRENT_SOURCE_DIR}/ble_ffi.json)
set(FFI_HEADER    ${CMAKE_CURRENT_SOURCE_DIR}/ble_bridge.h)
set(FFI_SCRIPT    ${CMAKE_CURRENT_SOURCE_DIR}/check_ffi.js)
set(FFI_CHECK_CPP ${CMAKE_CURRENT_BINARY_DIR}/ffi_check.cpp)

add_custom_command(
  OUTPUT ${FFI_CHECK_CPP}
  COMMAND ${NODE_EXECUTABLE} ${FFI_SCRIPT} ${FFI_JSON} ${FFI_HEADER} ${FFI_CHECK_CPP}
  DEPENDS ${FFI_JSON} ${FFI_HEADER} ${FFI_SCRIPT}
  COMMENT "Checking ble_ffi.json against ble_bridge.h"
  VERBATIM)

add_library(ble_bridge SHARED ble_bridge.cpp ${FFI_CHECK_CPP})
target_include_directories(ble_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(ble_bridge PRIVATE ${NATIVEBLE_INCLUDE_DIR})
target_link_libraries(ble_bridge PRIVATE ${NATIVEBLE_TARGET} ${CMAKE_DL_LIBS})

# Treat sources as UTF-8 under MSVC
if(MSVC)
  target_compile_options(ble_bridge PRIVATE /utf-8)
  # /EHa: let catch(...) also catch Windows structured exceptions (e.g. access violations)
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  target_compile_options(ble_bridge PRIVATE /EHa)
endif()

# Put outputs directly under build/ (avoid the MSVC Release/ subfolder)
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
  set_target_properties(ble_bridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR})
endforeach()
