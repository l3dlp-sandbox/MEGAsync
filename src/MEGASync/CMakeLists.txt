#
# MEGA Desktop App
#

if (WIN32)
    add_executable(MEGAsync WIN32)
elseif (APPLE)
    add_executable(MEGAsync MACOSX_BUNDLE)
else()
    add_executable(MEGAsync)
    set_target_properties(MEGAsync PROPERTIES OUTPUT_NAME "megasync")
endif()

set(MEGA_DESKTOP_APP_HEADERS
    BlockingStageProgressController.h
    CommonMessages.h
    EventUpdater.h
    FolderTransferEvents.h
    FolderTransferListener.h
    MegaApplication.h
    ScaleFactorManager.h
    ScanStageController.h
    TransferQuota.h
    UserAlertTimedClustering.h
    drivedata.h
)

set(MEGA_DESKTOP_APP_SOURCES
    BlockingStageProgressController.cpp
    CommonMessages.cpp
    EventUpdater.cpp
    FolderTransferListener.cpp
    MegaApplication.cpp
    ScaleFactorManager.cpp
    ScanStageController.cpp
    TransferQuota.cpp
    UserAlertTimedClustering.cpp
    drivedata.cpp
    main.cpp
)

if (WIN32)
    set(RESOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/icon.rc
        ${CMAKE_CURRENT_SOURCE_DIR}/config.rc
    )
elseif (APPLE)
    set(RESOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/app.icns
        ${CMAKE_CURRENT_SOURCE_DIR}/folder_yosemite.icns
        ${CMAKE_CURRENT_SOURCE_DIR}/folder_bigsur.icns
        ${CMAKE_CURRENT_SOURCE_DIR}/appicon32.tiff
    )

    set_target_properties(MEGAsync PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info_MEGA.plist
        RESOURCE "${RESOURCE_FILES}"
    )
endif()

target_sources(MEGAsync
    PRIVATE
    ${MEGA_DESKTOP_APP_HEADERS}
    ${MEGA_DESKTOP_APP_SOURCES}
    ${RESOURCE_FILES}
)

# Activate properties for Qt code
set_target_properties(MEGAsync
    PROPERTIES
    AUTOUIC ON # Activates the User Interface Compiler generator for Qt.
    AUTOMOC ON # Activates the meta-object code generator for Qt.
    AUTORCC ON # Activates the creation of rules over qrc files
)


find_package(Qt5 REQUIRED COMPONENTS LinguistTools)
qt5_create_translation(QM_OUTPUT ${CMAKE_CURRENT_SOURCE_DIR} gui/translations/translation.source.ts
    OPTIONS -locations none -no-ui-lines -no-obsolete)

# Generate ts files before MEGAsync is compiled
add_custom_target(generate_ts DEPENDS ${QM_OUTPUT})
add_dependencies(MEGAsync generate_ts)

target_include_directories(MEGAsync PRIVATE ${CMAKE_CURRENT_LIST_DIR})

set(ExecutableTarget MEGAsync)

include(control/control.cmake)
include(gui/gui.cmake)
include(notifications/notifications.cmake)
include(platform/platform.cmake)
include(qtlockedfile/qtlockedfile.cmake)
include(stalled_issues/stalledissues.cmake)
include(syncs/syncs.cmake)
include(transfers/transfers.cmake)
include(UserAttributesRequests/userattributesrequests.cmake)

target_compile_definitions(MEGAsync
    PUBLIC
    QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII
    $<$<CONFIG:Debug>:LOG_TO_STDOUT LOG_TO_LOGGER CREATE_COMPATIBLE_MINIDUMPS>
    $<$<BOOL:${WIN32}>:PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE>
    $<$<BOOL:${ENABLE_ISOLATED_GFX}>:ENABLE_SDK_ISOLATED_GFX>
    $<$<BOOL:${USE_BREAKPAD}>:USE_BREAKPAD>
)

# Load and link needed libraries for the Desktop App target
if(USE_BREAKPAD)
    find_package(unofficial-breakpad CONFIG REQUIRED)

    if(NOT DEFINED CACHE{CRASH_BACKEND_URL})
        set(CRASH_BACKEND_URL "" CACHE STRING "Crash backend URL")
    endif()
    if(DEFINED ENV{MEGA_CRASH_BACKEND_URL} AND NOT "$ENV{MEGA_CRASH_BACKEND_URL}" STREQUAL "")
        set(CRASH_BACKEND_URL "$ENV{MEGA_CRASH_BACKEND_URL}" CACHE STRING "Crash backend URL" FORCE)
    endif()
    if("${CRASH_BACKEND_URL}" STREQUAL "")
        message(WARNING "CRASH_BACKEND_URL is empty. Crash reporting may not function correctly.")
    endif()
    target_compile_definitions(MEGAsync PRIVATE CRASH_BACKEND_URL="${CRASH_BACKEND_URL}")

    message(STATUS "Breakpad added")
