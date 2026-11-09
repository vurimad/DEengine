#include "app/framework.h"
#include "app/viewer.h"
#include "editor/editor.h" // dev builds only

#include <cstdlib>

// The process's entry point: the command line, the application, and the framework that runs it.
// Flags: app/framework.h (the engine's) and the application's own header. --editor (dev builds) runs the editor
// instead of the viewer.
int main(int argc, char** argv) {
    de::app::FrameworkArgs args;
    if (!de::app::parse_args(argc, argv, args)) {
        return EXIT_FAILURE;
    }
#if DE_BUILD_DEV
    if (de::app::has_flag(args, "--editor")) {
        de::editor::Editor editor; // engine/editor/editor.h
        return de::app::run_framework(editor, args);
    }
#endif
    de::app::Viewer viewer;
    return de::app::run_framework(viewer, args);
}
