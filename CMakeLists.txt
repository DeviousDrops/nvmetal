cmake_minimum_required(VERSION 3.20)
project(nvmetal VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# One flag set for every build type: the brief requires -O2 -g everywhere, and
# keeping optimisation on in sanitizer builds exercises the same code paths.
set(CMAKE_C_FLAGS_RELWITHDEBINFO "")
set(CMAKE_C_FLAGS_RELEASE "")
set(CMAKE_C_FLAGS_DEBUG "")
add_compile_options(-Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE)

set(NVMETAL_SANITIZE "" CACHE STRING "Sanitizers: empty, 'address,undefined' or 'thread'")
if(NVMETAL_SANITIZE)
  add_compile_options(-fsanitize=${NVMETAL_SANITIZE} -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${NVMETAL_SANITIZE})
endif()

find_package(Threads REQUIRED)

add_library(nvmetal STATIC
  src/util.c
  src/vfio.c
  src/dma.c
  src/prp.c
  src/qpair.c
  src/ctrl.c
  src/status.c
)
target_include_directories(nvmetal PUBLIC include PRIVATE src)
target_link_libraries(nvmetal PUBLIC Threads::Threads)

enable_testing()
add_subdirectory(tools)
add_subdirectory(tests)
