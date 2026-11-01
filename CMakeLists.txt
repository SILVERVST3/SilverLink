cmake_minimum_required(VERSION 3.20)
project(SilverLink VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "SilverLink e um projeto Windows (x64).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Static C/C++ runtime: no VC++ redistributable needed (the ASIO DLL is loaded into REAPER)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)  # FetchContent_Populate for header/source-only deps
endif()

# ---------------------------------------------------------------------------
# ASIO SDK (Steinberg, GPLv3 option). Pass -DASIOSDK_DIR=<folder containing common/asio.h>
# or let CMake fetch a copy from GitHub.
# ---------------------------------------------------------------------------
set(ASIOSDK_DIR "" CACHE PATH "Pasta do ASIO SDK (contendo common/asio.h)")
if(NOT ASIOSDK_DIR OR NOT EXISTS "${ASIOSDK_DIR}/common/asio.h")
    FetchContent_Declare(asiosdk
        GIT_REPOSITORY https://github.com/jcelerier/asio-2.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE)
    FetchContent_GetProperties(asiosdk)
    if(NOT asiosdk_POPULATED)
        FetchContent_Populate(asiosdk)
    endif()
    set(ASIOSDK_DIR "${asiosdk_SOURCE_DIR}")
endif()
message(STATUS "ASIO SDK: ${ASIOSDK_DIR}")

# ---------------------------------------------------------------------------
# Dear ImGui (MIT)
# ---------------------------------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

function(slink_warnings tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W3 /utf-8 /permissive- /EHsc)
        target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers)
        target_compile_definitions(${tgt} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        target_link_options(${tgt} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endfunction()

set(COMMON_SRC common/Net.cpp)
set(RES_SRC res/SilverLink.rc)

# ---------------------------------------------------------------------------
# Engine (tray app)
# ---------------------------------------------------------------------------
add_executable(SilverLink WIN32 engine/main.cpp engine/Engine.cpp ${COMMON_SRC} ${RES_SRC})
slink_warnings(SilverLink)
target_link_libraries(SilverLink PRIVATE ws2_32 iphlpapi winmm avrt shell32 advapi32 user32)
if(NOT MSVC)
    target_link_options(SilverLink PRIVATE -municode)
endif()

# ---------------------------------------------------------------------------
# ASIO driver (COM in-proc server)
# ---------------------------------------------------------------------------
add_library(SilverLink_ASIO SHARED asio/SilverAsio.cpp asio/SilverAsio.def)
slink_warnings(SilverLink_ASIO)
set_target_properties(SilverLink_ASIO PROPERTIES PREFIX "" OUTPUT_NAME "SilverLink_ASIO")
target_include_directories(SilverLink_ASIO PRIVATE "${ASIOSDK_DIR}/common")
target_link_libraries(SilverLink_ASIO PRIVATE ole32 uuid avrt advapi32 shell32 user32)

# ---------------------------------------------------------------------------
# Controller (routing panel, Dear ImGui + Direct3D 11)
# ---------------------------------------------------------------------------
add_executable(SilverLink_Controller WIN32
    controller/main.cpp controller/Ui.cpp controller/Discovery.cpp ${COMMON_SRC} ${RES_SRC}
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
slink_warnings(SilverLink_Controller)
target_include_directories(SilverLink_Controller PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(SilverLink_Controller PRIVATE d3d11 dxgi d3dcompiler dwmapi ws2_32 iphlpapi user32 gdi32 shell32)
if(NOT MSVC)
    target_link_options(SilverLink_Controller PRIVATE -municode)
endif()

install(TARGETS SilverLink SilverLink_Controller RUNTIME DESTINATION .)
install(TARGETS SilverLink_ASIO RUNTIME DESTINATION . LIBRARY DESTINATION .)
