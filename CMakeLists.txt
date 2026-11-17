cmake_minimum_required(VERSION 3.20)
project(kalm VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(ENABLE_SANITIZERS "Enable AddressSanitizer, LeakSanitizer, and UndefinedBehaviorSanitizer" OFF)
add_compile_options(-Wall -Wextra -Wpedantic)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,leak,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,leak,undefined)
endif()

include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Qml Quick DBus)

add_library(kalm_core STATIC
    src/i18n.cpp
    src/i18n.hpp
    src/probe.cpp
    src/probe.hpp
    src/protocol.cpp
    src/protocol.hpp
    src/transport.cpp
    src/transport.hpp
    src/registry.cpp
    src/registry.hpp
    src/session.cpp
    src/session.hpp
)
target_include_directories(kalm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kalm_core PUBLIC Qt6::Core Qt6::DBus)

add_executable(kalm
    src/main.cpp
    src/backend.cpp
    src/backend.hpp
    resources.qrc
)
target_link_libraries(kalm PRIVATE
    kalm_core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Qml
    Qt6::Quick
)

install(TARGETS kalm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES io.github.kalm.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(DIRECTORY translations/ DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/kalm/translations)

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_executable(test_kalm tests/test_kalm.cpp resources.qrc)
    target_link_libraries(test_kalm PRIVATE kalm_core)
    add_test(NAME test_kalm COMMAND test_kalm)
endif()
