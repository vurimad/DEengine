find_package(imgui CONFIG REQUIRED)

# The editor (deengine --editor, dev builds): panels over the running engine, and the commands every change goes
# through. The ship build doesn't call it, so none of it links in.
add_library(de_editor STATIC
    asset_browser.cpp
    asset_browser.h
    cell_map_panel.cpp
    cell_map_panel.h
    cell_saver.cpp
    cell_saver.h
    cell_source.cpp
    cell_source.h
    commands.cpp
    commands.h
    editor.cpp
    editor.h
    gizmo.cpp
    gizmo.h
    hierarchy_panel.cpp
    hierarchy_panel.h
    icons.cpp
    icons.h
    navigation.cpp
    navigation.h
    outline.cpp
    outline.h
    properties_panel.cpp
    properties_panel.h
    selection.h
    source_files.cpp
    source_files.h
    theme.cpp
    theme.h
    widgets.cpp
    widgets.h)
add_library(de::editor ALIAS de_editor)

target_link_libraries(de_editor
    PUBLIC de::app de::asset de::core de::platform de::world de::compile_options imgui::imgui
    PRIVATE de::physics de::render_scene)
