cmake_minimum_required(VERSION 3.21)
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment version")
endif()
project(ivynote-qt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(IVY_BUILD_TESTS "Build unit and integration tests" ON)
option(IVY_BUILD_APP "Build the desktop application" ON)
# Shown in the app and sent as the User-Agent. Release CI passes
# -DIVY_VERSION from the git tag (v1.2.3 -> 1.2.3), so a tag is all a release
# needs; otherwise it follows project(VERSION).
if(NOT IVY_VERSION)
    set(IVY_VERSION "${PROJECT_VERSION}")
endif()
if(NOT IVY_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "IVY_VERSION must start with MAJOR.MINOR.PATCH, got '${IVY_VERSION}'")
endif()
set(IVY_VERSION_MAJOR ${CMAKE_MATCH_1})
set(IVY_VERSION_MINOR ${CMAKE_MATCH_2})
set(IVY_VERSION_PATCH ${CMAKE_MATCH_3})
set(IVY_VERSION_NUMERIC "${IVY_VERSION_MAJOR}.${IVY_VERSION_MINOR}.${IVY_VERSION_PATCH}")

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Sql Svg Concurrent)
find_package(Qt6Keychain REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(MD4C QUIET IMPORTED_TARGET md4c md4c-html)
if(MD4C_FOUND)
    set(IVY_MD4C_TARGET PkgConfig::MD4C)
else()
    find_package(md4c CONFIG REQUIRED)
    set(IVY_MD4C_TARGET md4c::md4c md4c::md4c-html)
endif()

qt_standard_project_setup()

# Everything except main.cpp, so the tests can link the same code.
add_library(ivycore STATIC
    src/core/models.cpp
    src/core/hlc.cpp
    src/core/ids.cpp
    src/core/markdown.cpp
    src/core/search.cpp
    src/core/tree.cpp
    src/core/i18n.cpp
    src/core/format.cpp
    src/core/notelock.cpp
    src/core/tasks.cpp
    src/core/diff.cpp
    src/core/insertitems.cpp
    src/core/agenda.cpp
    src/storage/crypto.cpp
    src/storage/secrets.cpp
    src/storage/localstore.cpp
    src/sync/api.cpp
    src/sync/transport.cpp
    src/sync/engine.cpp
    src/sync/events.cpp
    src/vault/vaults.cpp
    src/vault/session.cpp
    src/vault/files.cpp
    src/vault/transfer.cpp
    src/vault/backup.cpp
    src/ui/render.cpp
)
target_include_directories(ivycore PUBLIC src)
target_compile_definitions(ivycore PUBLIC IVY_VERSION="${IVY_VERSION}")
target_link_libraries(ivycore PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Concurrent
    qt6keychain PkgConfig::SODIUM ${IVY_MD4C_TARGET})
if(APPLE)
    target_link_libraries(ivycore PUBLIC "-framework Security" "-framework CoreFoundation")
endif()

# Widgets, shared by the app and the UI tests.
add_library(ivyui STATIC
    src/ui/theme.cpp
    src/ui/icons.cpp
    src/ui/widgets.cpp
    src/ui/toast.cpp
    src/ui/shelf.cpp
    src/ui/panels.cpp
    src/ui/noteinfo.cpp
    src/ui/editor.cpp
    src/ui/highlighter.cpp
    src/ui/markdownview.cpp
    src/ui/remoteimages.cpp
    src/ui/notepane.cpp
    src/ui/palette.cpp
    src/ui/dialogs.cpp
    src/ui/diffdialog.cpp
    src/ui/taskpicker.cpp
    src/ui/vaultdialog.cpp
    src/ui/officialsites.cpp
    src/ui/serverpanels.cpp
    src/ui/updater.cpp
    src/ui/backupdialog.cpp
    src/ui/settingsdialog.cpp
    src/ui/historydialog.cpp
    src/ui/mainwindow.cpp
)
target_link_libraries(ivyui PUBLIC ivycore Qt6::Widgets Qt6::Svg)

if(IVY_BUILD_APP)
qt_add_executable(ivynote WIN32 MACOSX_BUNDLE
    src/main.cpp
    resources/resources.qrc
)
target_link_libraries(ivynote PRIVATE ivyui)
if(APPLE)
    configure_file(resources/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
    target_sources(ivynote PRIVATE resources/icons/app.icns)
    set_source_files_properties(resources/icons/app.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    set_target_properties(ivynote PROPERTIES
        OUTPUT_NAME Ivynote
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    )
else()
    set_target_properties(ivynote PROPERTIES OUTPUT_NAME ivynote)
endif()

if(WIN32)
    configure_file(resources/ivynote.rc.in ${CMAKE_CURRENT_BINARY_DIR}/ivynote.rc @ONLY)
    target_sources(ivynote PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/ivynote.rc)
endif()

include(GNUInstallDirs)
if(APPLE)
    install(TARGETS ivynote BUNDLE DESTINATION .)
else()
    install(TARGETS ivynote RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(UNIX AND NOT APPLE)
    install(FILES resources/ivynote.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications RENAME app.ivynote.Ivynote.desktop)
    install(FILES resources/icons/app.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME app.ivynote.Ivynote.svg)
    install(FILES resources/icons/app-512.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps RENAME app.ivynote.Ivynote.png)
    install(FILES resources/app.ivynote.Ivynote.appdata.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()
endif()

if(IVY_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    foreach(t core engine integration ui transfer notelock)
        add_executable(test_${t} tests/test_${t}.cpp)
        target_link_libraries(test_${t} PRIVATE ivyui Qt6::Test)
        add_test(NAME ${t} COMMAND test_${t})
        set_tests_properties(${t} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;IVY_SECRETS_IN_FILE=1")
    endforeach()
endif()
