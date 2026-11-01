# Build options for individual vcpkg ports, shared by our overlay triplets.

if(PORT STREQUAL "tracy")
    # The profiler client listens on localhost only and doesn't broadcast: no firewall prompt
    # on dev machines, nothing exposed on the network.
    set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DTRACY_ONLY_LOCALHOST=ON -DTRACY_NO_BROADCAST=ON)
endif()
