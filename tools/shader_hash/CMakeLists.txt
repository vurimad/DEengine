# Runs during the build, before the engine's shader library compiles (engine/render/shader/).
add_executable(de_shader_hash main.cpp)
target_link_libraries(de_shader_hash PRIVATE de::asset de::core de::platform de::compile_options)