endif()

if (WIN32)
    find_package(Qt5 REQUIRED COMPONENTS WinExtras)
elseif (APPLE)
    find_package(Qt5 REQUIRED COMPONENTS MacExtras Svg)
else()
    find_package(Qt5 REQUIRED COMPONENTS Svg)
endif()

find_package(Qt5 REQUIRED COMPONENTS Widgets Core Gui Network Qml Quick QuickWidgets)
message(STATUS "Building the MEGA Desktop App with Qt v${Qt5_VERSION} from ${Qt5_DIR}")

target_link_libraries(MEGAsync
    PUBLIC
    MEGA::SDKlib
    MEGA::SDKQtBindings
    $<$<BOOL:${WIN32}>:Qt5::WinExtras>
    $<$<BOOL:${APPLE}>:Qt5::MacExtras>
    $<$<BOOL:${UNIX}>:Qt5::Svg>
    $<$<BOOL:${USE_BREAKPAD}>:unofficial::breakpad::libbreakpad>
    $<$<BOOL:${USE_BREAKPAD}>:unofficial::breakpad::libbreakpad_client>
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
    Qt5::Network
    Qt5::Qml
    Qt5::Quick
    Qt5::QuickWidgets
)

## Adjust compilation flags for warnings and errors ##
target_platform_compile_options(
    TARGET MEGAsync
    WINDOWS /W4
            /wd4201 # nameless struct/union (nonstandard)
            /wd4458 # identifier hides class member
            /wd4100 # unreferenced formal parameter
            /wd4456 # declaration hides previous local declaration
            /we4800 # Implicit conversion from 'type' to bool. Possible information loss
    UNIX $<$<CONFIG:Debug>:-ggdb3> -Wall -Wextra -Wconversion -Wno-unused-parameter -D__STDC_FORMAT_MACROS
)

if (ENABLE_DESKTOP_APP_WERROR)
    target_platform_compile_options(
        TARGET MEGAsync
        WINDOWS /WX
        UNIX  $<$<CONFIG:Debug>: -Werror
                                 -Wno-error=deprecated-declarations> # Kept as a warning, do not promote to error.
    )
endif()

# Install MEGAsync binary, dynamic libraries and, if needed, deploy Qt dependencies.
if (UNIX AND NOT APPLE)

    install(TARGETS MEGAsync RUNTIME) # In CMAKE_INSTALL_LIBDIR directory

    # Install GFX worker if needed
    if(TARGET gfxworker)
        install(TARGETS gfxworker RUNTIME) # In CMAKE_INSTALL_LIBDIR directory
    endif()

    set(vcpkg_lib_folder "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>lib/")

    install(DIRECTORY "${vcpkg_lib_folder}"
        TYPE LIB # In CMAKE_INSTALL_LIBDIR directory
        FILES_MATCHING
        PATTERN "*.so*"
        PATTERN "manual-link" EXCLUDE
        PATTERN "pkgconfig" EXCLUDE
    )

    if(DEPLOY_QT_LIBRARIES)
        include(desktopapp_deploy_qt)
    endif()

endif()

# Amend manifest to tell Windows that the application is DPI aware (needed for Windows 8.1 and up)
if (MSVC)
    add_custom_command(
        TARGET MEGAsync
        POST_BUILD
        COMMAND "mt.exe" -manifest \"${CMAKE_CURRENT_SOURCE_DIR}\\MEGAsync.exe.manifest\" -inputresource:\"$<TARGET_FILE:MEGAsync>\"\;\#1 -outputresource:\"$<TARGET_FILE:MEGAsync>\"\;\#1
        COMMENT "Adding display aware manifest..."
    )
endif()

# Copy GFX worker if needed
if(TARGET gfxworker)
    set_target_properties(gfxworker PROPERTIES OUTPUT_NAME "mega-desktop-app-gfxworker")
    add_custom_command(
        TARGET MEGAsync POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:gfxworker> $<TARGET_FILE_DIR:MEGAsync>
        COMMENT "Copying gfxworker for MEGAsync."
    )
endif()

