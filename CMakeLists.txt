cmake_minimum_required(VERSION 3.10)
project(low_latency_layer
    VERSION 0.02
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE CXX_SOURCES src/*.cc)

set(LIBRARY_NAME VkLayer_NJ3AHXAC_LowLatency)
add_library(${LIBRARY_NAME} SHARED
    ${CXX_SOURCES}
)

find_package(Vulkan REQUIRED)
target_link_libraries(${LIBRARY_NAME}
    ${Vulkan_LIBRARIES}
)

include(GNUInstallDirs)
# Prefix an absolute directory to our layer's json so the loader knows where the shared library is.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/low_latency_layer.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/low_latency_layer.json"
    @ONLY
)
install(TARGETS ${LIBRARY_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/low_latency_layer.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d/"
)

target_compile_options(${LIBRARY_NAME} PRIVATE
    # Inherited from Mesa.
    -g
    -O2
    -D_GLIBCXX_ASSERTIONS
    -D__STDC_CONSTANT_MACROS
    -D__STDC_FORMAT_MACROS
    -D__STDC_LIMIT_MACROS

    -Wall
    -Wextra
    -Wno-sign-conversion
    -Wno-missing-field-initializers

    -Wundef
    -Wconversion
    -Wdouble-promotion
)
