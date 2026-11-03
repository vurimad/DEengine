#include "app/framework.h"
#include "app/viewer.h"

#include <cstdlib>

// The process's entry point: the command line, the application, and the framework that runs it.
// Flags: app/framework.h (the engine's) and the application's own header.
int main(int argc, char** argv) {
    de::app::FrameworkArgs args;
    if (!de::app::parse_args(argc, argv, args)) {
        return EXIT_FAILURE;
    }
    de::app::Viewer viewer;
    return de::app::run_framework(viewer, args);
}
