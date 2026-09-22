cmake_minimum_required(VERSION 3.20)

project(port_os
    VERSION 2.0.0
    DESCRIPTION "Port OS - AI-native operating environment (Super-Nova 2.0)"
    LANGUAGES C CXX
)

# ---- C++23 requirement ----
# Requires: GCC 13+ (MSYS2 MinGW64), LLVM-MinGW Clang 17+, or MSVC 2022 17.8+
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Platform detection ----
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif()

# ---- Compiler warnings ----
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ---- Include paths ----
include_directories(${PROJECT_SOURCE_DIR}/include)

# ============================================================
# LAYER 1 — Core Services
# ============================================================
add_library(port_core STATIC
    src/core/event_bus.cpp
    src/core/logger.cpp
    src/core/config_engine.cpp
    src/core/metrics.cpp
    src/core/service_manager.cpp
)
target_include_directories(port_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
if(NOT MSVC)
    target_link_libraries(port_core PUBLIC pthread)
endif()

# ============================================================
# LAYER 2 — Security + Storage
# ============================================================
add_library(port_security STATIC
    src/security/sandbox.cpp
    src/security/audit_log.cpp
)
target_include_directories(port_security PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(port_security PUBLIC port_core)

add_library(port_storage STATIC
    src/storage/session_store.cpp
)
target_include_directories(port_storage PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(port_storage PUBLIC port_core)

# ============================================================
# LAYER 3 — AI Layer
# ============================================================
add_library(port_ai STATIC
    src/ai/gemini_provider.cpp
    src/ai/multi_provider.cpp
    src/ai/context_engine.cpp
    src/ai/tool_executor.cpp
    src/ai/prompt_runtime.cpp
)
target_include_directories(port_ai PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(port_ai PUBLIC port_core)
if(WIN32)
    target_link_libraries(port_ai PUBLIC wininet)
endif()

# ============================================================
# LAYER 4 — Kernel
# ============================================================
add_library(port_kernel STATIC
    src/kernel/ai_kernel.cpp
    src/kernel/command_router.cpp
    src/kernel/foundation_driver.cpp
    src/kernel/runtime_ai_adapter.cpp
)
target_include_directories(port_kernel PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(port_kernel PUBLIC port_core port_security port_storage port_ai)
if(WIN32)
    target_link_libraries(port_kernel PUBLIC wininet urlmon)
endif()

# ============================================================
# EXECUTABLE — Desktop (Windows GUI)
# ============================================================
if(WIN32)
    add_executable(port-desktop WIN32
        src/desktop/main.cpp
    )
    target_include_directories(port-desktop PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(port-desktop PRIVATE port_kernel gdiplus)
    if(NOT MSVC)
        target_link_options(port-desktop PRIVATE -mwindows)
    endif()
endif()

# ============================================================
# TESTS — Individual test executables (CTest)
# ============================================================
enable_testing()

function(add_port_test NAME SRC)
    add_executable(${NAME} ${SRC})
    target_include_directories(${NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
    )
    target_link_libraries(${NAME} PRIVATE port_kernel port_core port_security)
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

add_port_test(test_command_router  tests/test_command_router.cpp)
add_port_test(test_sandbox         tests/test_sandbox.cpp)
add_port_test(test_ai_kernel       tests/test_ai_kernel.cpp)
add_port_test(test_event_bus       tests/test_event_bus.cpp)
add_port_test(test_config          tests/test_config.cpp)
add_port_test(test_session_store   tests/test_session_store.cpp)
add_port_test(test_ai_runtime      tests/test_ai_runtime.cpp)
