cmake_minimum_required(VERSION 3.28)
project(OzzSoundBoard VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# SPA headers rely on GNU compound literals.
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(OZZSB_BUILD_GUI "Build the Qt6 GUI" ON)
option(OZZSB_BUILD_TESTS "Build unit tests" ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(Threads REQUIRED)

set(OZZSB_WARNINGS -Wall -Wextra -Wpedantic -Wno-missing-field-initializers -Wno-pedantic)

add_library(ozzsb_core STATIC
  src/core/Paths.cpp
  src/core/SoundLibrary.cpp
  src/audio/Clip.cpp
  src/audio/InputSelection.cpp
  src/audio/Mixer.cpp
  src/ipc/Protocol.cpp
  src/ipc/Socket.cpp
)
target_include_directories(ozzsb_core PUBLIC src)
target_compile_options(ozzsb_core PRIVATE ${OZZSB_WARNINGS})
target_link_libraries(ozzsb_core
  PUBLIC nlohmann_json::nlohmann_json Threads::Threads
  PRIVATE PkgConfig::SNDFILE PkgConfig::SAMPLERATE)

add_library(ozzsb_engine STATIC
  src/audio/Engine.cpp
  src/daemon/Daemon.cpp
)
target_include_directories(ozzsb_engine PUBLIC src)
target_compile_options(ozzsb_engine PRIVATE ${OZZSB_WARNINGS})
target_link_libraries(ozzsb_engine PUBLIC ozzsb_core PkgConfig::PIPEWIRE)

add_executable(ozzsoundboard src/cli/main.cpp)
target_compile_options(ozzsoundboard PRIVATE ${OZZSB_WARNINGS})
target_link_libraries(ozzsoundboard PRIVATE ozzsb_engine)

install(TARGETS ozzsoundboard RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(dist/ozzsoundboard.service.in ${CMAKE_CURRENT_BINARY_DIR}/ozzsoundboard.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ozzsoundboard.service
  DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user)

if(OZZSB_BUILD_GUI)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
  set(CMAKE_AUTOMOC ON)
  add_executable(ozzsoundboard-gui gui/main.cpp gui/MainWindow.cpp gui/MainWindow.hpp)
  target_compile_options(ozzsoundboard-gui PRIVATE ${OZZSB_WARNINGS})
  target_link_libraries(ozzsoundboard-gui PRIVATE ozzsb_core Qt6::Widgets)
  install(TARGETS ozzsoundboard-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  configure_file(dist/ozzsoundboard-gui.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/ozzsoundboard-gui.desktop @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ozzsoundboard-gui.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()

if(OZZSB_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(ozzsb_tests
    tests/SpscRingTest.cpp
    tests/MixerTest.cpp
    tests/ProtocolTest.cpp
    tests/SoundLibraryTest.cpp
    tests/ClipTest.cpp
    tests/SocketTest.cpp
    tests/InputSelectionTest.cpp
  )
  target_compile_options(ozzsb_tests PRIVATE ${OZZSB_WARNINGS})
  target_link_libraries(ozzsb_tests PRIVATE ozzsb_core GTest::gtest_main PkgConfig::SNDFILE)
  include(GoogleTest)
  gtest_discover_tests(ozzsb_tests)
endif()
