# Windows CI/local uses Qt 5.12.12 (msvc2017_64); Linux distros ship newer
# Qt 5.x (Ubuntu 24.04: 5.15.13). The code targets stable 5.12-era APIs, so a
# minimum-version find is used instead of the former EXACT 5.12.12 pin.
find_package(Qt5 5.12 REQUIRED COMPONENTS
    Core
    Concurrent
    Widgets
    Network
    Multimedia
    MultimediaWidgets
)

# The Windows resource compiler language is only needed for app.rc
# (the exe icon); harmless to enable on MSVC, required for MinGW.
if(WIN32)
    enable_language(RC)
endif()

# Development-only escape hatch for a MinGW toolchain, which cannot compile
# music_window.cpp / pdf_window.cpp (they need MSVC + the C++/WinRT headers for
# SystemMediaTransportControls and Windows.Data.Pdf). When ON, those two
# translation units are replaced by src/windows_fallback.cpp, an honest
# "not available" implementation of the same two classes, so the rest of the
# desktop app can be built and exercised locally. OFF by default; ignored for
# MSVC builds, which always compile the real WinRT windows, so CI and release
# artifacts are byte-for-byte unchanged.
option(MVP_MINGW_APP_FALLBACK
    "Build music/PDF as 'not available' stubs so the app links with MinGW (development only, never release)"
    OFF)

set(MVP_APP_WINRT_SOURCES
    src/music_window.cpp
    src/pdf_window.cpp
)
set(MVP_APP_FALLBACK_SOURCES "")
if(MVP_MINGW_APP_FALLBACK)
    if(MSVC)
        message(WARNING
            "MVP_MINGW_APP_FALLBACK ignored: MSVC builds always use the real "
            "C++/WinRT music and PDF windows.")
    elseif(WIN32 OR MINGW)
        set(MVP_APP_WINRT_SOURCES "")
        set(MVP_APP_FALLBACK_SOURCES src/windows_fallback.cpp)
        message(WARNING
            "MVP_MINGW_APP_FALLBACK=ON: music and PDF are 'not available' stubs. "
            "This is a development smoke-test build, not a release configuration.")
    else()
        message(WARNING
            "MVP_MINGW_APP_FALLBACK ignored: only supported for a non-MSVC "
            "Windows (MinGW) build.")
    endif()
endif()

set(MVP_APP_SOURCES
    src/main.cpp
    src/app_update.cpp
    src/main_window.cpp
    src/media_decoder.cpp
    src/player_window.cpp
    ${MVP_APP_WINRT_SOURCES}
    ${MVP_APP_FALLBACK_SOURCES}
    src/in_window_sheet.cpp
    src/record_document.cpp
    src/record_window.cpp
    src/settings_dialog.cpp
    src/batch_worker.cpp
    src/share_server.cpp
    src/share_tls.cpp
    src/peer_link.cpp
    src/peer_browser.cpp

    third_party/qrcodegen/qrcodegen.cpp
    include/videovault/app/app_update.hpp
    include/videovault/app/main_window.hpp
    include/videovault/app/media_decoder.hpp
    include/videovault/app/player_window.hpp
    include/videovault/app/music_window.hpp
    include/videovault/app/pdf_window.hpp
    include/videovault/app/in_window_sheet.hpp
    include/videovault/app/record_document.hpp
    include/videovault/app/record_window.hpp
    include/videovault/app/settings_dialog.hpp
    include/videovault/app/batch_worker.hpp
    include/videovault/app/share_server.hpp
    include/videovault/app/share_tls.hpp
    include/videovault/app/peer_link.hpp
    include/videovault/app/peer_browser.hpp
    resources/resources.qrc
)

add_library(MegaVaultSync STATIC
    src/sync_state.cpp src/sync_client.cpp src/sync_engine.cpp src/sync_page.cpp
    include/videovault/app/sync_state.hpp
    include/videovault/app/sync_client.hpp
    include/videovault/app/sync_engine.hpp
    include/videovault/app/sync_page.hpp)
