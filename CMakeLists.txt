cmake_minimum_required(VERSION 3.25)

project(
  LedScribe
  VERSION 0.1.0
  DESCRIPTION "CH546 11x44 display writer"
  LANGUAGES CXX
)

include(FetchContent)
FetchContent_Declare(
  stb
  URL https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h
  DOWNLOAD_NO_EXTRACT TRUE
  DOWNLOAD_NAME stb_truetype.h
)
FetchContent_MakeAvailable(stb)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_library(
  ledscribe_core
  STATIC
  src/utils.cpp
  src/pack.cpp
  src/protocol.cpp
  src/device.cpp
  src/vendor_font.cpp
  src/truetype.cpp
  src/render.cpp
)
target_link_libraries(ledscribe_core PUBLIC PkgConfig::LIBUSB)
target_include_directories(ledscribe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(ledscribe_core SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_compile_features(ledscribe_core PUBLIC cxx_std_20)
set_target_properties(ledscribe_core PROPERTIES CXX_EXTENSIONS OFF)

add_executable(ledscribe src/main.cpp)
target_link_libraries(ledscribe PRIVATE ledscribe_core)
set_target_properties(ledscribe PROPERTIES CXX_EXTENSIONS OFF)

option(LEDSCRIBE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(ledscribe_enable_warnings target)
  target_compile_options(
    ${target}
    PRIVATE
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wextra>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wshadow>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wconversion>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wno-sign-conversion>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wpedantic>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wformat=2>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wnull-dereference>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wdouble-promotion>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wimplicit-fallthrough>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wvla>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wnon-virtual-dtor>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Woverloaded-virtual>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wold-style-cast>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wcast-qual>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wfloat-equal>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wundef>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wmissing-declarations>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wuseless-cast>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wduplicated-cond>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wduplicated-branches>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wlogical-op>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wstrict-aliasing=3>
  )
  if(LEDSCRIBE_WARNINGS_AS_ERRORS)
    target_compile_options(
      ${target} PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Werror>
    )
  endif()
endfunction()

ledscribe_enable_warnings(ledscribe_core)
ledscribe_enable_warnings(ledscribe)

include(CTest)
option(LEDSCRIBE_BUILD_GUI "Build the Qt 6 Widgets GUI" OFF)
if(LEDSCRIBE_BUILD_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  add_library(ledscribe_gui_components STATIC
    src/gui/project.cpp
    src/gui/pixel_preview.cpp
    src/gui/worker.cpp
    src/gui/main_window.cpp
  )
  target_link_libraries(ledscribe_gui_components PUBLIC ledscribe_core Qt6::Widgets)
  target_include_directories(ledscribe_gui_components PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/gui)
  target_compile_definitions(ledscribe_gui_components PUBLIC QT_NO_KEYWORDS)
  set_target_properties(ledscribe_gui_components PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
  ledscribe_enable_warnings(ledscribe_gui_components)
  add_executable(ledscribe-gui src/gui/main.cpp)
  target_link_libraries(ledscribe-gui PRIVATE ledscribe_gui_components)
  set_target_properties(ledscribe-gui PROPERTIES CXX_EXTENSIONS OFF)
  ledscribe_enable_warnings(ledscribe-gui)
  if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(ledscribe_gui_tests tests/gui_tests.cpp)
    target_link_libraries(ledscribe_gui_tests PRIVATE ledscribe_gui_components Qt6::Test)
    set_target_properties(ledscribe_gui_tests PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    ledscribe_enable_warnings(ledscribe_gui_tests)
    add_test(NAME ledscribe_gui_tests COMMAND ledscribe_gui_tests)
    set_tests_properties(ledscribe_gui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endif()
endif()

if(BUILD_TESTING)
  add_executable(ledscribe_tests tests/tests.cpp)
  target_link_libraries(ledscribe_tests PRIVATE ledscribe_core)
  set_target_properties(ledscribe_tests PROPERTIES CXX_EXTENSIONS OFF)
  ledscribe_enable_warnings(ledscribe_tests)
  add_test(NAME ledscribe_tests COMMAND ledscribe_tests)
  add_executable(ledscribe_font_tests tests/font_tests.cpp)
  target_link_libraries(ledscribe_font_tests PRIVATE ledscribe_core)
  set_target_properties(ledscribe_font_tests PROPERTIES CXX_EXTENSIONS OFF)
  ledscribe_enable_warnings(ledscribe_font_tests)
  add_test(NAME ledscribe_font_tests COMMAND ledscribe_font_tests)
  add_executable(ledscribe_render_tests tests/render_tests.cpp)
  target_link_libraries(ledscribe_render_tests PRIVATE ledscribe_core)
  set_target_properties(ledscribe_render_tests PROPERTIES CXX_EXTENSIONS OFF)
  ledscribe_enable_warnings(ledscribe_render_tests)
  add_test(NAME ledscribe_render_tests COMMAND ledscribe_render_tests)
  add_executable(ledscribe_device_tests tests/device_tests.cpp)
  target_link_libraries(ledscribe_device_tests PRIVATE ledscribe_core)
  set_target_properties(ledscribe_device_tests PROPERTIES CXX_EXTENSIONS OFF)
  ledscribe_enable_warnings(ledscribe_device_tests)
  add_test(NAME ledscribe_device_tests COMMAND ledscribe_device_tests)
  add_test(
    NAME ledscribe_cli_tests
    COMMAND
      ${CMAKE_COMMAND} -DLEDSCRIBE=$<TARGET_FILE:ledscribe>
      -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests -P
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake
  )
endif()


include(GNUInstallDirs)
install(TARGETS ledscribe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(LEDSCRIBE_BUILD_GUI)
  install(TARGETS ledscribe-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES udev/70-ledscribe.rules DESTINATION lib/udev/rules.d)
