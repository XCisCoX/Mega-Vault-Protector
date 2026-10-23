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
    src/music_library.cpp
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
    src/local_audio_pipeline.cpp
    src/player_window.cpp
    ${MVP_APP_WINRT_SOURCES}
    ${MVP_APP_FALLBACK_SOURCES}
    src/in_window_sheet.cpp
    src/record_document.cpp
    src/record_window.cpp
    src/secure_input.cpp
    src/settings_dialog.cpp
    src/tag_filter_button.cpp
    src/theme.cpp
    src/icons.cpp
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
    include/videovault/app/music_library.hpp
    include/videovault/app/pdf_window.hpp
    include/videovault/app/in_window_sheet.hpp
    include/videovault/app/record_document.hpp
    include/videovault/app/record_window.hpp
    include/videovault/app/settings_dialog.hpp
    include/videovault/app/tag_filter_button.hpp
    include/videovault/app/icons.hpp
    include/videovault/app/batch_worker.hpp
    include/videovault/app/share_server.hpp
    include/videovault/app/share_tls.hpp
    include/videovault/app/peer_link.hpp
    include/videovault/app/peer_browser.hpp
    resources/resources.qrc
    resources/icons.qrc
    resources/telegram_player.qrc
)

add_library(MegaVaultNetwork STATIC src/proxy_settings.cpp)
target_include_directories(MegaVaultNetwork PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(MegaVaultNetwork PUBLIC Qt5::Network)
target_compile_features(MegaVaultNetwork PUBLIC cxx_std_20)
if(WIN32 OR MINGW)
    target_link_libraries(MegaVaultNetwork PRIVATE crypt32)
elseif(APPLE)
    target_link_libraries(MegaVaultNetwork PRIVATE "-framework Security" "-framework CoreFoundation")
endif()

add_library(MegaVaultPlugins STATIC src/plugin_host.cpp src/plugin_audio.cpp src/plugin_tools.cpp src/media_decoder.cpp
    src/remote_stream_source.cpp
    src/soundcloud_provider.cpp
    src/soundcloud_transport.cpp
    src/soundcloud_hls_source.cpp
    src/soundcloud_page.cpp
    src/music_resolver.cpp
    include/videovault/app/music_resolver.hpp
    include/videovault/app/soundcloud_provider.hpp
    include/videovault/app/soundcloud_page.hpp
    include/videovault/app/plugin_audio.hpp include/videovault/app/plugin_tools.hpp include/videovault/app/plugin_host.hpp
    include/videovault/app/byte_source.hpp include/videovault/app/remote_stream_source.hpp
    resources/plugins.qrc)
target_include_directories(MegaVaultPlugins PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(MegaVaultPlugins PUBLIC cxx_std_20)
set_target_properties(MegaVaultPlugins PROPERTIES AUTOMOC ON AUTORCC ON)
target_link_libraries(MegaVaultPlugins PUBLIC MegaVaultNetwork VideoVault::Core MegaVaultExtensions Qt5::Widgets Qt5::Network Qt5::Multimedia Qt5::Concurrent ${MVP_FFMPEG_LIBS})
if(WIN32 OR MINGW)
    target_include_directories(MegaVaultPlugins PRIVATE "${MVP_VCPKG_PREFIX}/include")
    # Provider requests use WinHTTP on Windows: Qt 5.12 links OpenSSL 1.1 while
    # the shipped app carries OpenSSL 3, so QSslSocket cannot initialise there.
    target_link_libraries(MegaVaultPlugins PUBLIC winhttp)
elseif(ANDROID)
    target_include_directories(MegaVaultPlugins PRIVATE ${MVP_DEP_INCLUDE_DIRS})
endif()

add_library(MegaVaultSync STATIC
    src/sync_state.cpp src/sync_client.cpp src/sync_engine.cpp src/sync_page.cpp src/icons.cpp
    src/secure_input.cpp
    include/videovault/app/sync_state.hpp
    include/videovault/app/sync_client.hpp
    include/videovault/app/sync_engine.hpp
    include/videovault/app/sync_page.hpp)
target_include_directories(MegaVaultSync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(MegaVaultSync PUBLIC cxx_std_20)
set_target_properties(MegaVaultSync PROPERTIES AUTOMOC ON)
target_link_libraries(MegaVaultSync PUBLIC MegaVaultNetwork VideoVault::Core Qt5::Core Qt5::Concurrent Qt5::Network Qt5::Widgets)
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
        MegaVaultPlugins
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
# Keep the source attribution and complete license next to local/release builds.
add_custom_command(TARGET VideoVaultApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/telegram-player"
        "$<TARGET_FILE_DIR:VideoVaultApp>/licenses/telegram-player")

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
    add_executable(VideoVaultTelegramIconTests tests/telegram_player_icons.cpp resources/telegram_player.qrc)
    target_include_directories(VideoVaultTelegramIconTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VideoVaultTelegramIconTests PRIVATE Qt5::Widgets)
    target_compile_features(VideoVaultTelegramIconTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultTelegramIconTests PROPERTIES AUTORCC ON FOLDER "Tests")
    add_test(NAME telegram_player_icons COMMAND VideoVaultTelegramIconTests)
    set_tests_properties(telegram_player_icons PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    mvp_copy_runtime_dlls(VideoVaultTelegramIconTests)

    add_executable(VideoVaultTelegramPhotoChromeTests tests/telegram_photo_chrome.cpp)
    target_include_directories(VideoVaultTelegramPhotoChromeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VideoVaultTelegramPhotoChromeTests PRIVATE Qt5::Core)
    target_compile_features(VideoVaultTelegramPhotoChromeTests PRIVATE cxx_std_20)
    add_test(NAME telegram_photo_chrome COMMAND VideoVaultTelegramPhotoChromeTests)
    set_tests_properties(telegram_photo_chrome PROPERTIES TIMEOUT 30)

    add_executable(VideoVaultVideoPacingTests tests/video_pacing.cpp)
    target_include_directories(VideoVaultVideoPacingTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(VideoVaultVideoPacingTests PRIVATE cxx_std_20)
    add_test(NAME video_av_timing COMMAND VideoVaultVideoPacingTests)
    set_tests_properties(video_av_timing PROPERTIES TIMEOUT 30)

    add_executable(VideoVaultTelegramVideoChromeTests tests/telegram_video_chrome.cpp)
    target_include_directories(VideoVaultTelegramVideoChromeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VideoVaultTelegramVideoChromeTests PRIVATE Qt5::Widgets)
    target_compile_features(VideoVaultTelegramVideoChromeTests PRIVATE cxx_std_20)
    add_test(NAME telegram_video_chrome COMMAND VideoVaultTelegramVideoChromeTests)
    set_tests_properties(telegram_video_chrome PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    mvp_copy_runtime_dlls(VideoVaultTelegramVideoChromeTests)

    foreach(_chrome_kind Music Video)
        if(_chrome_kind STREQUAL "Music")
            set(_chrome_test tests/telegram_music_ui.cpp)
            set(_chrome_name telegram_music_ui)
        else()
            set(_chrome_test tests/telegram_video_window.cpp)
            set(_chrome_name telegram_video_window)
        endif()
        set(_chrome_target VideoVaultTelegram${_chrome_kind}UiTests)
        add_executable(${_chrome_target} ${_chrome_test}
            src/music_window.cpp src/player_window.cpp src/media_decoder.cpp src/local_audio_pipeline.cpp
            src/icons.cpp src/theme.cpp resources/icons.qrc resources/telegram_player.qrc
            include/videovault/app/music_window.hpp include/videovault/app/player_window.hpp)
        target_include_directories(${_chrome_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(${_chrome_target} PRIVATE VideoVault::Core Qt5::Widgets
            Qt5::Multimedia Qt5::Concurrent ${MVP_FFMPEG_LIBS})
        if(WIN32)
            target_include_directories(${_chrome_target} PRIVATE "${MVP_VCPKG_PREFIX}/include" "${_mvp_cppwinrt}")
            target_link_libraries(${_chrome_target} PRIVATE windowsapp)
        elseif(ANDROID)
            target_include_directories(${_chrome_target} PRIVATE ${MVP_DEP_INCLUDE_DIRS})
        endif()
        target_compile_features(${_chrome_target} PRIVATE cxx_std_20)
        set_target_properties(${_chrome_target} PROPERTIES AUTOMOC ON AUTORCC ON FOLDER "Tests")
        add_test(NAME ${_chrome_name} COMMAND ${_chrome_target})
        set_tests_properties(${_chrome_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
        mvp_copy_runtime_dlls(${_chrome_target})
    endforeach()

    add_executable(VideoVaultPlaylistModelTests tests/playlist_model.cpp)
    target_include_directories(VideoVaultPlaylistModelTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(VideoVaultPlaylistModelTests PRIVATE cxx_std_20)
    add_test(NAME music_playlist_model COMMAND VideoVaultPlaylistModelTests)
    # QCoreApplication-only tests. Compile the real MusicWindow translation unit
    # too, but never instantiate widgets or open an audio device in this test.
    add_executable(VideoVaultLocalAudioTests
        tests/local_audio_pipeline.cpp src/local_audio_pipeline.cpp src/media_decoder.cpp
        src/music_window.cpp src/player_window.cpp src/icons.cpp src/theme.cpp
        include/videovault/app/music_window.hpp include/videovault/app/player_window.hpp)
    target_include_directories(VideoVaultLocalAudioTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VideoVaultLocalAudioTests PRIVATE VideoVault::Core Qt5::Core Qt5::Gui
        Qt5::Widgets Qt5::Multimedia Qt5::Concurrent ${MVP_FFMPEG_LIBS})
    if(WIN32)
        target_include_directories(VideoVaultLocalAudioTests PRIVATE "${MVP_VCPKG_PREFIX}/include" "${_mvp_cppwinrt}")
        target_link_libraries(VideoVaultLocalAudioTests PRIVATE windowsapp)
    elseif(ANDROID)
        target_include_directories(VideoVaultLocalAudioTests PRIVATE ${MVP_DEP_INCLUDE_DIRS})
    endif()
    target_compile_features(VideoVaultLocalAudioTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultLocalAudioTests PROPERTIES AUTOMOC ON FOLDER "Tests")
    add_test(NAME local_audio_pipeline COMMAND VideoVaultLocalAudioTests)
    set_tests_properties(local_audio_pipeline PROPERTIES TIMEOUT 90)
    mvp_copy_runtime_dlls(VideoVaultLocalAudioTests)
    # Real Ogg/Opus generated in RAM, decoded through the encrypted music path.
    add_executable(VideoVaultOpusAudioTests tests/opus_audio.cpp
        src/local_audio_pipeline.cpp src/media_decoder.cpp)
    target_include_directories(VideoVaultOpusAudioTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VideoVaultOpusAudioTests PRIVATE VideoVault::Core Qt5::Core Qt5::Gui ${MVP_FFMPEG_LIBS})
    if(WIN32)
        target_include_directories(VideoVaultOpusAudioTests PRIVATE "${MVP_VCPKG_PREFIX}/include")
    elseif(ANDROID)
        target_include_directories(VideoVaultOpusAudioTests PRIVATE ${MVP_DEP_INCLUDE_DIRS})
    endif()
    target_compile_features(VideoVaultOpusAudioTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultOpusAudioTests PROPERTIES FOLDER "Tests")
    add_test(NAME opus_audio COMMAND VideoVaultOpusAudioTests)
    set_tests_properties(opus_audio PROPERTIES TIMEOUT 180)
    mvp_copy_runtime_dlls(VideoVaultOpusAudioTests)

    add_executable(VideoVaultProxyTests tests/proxy_settings.cpp src/app_update.cpp)
    target_link_libraries(VideoVaultProxyTests PRIVATE MegaVaultNetwork MegaVaultSync MegaVaultPlugins)
    if(WIN32 OR MINGW)
        target_link_libraries(VideoVaultProxyTests PRIVATE winhttp)
    endif()
    add_test(NAME proxy_settings COMMAND VideoVaultProxyTests)
    set_tests_properties(proxy_settings PROPERTIES TIMEOUT 30)
    mvp_copy_runtime_dlls(VideoVaultProxyTests)
    add_executable(VideoVaultPluginTests tests/plugin_host.cpp)
    target_link_libraries(VideoVaultPluginTests PRIVATE MegaVaultPlugins)
    add_test(NAME plugin_host COMMAND VideoVaultPluginTests)
    set_tests_properties(plugin_host PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    mvp_copy_runtime_dlls(VideoVaultPluginTests)
    # Sync protocol 4: the state file, key derivation and validation run without
    # a server; the engine test runs against a real sync_server through
    # tests/run_sync_live.py and skips when it is started without one.
    # Optional offscreen screenshot harness (tools/screenshots.cpp). Off by default so
    # CI does not build a second copy of every app source; enable it locally with
    # -DMVP_SCREENSHOT_HARNESS=ON when reviewing icon or layout work.
    option(MVP_SCREENSHOT_HARNESS "Build the offscreen screenshot harness" OFF)
    if(MVP_SCREENSHOT_HARNESS)
    set(_shot_sources ${MVP_APP_SOURCES})
    list(REMOVE_ITEM _shot_sources src/main.cpp)
    add_executable(MvpScreenshotHarness tools/screenshots.cpp ${_shot_sources})
    get_target_property(_shot_links VideoVaultApp LINK_LIBRARIES)
    get_target_property(_shot_includes VideoVaultApp INCLUDE_DIRECTORIES)
    target_link_libraries(MvpScreenshotHarness PRIVATE ${_shot_links})
    if(WIN32 OR MINGW)
        target_link_libraries(MvpScreenshotHarness PRIVATE crypt32 secur32 winhttp dwmapi)
    endif()
    target_include_directories(MvpScreenshotHarness PRIVATE ${_shot_includes})
    target_compile_features(MvpScreenshotHarness PRIVATE cxx_std_20)
    set_target_properties(MvpScreenshotHarness PROPERTIES AUTOMOC ON AUTORCC ON FOLDER "Tools")
    endif()

    # Exercise the production gallery, not a separate model of its cache.
    set(_gallery_sources ${MVP_APP_SOURCES})
    list(REMOVE_ITEM _gallery_sources src/main.cpp)
    add_executable(VideoVaultGalleryPerformanceTests tests/gallery_performance.cpp ${_gallery_sources})
    get_target_property(_gallery_links VideoVaultApp LINK_LIBRARIES)
    get_target_property(_gallery_includes VideoVaultApp INCLUDE_DIRECTORIES)
    target_link_libraries(VideoVaultGalleryPerformanceTests PRIVATE ${_gallery_links})
    if(WIN32 OR MINGW)
        target_link_libraries(VideoVaultGalleryPerformanceTests PRIVATE crypt32 secur32 winhttp dwmapi)
    endif()
    target_include_directories(VideoVaultGalleryPerformanceTests PRIVATE ${_gallery_includes})
    target_compile_features(VideoVaultGalleryPerformanceTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultGalleryPerformanceTests PROPERTIES AUTOMOC ON AUTORCC ON)

    add_executable(VideoVaultVideoNavigationTests tests/telegram_video_navigation.cpp ${_gallery_sources})
    target_link_libraries(VideoVaultVideoNavigationTests PRIVATE ${_gallery_links})
    target_include_directories(VideoVaultVideoNavigationTests PRIVATE ${_gallery_includes})
    target_compile_features(VideoVaultVideoNavigationTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultVideoNavigationTests PROPERTIES AUTOMOC ON AUTORCC ON FOLDER "Tests")
    if(WIN32 OR MINGW)
        target_link_libraries(VideoVaultVideoNavigationTests PRIVATE crypt32 secur32 winhttp dwmapi)
    endif()
    add_test(NAME video_window_navigation COMMAND VideoVaultVideoNavigationTests)
    set_tests_properties(video_window_navigation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    mvp_copy_runtime_dlls(VideoVaultVideoNavigationTests)
    add_test(NAME library_commands COMMAND VideoVaultGalleryPerformanceTests)
    set_tests_properties(library_commands PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MVP_LIBRARY_COMMANDS_TEST=1" TIMEOUT 30)
    add_test(NAME share_unlock COMMAND VideoVaultGalleryPerformanceTests)
    set_tests_properties(share_unlock PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MVP_SHARE_UNLOCK_TEST=1" TIMEOUT 60)
    add_test(NAME auth_layout COMMAND VideoVaultGalleryPerformanceTests)
    set_tests_properties(auth_layout PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MVP_AUTH_LAYOUT_TEST=1" TIMEOUT 60)
    add_test(NAME library_chrome COMMAND VideoVaultGalleryPerformanceTests)
    set_tests_properties(library_chrome PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MVP_LIBRARY_CHROME_TEST=1" TIMEOUT 90)
    add_test(NAME gallery_performance COMMAND VideoVaultGalleryPerformanceTests)
    set_tests_properties(gallery_performance PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    add_test(NAME vault_creation_ui COMMAND VideoVaultGalleryPerformanceTests)
    set_tests_properties(vault_creation_ui PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MVP_VAULT_CREATION_TEST=1" TIMEOUT 150)

    add_executable(VideoVaultPluginToolsTests tests/plugin_tools.cpp)
    target_link_libraries(VideoVaultPluginToolsTests PRIVATE MegaVaultPlugins)
    add_test(NAME plugin_tools COMMAND VideoVaultPluginToolsTests)
    set_tests_properties(plugin_tools PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    mvp_copy_runtime_dlls(VideoVaultPluginToolsTests)
    add_executable(VideoVaultPluginLiveTests tests/plugin_live.cpp)
    target_link_libraries(VideoVaultPluginLiveTests PRIVATE MegaVaultPlugins)
    add_test(NAME plugin_live COMMAND VideoVaultPluginLiveTests)
    set_tests_properties(plugin_live PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 240)
    mvp_copy_runtime_dlls(VideoVaultPluginLiveTests)

    # Progressive provider streaming: a loopback mock HTTP server drives the
    # production RemoteStreamSource (Range, pacing, truncation, cancellation) and
    # the same bytes are encrypted into a real vault while they arrive.
    add_executable(VideoVaultMusicResolverTests tests/music_resolver.cpp)
    target_link_libraries(VideoVaultMusicResolverTests PRIVATE MegaVaultPlugins VideoVault::Core)
    add_test(NAME music_resolver COMMAND VideoVaultMusicResolverTests)
    set_tests_properties(music_resolver PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    mvp_copy_runtime_dlls(VideoVaultMusicResolverTests)

    add_executable(VideoVaultSoundCloudTests tests/soundcloud_provider.cpp
        src/soundcloud_provider.cpp src/soundcloud_transport.cpp src/soundcloud_hls_source.cpp src/media_decoder.cpp)
    target_include_directories(VideoVaultSoundCloudTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VideoVaultSoundCloudTests PRIVATE MegaVaultNetwork VideoVault::Core Qt5::Core Qt5::Gui ${MVP_FFMPEG_LIBS})
    target_compile_features(VideoVaultSoundCloudTests PRIVATE cxx_std_20)
    if(WIN32 OR MINGW)
        target_include_directories(VideoVaultSoundCloudTests PRIVATE "${MVP_VCPKG_PREFIX}/include")
        target_link_libraries(VideoVaultSoundCloudTests PRIVATE winhttp)
    endif()
    add_test(NAME soundcloud_provider COMMAND VideoVaultSoundCloudTests)
    set_tests_properties(soundcloud_provider PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    mvp_copy_runtime_dlls(VideoVaultSoundCloudTests)

    add_executable(VideoVaultProgressiveStreamTests tests/progressive_stream.cpp)
    target_link_libraries(VideoVaultProgressiveStreamTests PRIVATE MegaVaultPlugins VideoVault::Core)
    add_test(NAME progressive_stream COMMAND VideoVaultProgressiveStreamTests)
    set_tests_properties(progressive_stream PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
    mvp_copy_runtime_dlls(VideoVaultProgressiveStreamTests)
    add_test(NAME music_footer COMMAND VideoVaultGalleryPerformanceTests)
    set_tests_properties(music_footer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MVP_MUSIC_FOOTER_TEST=1" TIMEOUT 60)
    add_test(NAME plugin_navigation COMMAND VideoVaultGalleryPerformanceTests)
    set_tests_properties(plugin_navigation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MVP_PLUGIN_NAVIGATION_TEST=1" TIMEOUT 60)
    add_test(NAME plugin_folder COMMAND VideoVaultGalleryPerformanceTests)
    set_tests_properties(plugin_folder PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MVP_PLUGIN_FOLDER_TEST=1" TIMEOUT 120)

    add_executable(VideoVaultTagFilterTests
        tests/tag_filter_button.cpp
        src/tag_filter_button.cpp
        src/theme.cpp
        include/videovault/app/tag_filter_button.hpp)
    target_include_directories(VideoVaultTagFilterTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VideoVaultTagFilterTests PRIVATE Qt5::Widgets)
    target_compile_features(VideoVaultTagFilterTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultTagFilterTests PROPERTIES AUTOMOC ON FOLDER "Tests")
    add_test(NAME tag_filter COMMAND VideoVaultTagFilterTests)
    set_tests_properties(tag_filter PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    mvp_copy_runtime_dlls(VideoVaultTagFilterTests)

    add_executable(VideoVaultThemeTests tests/theme.cpp src/theme.cpp src/tag_filter_button.cpp
        include/videovault/app/tag_filter_button.hpp)
    target_include_directories(VideoVaultThemeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VideoVaultThemeTests PRIVATE Qt5::Widgets)
    target_compile_features(VideoVaultThemeTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultThemeTests PROPERTIES AUTOMOC ON)
    add_test(NAME theme COMMAND VideoVaultThemeTests)
    set_tests_properties(theme PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    mvp_copy_runtime_dlls(VideoVaultThemeTests)

    add_executable(VideoVaultAppUpdateTests tests/app_update.cpp src/app_update.cpp)
    target_include_directories(VideoVaultAppUpdateTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VideoVaultAppUpdateTests PRIVATE MegaVaultNetwork VideoVault::Core Qt5::Core Qt5::Network)
    if(WIN32 OR MINGW)
        target_link_libraries(VideoVaultAppUpdateTests PRIVATE winhttp)
    endif()
    target_compile_features(VideoVaultAppUpdateTests PRIVATE cxx_std_20)
    add_test(NAME app_update COMMAND VideoVaultAppUpdateTests)
    set_tests_properties(app_update PROPERTIES TIMEOUT 30)
    mvp_copy_runtime_dlls(VideoVaultAppUpdateTests)

    add_executable(VideoVaultSettingsTests tests/settings_page.cpp
        src/settings_dialog.cpp src/in_window_sheet.cpp src/theme.cpp src/icons.cpp
        src/sync_state.cpp src/secure_input.cpp src/proxy_settings.cpp src/record_document.cpp
        resources/icons.qrc resources/resources.qrc
        include/videovault/app/settings_dialog.hpp include/videovault/app/in_window_sheet.hpp
        include/videovault/app/record_document.hpp include/videovault/app/sync_state.hpp)
    target_include_directories(VideoVaultSettingsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VideoVaultSettingsTests PRIVATE VideoVault::Core MegaVaultPlugins Qt5::Widgets Qt5::Concurrent)
    target_compile_features(VideoVaultSettingsTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultSettingsTests PROPERTIES AUTOMOC ON FOLDER "Tests")
    add_test(NAME settings_page COMMAND VideoVaultSettingsTests)
    set_tests_properties(settings_page PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    mvp_copy_runtime_dlls(VideoVaultSettingsTests)

    add_executable(VideoVaultSyncStateTests tests/sync_state.cpp)
    add_executable(VideoVaultSyncEngineTests tests/sync_engine.cpp)
    add_executable(VideoVaultSyncDialogTests tests/sync_page.cpp)
    foreach(_sync_test VideoVaultSyncStateTests VideoVaultSyncEngineTests VideoVaultSyncDialogTests)
        target_link_libraries(${_sync_test} PRIVATE MegaVaultSync)
    endforeach()
    add_test(NAME sync_state COMMAND VideoVaultSyncStateTests)
    add_test(NAME sync_engine COMMAND VideoVaultSyncEngineTests)
    set_tests_properties(sync_engine PROPERTIES SKIP_RETURN_CODE 77)
    # The page draws Lucide glyphs, and resources from a static library are not
    # carried into the binary: the qrc has to be compiled into the test itself.
    target_sources(VideoVaultSyncDialogTests PRIVATE resources/icons.qrc)
    set_target_properties(VideoVaultSyncDialogTests PROPERTIES AUTORCC ON)
    add_test(NAME sync_page COMMAND VideoVaultSyncDialogTests)
    set_tests_properties(sync_page PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # player_window.cpp draws Lucide glyphs, so the test compiles the helper and the
    # resources itself: a static library does not carry its qrc into the binary.
    add_executable(VideoVaultPlaybackTests
        tests/playback.cpp
        src/media_decoder.cpp
        src/player_window.cpp
        src/icons.cpp
        resources/icons.qrc
        resources/telegram_player.qrc
        include/videovault/app/media_decoder.hpp
        include/videovault/app/player_window.hpp
    )
    set_target_properties(VideoVaultPlaybackTests PROPERTIES
        AUTOMOC ON
        AUTORCC ON
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

    # Templates are the record container plus generators, so this links the same
    # record_document.cpp and the resources that carry the built-in templates.
    add_executable(VideoVaultTemplateFormatTests
        tests/template_format.cpp
        src/record_document.cpp
        resources/resources.qrc
        include/videovault/app/record_document.hpp
    )
    target_link_libraries(VideoVaultTemplateFormatTests
        PRIVATE
            Qt5::Core
    )
    target_include_directories(VideoVaultTemplateFormatTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_features(VideoVaultTemplateFormatTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultTemplateFormatTests PROPERTIES
        AUTORCC ON
        FOLDER "Tests"
    )
    add_test(NAME template_format COMMAND VideoVaultTemplateFormatTests)
    mvp_copy_runtime_dlls(VideoVaultTemplateFormatTests)
endif()
