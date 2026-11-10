find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_library(de_platform STATIC
    cpu.cpp
    cpu.h
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
    process.cpp
    process.h
    project.cpp
    project.h
    random.h
    socket.h
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
    target_sources(de_platform PRIVATE cpu_win32.cpp debug_win32.cpp file_win32.cpp random_win32.cpp socket_win32.cpp
        thread_win32.cpp)
    target_link_libraries(de_platform PRIVATE bcrypt shell32 user32 ws2_32)
else()
    target_sources(de_platform PRIVATE cpu_linux.cpp debug_linux.cpp file_linux.cpp random_linux.cpp socket_linux.cpp
        thread_linux.cpp)
endif()

target_link_libraries(de_platform
    PUBLIC de::core de::compile_options
    PRIVATE SDL3::SDL3 imgui::imgui)
