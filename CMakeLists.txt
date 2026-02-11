cmake_minimum_required(VERSION 3.10)
project(low_latency_layer
    VERSION 0.01
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

# Copy layer into out/
set(OUTPUT_DIR "${CMAKE_BINARY_DIR}/out")
add_custom_command(TARGET ${LIBRARY_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_DIR}"
)

add_custom_command(TARGET ${LIBRARY_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:${LIBRARY_NAME}>"
	"${OUTPUT_DIR}/"
)

add_custom_command(TARGET ${LIBRARY_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/low_latency_layer.json"
	"${OUTPUT_DIR}/"
)

set(SANITIZE_FLAGS
  -fsanitize=address,undefined,leak
  -fno-omit-frame-pointer
  -fno-optimize-sibling-calls
  -fno-sanitize-recover=all
  -fsanitize-address-use-after-scope
)

target_compile_options(${LIBRARY_NAME} PRIVATE
    #${SANITIZE_FLAGS}
    -g3
    -O1
    -D_GLIBCXX_ASSERTIONS
)

target_link_options(${LIBRARY_NAME} PRIVATE 
    #${SANITIZE_FLAGS}
)
