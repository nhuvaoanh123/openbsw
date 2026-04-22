add_library(rtScheduling STATIC
    src/Registry.cpp
    src/Json.cpp)

target_include_directories(rtScheduling PUBLIC include)

target_compile_features(rtScheduling PUBLIC cxx_std_17)

# Deliberate: this library depends only on the C++17 standard library.
# AsyncBridge.h (header-only) couples to <async/Async.h> when included by
# an application translation unit, but the library itself has no openbsw
# dependencies, which is what keeps unit tests hostable without the
# async/RTOS stack.

if (BUILD_EXECUTABLE STREQUAL "unitTest")

    add_subdirectory(test)

endif ()
