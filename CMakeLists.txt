cmake_minimum_required(VERSION 3.20)
project(kalm VERSION 0.4.0 LANGUAGES CXX)

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
    src/huawei.cpp
    src/huawei.hpp
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
install(CODE "
    set(_kalm_exec \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/kalm\")
    file(READ \"${CMAKE_CURRENT_SOURCE_DIR}/io.github.kalm.desktop.in\" _desktop_in)
    string(REPLACE \"@CMAKE_INSTALL_FULL_BINDIR@/kalm\" \"\${_kalm_exec}\" _desktop_out \"\${_desktop_in}\")
    set(_apps_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/applications\")
    file(MAKE_DIRECTORY \"\${_apps_dir}\")
    file(WRITE \"\${_apps_dir}/io.github.kalm.desktop\" \"\${_desktop_out}\")
    message(STATUS \"Installing: \${_apps_dir}/io.github.kalm.desktop (Exec=\${_kalm_exec})\")
")
install(DIRECTORY translations/ DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/kalm/translations)

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_executable(test_kalm tests/test_kalm.cpp resources.qrc)
    target_link_libraries(test_kalm PRIVATE kalm_core)
    add_test(NAME test_kalm COMMAND test_kalm)
endif()
