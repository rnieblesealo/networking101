# Simple CMake
# Finds raylib and sets up SDL server project
# Also links an include/ dir if needed

cmake_minimum_required(VERSION 3.10)

project(sdlserver)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(raylib REQUIRED)

add_executable(${PROJECT_NAME} 
  src/main.c
  src/server.c
)

if(APPLE)
    target_link_libraries(${PROJECT_NAME} raylib "-framework CoreVideo" "-framework IOKit" "-framework Cocoa" "-framework GLUT" "-framework OpenGL")
else()
    target_link_libraries(${PROJECT_NAME} raylib)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE 
  include
)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR})
