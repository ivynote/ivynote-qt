cmake_minimum_required(VERSION 3.21)
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

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Sql Svg Concurrent WebEngineWidgets)
find_package(Qt6Keychain REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c md4c-html)

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
    src/ui/render.cpp
)
target_include_directories(ivycore PUBLIC src)
target_compile_definitions(ivycore PUBLIC IVY_VERSION="${IVY_VERSION}")
target_link_libraries(ivycore PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Concurrent
    qt6keychain PkgConfig::SODIUM PkgConfig::MD4C)

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
    src/ui/vaultdialog.cpp
    src/ui/officialsites.cpp
    src/ui/serverpanels.cpp
    src/ui/updater.cpp
    src/ui/settingsdialog.cpp
    src/ui/historydialog.cpp
    src/ui/mainwindow.cpp
)
target_link_libraries(ivyui PUBLIC ivycore Qt6::Widgets Qt6::Svg Qt6::WebEngineWidgets)

if(IVY_BUILD_APP)
qt_add_executable(ivynote WIN32
    src/main.cpp
    resources/resources.qrc
)
target_link_libraries(ivynote PRIVATE ivyui)
set_target_properties(ivynote PROPERTIES OUTPUT_NAME ivynote)

if(WIN32)
    configure_file(resources/ivynote.rc.in ${CMAKE_CURRENT_BINARY_DIR}/ivynote.rc @ONLY)
    target_sources(ivynote PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/ivynote.rc)
endif()

include(GNUInstallDirs)
install(TARGETS ivynote RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
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
        set_tests_properties(${t} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
endif()
