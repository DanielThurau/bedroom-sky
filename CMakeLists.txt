cmake_minimum_required(VERSION 3.20)
project(bedroom-sky LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# On macOS, OpenGL is a framework
find_package(OpenGL REQUIRED)

add_executable(bedroom-sky
    src/main.cpp
    src/renderer_init.cpp
    src/renderer_scene.cpp
    src/renderer_aircraft.cpp
    src/renderer_ui.cpp
    src/flight_tracker.cpp
    src/terrain.cpp
    src/buildings.cpp
    src/streets.cpp
    src/transit_predictor.cpp
    src/glad.c
)

target_include_directories(bedroom-sky PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(bedroom-sky PRIVATE
    glfw
    glm::glm
    OpenGL::GL
    ${CURL_LIBRARIES}
    Threads::Threads
)

# macOS frameworks
if(APPLE)
    target_link_libraries(bedroom-sky PRIVATE "-framework Cocoa" "-framework IOKit")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(config-tests tests/config_tests.cpp)
    target_include_directories(config-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME config-tests COMMAND config-tests)
    add_test(NAME demo-ignores-private-environment
        COMMAND ${CMAKE_COMMAND} -E env BEDROOM_SKY_LATITUDE=invalid
                $<TARGET_FILE:bedroom-sky> --demo --check-config)
    set_tests_properties(demo-ignores-private-environment PROPERTIES
        PASS_REGULAR_EXPRESSION "Configuration valid")
    add_test(NAME normal-rejects-invalid-environment
        COMMAND ${CMAKE_COMMAND} -E env BEDROOM_SKY_LATITUDE=invalid
                $<TARGET_FILE:bedroom-sky> --check-config)
    set_tests_properties(normal-rejects-invalid-environment PROPERTIES WILL_FAIL TRUE)
endif()
