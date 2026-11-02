find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_library(de_platform STATIC
    debug.h
    events.cpp
    events.h
    file.h
    imgui_events.h
    imgui_platform.cpp
    imgui_platform.h
    input.cpp
    input.h
    platform.cpp
    platform.h
    thread.cpp
    thread.h
    time.cpp
    time.h
    vfs.cpp
    vfs.h
    window.cpp
    window.h)
add_library(de::platform ALIAS de_platform)

# OS-specific code lives in per-OS files picked here, so no source needs a platform #ifdef.
if(WIN32)
    target_sources(de_platform PRIVATE debug_win32.cpp file_win32.cpp thread_win32.cpp)
else()
    target_sources(de_platform PRIVATE debug_linux.cpp file_linux.cpp thread_linux.cpp)
endif()

target_link_libraries(de_platform
    PUBLIC de::core de::compile_options
    PRIVATE SDL3::SDL3 imgui::imgui)