target_include_directories(MegaVaultSync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(MegaVaultSync PUBLIC cxx_std_20)
set_target_properties(MegaVaultSync PROPERTIES AUTOMOC ON)
target_link_libraries(MegaVaultSync PUBLIC VideoVault::Core Qt5::Core Qt5::Concurrent Qt5::Network Qt5::Widgets)
if(WIN32)
    target_link_libraries(MegaVaultSync PRIVATE MvpSodium)
elseif(ANDROID)
    target_include_directories(MegaVaultSync PRIVATE ${MVP_DEP_INCLUDE_DIRS})
    target_link_libraries(MegaVaultSync PRIVATE ${MVP_SODIUM_LIBS})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SYNC_SODIUM REQUIRED IMPORTED_TARGET libsodium)
    target_link_libraries(MegaVaultSync PRIVATE PkgConfig::SYNC_SODIUM)
endif()

if(ANDROID)
    # Android apps are shared libraries: androiddeployqt packages the .so into
    # the APK, where the Qt Activity (android.app.lib_name = MegaVaultProtect)
    # loads it. There is no WIN32 subsystem or .rc resource on Android.
    add_library(VideoVaultApp SHARED ${MVP_APP_SOURCES})
    set_target_properties(VideoVaultApp PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
else()
    add_executable(VideoVaultApp WIN32 ${MVP_APP_SOURCES})
endif()

if(WIN32)
    # Embeds the application icon into MegaVaultProtect.exe (Explorer,
    # taskbar, and the Alt-Tab icon all come from this resource).
    target_sources(VideoVaultApp PRIVATE resources/app.rc)
endif()

target_include_directories(VideoVaultApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen
)
if(WIN32)
    # vcpkg FFmpeg headers (Linux pkg-config targets carry their own includes).
    target_include_directories(VideoVaultApp PRIVATE "${MVP_VCPKG_PREFIX}/include")
elseif(ANDROID)
    # NDK-built FFmpeg/sodium/sqlcipher headers live in the dep prefix.
    target_include_directories(VideoVaultApp PRIVATE ${MVP_DEP_INCLUDE_DIRS})
endif()

target_link_libraries(VideoVaultApp
    PRIVATE
        VideoVault::Core
        MegaVaultSync
        Qt5::Core
        Qt5::Concurrent
        Qt5::Widgets
        Qt5::Network
        Qt5::Multimedia
        Qt5::MultimediaWidgets
        ${MVP_FFMPEG_LIBS}
)
if(UNIX AND NOT ANDROID)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(VideoVaultApp PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
if(WIN32)
    target_link_libraries(VideoVaultApp PRIVATE crypt32 secur32 windowsapp winhttp)
    set(_mvp_cppwinrt "")
    if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        set(_mvp_cppwinrt "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
    endif()
    if(NOT EXISTS "${_mvp_cppwinrt}/winrt/Windows.Data.Pdf.h")
        file(GLOB _mvp_kits "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
        list(SORT _mvp_kits ORDER DESCENDING)
        list(GET _mvp_kits 0 _mvp_cppwinrt)
    endif()
    if(EXISTS "${_mvp_cppwinrt}/winrt/Windows.Data.Pdf.h")
        target_include_directories(VideoVaultApp PRIVATE "${_mvp_cppwinrt}")
    endif()
endif()

target_compile_features(VideoVaultApp PRIVATE cxx_std_20)
set_target_properties(VideoVaultApp PROPERTIES
    OUTPUT_NAME "MegaVaultProtect"
    FOLDER "Application"
    AUTOMOC ON
    AUTORCC ON
)
if(MSVC)
    set_target_properties(VideoVaultApp PROPERTIES
        VS_DEBUGGER_ENVIRONMENT "PATH=C:\\Qt\\Qt5.12.12\\5.12.12\\msvc2017_64\\bin;%PATH%")
endif()

mvp_copy_runtime_dlls(VideoVaultApp)

option(MVP_BUILD_LEGACY_CLOUD "Build deprecated remote vault-password service (NOT sync v2 E2EE)" OFF)
if(NOT ANDROID AND MVP_BUILD_LEGACY_CLOUD)
    message(WARNING "MegaVaultCloud is deprecated remote decrypted access, NOT sync v2 E2EE. Use the Python sync v2 relay.")
    add_executable(MegaVaultCloud
        src/cloud_server.cpp
        src/share_tls.cpp
    )
    target_include_directories(MegaVaultCloud PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(MegaVaultCloud
        PRIVATE
            VideoVault::Core
            Qt5::Core
            Qt5::Network
            ${MVP_SODIUM_LIBS}
            ${MVP_ARGON2_LIBS}
            ${MVP_SQLCIPHER_LIBS}
            ${MVP_FFMPEG_LIBS}
    )
    if(WIN32)
        target_include_directories(MegaVaultCloud PRIVATE "${MVP_VCPKG_PREFIX}/include")
        target_link_libraries(MegaVaultCloud PRIVATE crypt32 secur32)
    endif()
    if(UNIX AND NOT ANDROID)
        find_package(OpenSSL REQUIRED)
        target_link_libraries(MegaVaultCloud PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    target_compile_features(MegaVaultCloud PRIVATE cxx_std_20)
    set_target_properties(MegaVaultCloud PROPERTIES
        OUTPUT_NAME "MegaVaultCloud"
        FOLDER "Application"
    )
    mvp_copy_runtime_dlls(MegaVaultCloud)
endif()

# Headless playback test: decodes frames from an imported video through the
# same MediaDecoder the player window uses, without any GUI.
if(MVP_BUILD_TESTS)
    enable_testing()
    # Sync protocol 4: the state file, key derivation and validation run without
    # a server; the engine test runs against a real sync_server through
    # tests/run_sync_live.py and skips when it is started without one.
    add_executable(VideoVaultSyncStateTests tests/sync_state.cpp)
    add_executable(VideoVaultSyncEngineTests tests/sync_engine.cpp)
    add_executable(VideoVaultSyncDialogTests tests/sync_page.cpp)
    foreach(_sync_test VideoVaultSyncStateTests VideoVaultSyncEngineTests VideoVaultSyncDialogTests)
        target_link_libraries(${_sync_test} PRIVATE MegaVaultSync)
    endforeach()
    add_test(NAME sync_state COMMAND VideoVaultSyncStateTests)
    add_test(NAME sync_engine COMMAND VideoVaultSyncEngineTests)
    set_tests_properties(sync_engine PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME sync_page COMMAND VideoVaultSyncDialogTests)
    set_tests_properties(sync_page PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(VideoVaultPlaybackTests
        tests/playback.cpp
        src/media_decoder.cpp
        src/player_window.cpp
        include/videovault/app/media_decoder.hpp
        include/videovault/app/player_window.hpp
    )
    set_target_properties(VideoVaultPlaybackTests PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(VideoVaultPlaybackTests
        PRIVATE
            VideoVault::Core
            Qt5::Core
            Qt5::Gui
            Qt5::Widgets
            Qt5::Multimedia
            ${MVP_FFMPEG_LIBS}
    )
    target_include_directories(VideoVaultPlaybackTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    if(WIN32)
        target_include_directories(VideoVaultPlaybackTests PRIVATE "${MVP_VCPKG_PREFIX}/include")
    endif()
    target_compile_features(VideoVaultPlaybackTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultPlaybackTests PROPERTIES FOLDER "Tests")

    mvp_copy_runtime_dlls(VideoVaultPlaybackTests)

    # Headless check of the record content format: the JSON container, the
    # routing marker and the inferred value kinds that the record editor and
    # the gallery both depend on. It links the real record_document.cpp.
    add_executable(VideoVaultRecordFormatTests
        tests/record_format.cpp
        src/record_document.cpp
        include/videovault/app/record_document.hpp
    )
    target_link_libraries(VideoVaultRecordFormatTests
        PRIVATE
            Qt5::Core
    )
    target_include_directories(VideoVaultRecordFormatTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_features(VideoVaultRecordFormatTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultRecordFormatTests PROPERTIES FOLDER "Tests")

    mvp_copy_runtime_dlls(VideoVaultRecordFormatTests)
endif()
