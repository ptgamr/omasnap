cmake_minimum_required(VERSION 3.24)

# Independent entry point. Read the version, but never include/configure the
# root build: Studio needs no capture protocols, layer-shell, or recorder.
get_filename_component(OMASNAP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${OMASNAP_SOURCE_DIR}/CMakeLists.txt")
file(STRINGS "${OMASNAP_SOURCE_DIR}/CMakeLists.txt" OMASNAP_PROJECT_LINE
  REGEX "^project\\(omasnap VERSION [0-9.]+ ")
if(NOT OMASNAP_PROJECT_LINE MATCHES "VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)")
  message(FATAL_ERROR "Could not read the Omasnap version from the root CMakeLists.txt")
endif()
project(omasnap-studio VERSION "${CMAKE_MATCH_1}" LANGUAGES CXX)
include(GNUInstallDirs)
include(CTest)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS
  Core Gui Widgets Concurrent Multimedia MultimediaWidgets OpenGLWidgets)

# Explicit list intentionally mirrors the root Studio target. Keeping this
# entry point independent means the existing build does not need to change.
set(STUDIO_SOURCES
  studio-project.cpp studio-project.hpp
  studio-composition.cpp studio-composition.hpp
  studio-playback.cpp studio-playback.hpp
  studio-theme.cpp studio-theme.hpp
  studio-video.cpp studio-video.hpp
  studio.cpp studio.hpp
  studio-scenes.cpp studio-transitions.cpp
  studio-export.cpp studio-export.hpp
  studio-preview.cpp studio-preview.hpp
  zoom-track.cpp zoom-track.hpp
  overlay-chrome.cpp overlay-chrome.hpp
)
list(TRANSFORM STUDIO_SOURCES PREPEND "${OMASNAP_SOURCE_DIR}/src/")
add_library(studio-editor STATIC ${STUDIO_SOURCES})
target_include_directories(studio-editor PUBLIC "${OMASNAP_SOURCE_DIR}/src")
target_compile_features(studio-editor PUBLIC cxx_std_23)
target_compile_options(studio-editor PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(studio-editor PUBLIC
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent
  Qt6::Multimedia Qt6::MultimediaWidgets Qt6::OpenGLWidgets)

qt_add_executable(omasnap-studio "${OMASNAP_SOURCE_DIR}/src/studio-main.cpp")
target_compile_options(omasnap-studio PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(omasnap-studio PRIVATE OMASNAP_VERSION="${PROJECT_VERSION}")
target_link_libraries(omasnap-studio PRIVATE studio-editor)
# Preserve the selected, side-by-side Qt prefix when installing locally too.
# This is not a relocatable bundle: that Qt installation must remain in place.
set_target_properties(omasnap-studio PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

if(BUILD_TESTING)
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
  set(STUDIO_TEST_SOURCES
    studio-smoke.cpp
    studio-background-ui-smoke.cpp studio-background-ui-smoke.hpp
    studio-music-ui-smoke.cpp studio-music-ui-smoke.hpp
    studio-audio-ui-smoke.cpp studio-audio-ui-smoke.hpp
    studio-project-smoke.cpp studio-project-smoke.hpp
    studio-composition-smoke.cpp studio-composition-smoke.hpp
    studio-playback-smoke.cpp studio-playback-smoke.hpp
    studio-cuts-ui-smoke.cpp studio-cuts-ui-smoke.hpp
    studio-scenes-ui-smoke.cpp studio-scenes-ui-smoke.hpp
    studio-transitions-ui-smoke.cpp studio-transitions-ui-smoke.hpp
    studio-theme-smoke.cpp studio-theme-smoke.hpp
    zoom-track-smoke.cpp zoom-track-smoke.hpp
  )
  list(TRANSFORM STUDIO_TEST_SOURCES PREPEND "${OMASNAP_SOURCE_DIR}/tests/")
  qt_add_executable(omasnap-studio-smoke ${STUDIO_TEST_SOURCES})
  target_compile_options(omasnap-studio-smoke PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(omasnap-studio-smoke PRIVATE studio-editor Qt6::Test)
  add_test(NAME omasnap-studio-smoke COMMAND omasnap-studio-smoke)
  set_tests_properties(omasnap-studio-smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=generic")
endif()

install(TARGETS omasnap-studio RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${OMASNAP_SOURCE_DIR}/omasnap-studio.desktop"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
