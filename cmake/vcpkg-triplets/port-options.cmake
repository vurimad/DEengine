# Build options for individual vcpkg ports, shared by our overlay triplets.

if(PORT STREQUAL "tracy")
    # The profiler client listens on localhost only and doesn't broadcast: no firewall prompt
    # on dev machines, nothing exposed on the network. No crash handler either (the manifest
    # already turns the port's default feature off; this keeps it off if that ever changes):
    # crashes and fatal errors belong to the engine's fatal handler and the debugger.
    # No call stacks: our zones are explicit, and on Windows Tracy's startup symbol scan
    # (SymInitialize over every loaded module) fails noisily and costs startup time.
    set(VCPKG_CMAKE_CONFIGURE_OPTIONS
        -DTRACY_ONLY_LOCALHOST=ON -DTRACY_NO_BROADCAST=ON -DTRACY_NO_CRASH_HANDLER=ON -DTRACY_NO_CALLSTACK=ON)
endif()
