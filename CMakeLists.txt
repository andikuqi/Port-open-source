cmake_minimum_required(VERSION 3.20)

project(port_os
    VERSION 0.1.0
    DESCRIPTION "Port OS - AI-native operating environment powered by Super-Nova 1.0"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- fox_kernel (cross-platform) ----
add_library(fox_kernel
    src/kernel/ai_kernel.cpp
    src/kernel/ai_client.cpp
    src/kernel/command_router.cpp
    src/kernel/foundation_driver.cpp
    src/kernel/sandbox.cpp
)

target_include_directories(fox_kernel
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(fox_kernel PUBLIC cxx_std_11)

if(WIN32)
    target_link_libraries(fox_kernel PUBLIC wininet)
endif()

# ---- port-desktop (Windows GUI) ----
if(WIN32)
    add_executable(port-desktop WIN32
        src/desktop/main.cpp
    )
    target_link_libraries(port-desktop PRIVATE fox_kernel gdiplus)
    target_link_options(port-desktop PRIVATE -mwindows)
endif()

# ---- port-tests ----
add_executable(port-tests
    src/tests/main.cpp
)
target_link_libraries(port-tests PRIVATE fox_kernel)

# ---- Compiler warnings ----
if(MSVC)
    foreach(tgt fox_kernel port-tests)
        target_compile_options(${tgt} PRIVATE /W4)
    endforeach()
    if(WIN32)
        target_compile_options(port-desktop PRIVATE /W4)
    endif()
else()
    foreach(tgt fox_kernel port-tests)
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
    if(WIN32)
        target_compile_options(port-desktop PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
