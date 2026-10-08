cmake_minimum_required(VERSION 3.16)
project(snake3d LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Статическая стандартная библиотека: готовый .exe запускается на любом
# Windows-компьютере без установки Visual C++ Redistributable
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# raylib скачивается и собирается автоматически при первой конфигурации
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(raylib)

add_executable(snake3d src/main.cpp src/game.cpp src/render.cpp src/meshgen.cpp)
target_link_libraries(snake3d PRIVATE raylib)
if(MSVC)
  target_compile_options(snake3d PRIVATE /utf-8)  # русские строки в исходниках
  # без чёрного окна консоли рядом с игрой
  target_link_options(snake3d PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()
