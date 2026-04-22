add_executable(rtSchedulingTest
    src/FeasibilityTest.cpp
    src/RtaTest.cpp
    src/RegistryTest.cpp
    src/MonitoredRunnableTest.cpp
    src/JsonTest.cpp)

target_link_libraries(rtSchedulingTest PRIVATE rtScheduling gmock_main)

gtest_discover_tests(rtSchedulingTest PROPERTIES LABELS "rtSchedulingTest")
