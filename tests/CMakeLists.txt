add_executable(VideoVaultCoreTests
    core_smoke.cpp
)

target_link_libraries(VideoVaultCoreTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreTests)

add_test(NAME VideoVaultCore.Smoke COMMAND VideoVaultCoreTests)

add_executable(VideoVaultCoreGalleryTests
    import_gallery.cpp
)

target_link_libraries(VideoVaultCoreGalleryTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreGalleryTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreGalleryTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreGalleryTests)

add_test(NAME VideoVaultCore.Gallery COMMAND VideoVaultCoreGalleryTests)

add_executable(VideoVaultCoreAdminTests
    vault_admin.cpp
)

target_link_libraries(VideoVaultCoreAdminTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreAdminTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreAdminTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreAdminTests)

add_test(NAME VideoVaultCore.Admin COMMAND VideoVaultCoreAdminTests)

add_executable(VideoVaultCoreReaderTests
    package_reader.cpp
)

target_link_libraries(VideoVaultCoreReaderTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreReaderTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreReaderTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreReaderTests)

add_test(NAME VideoVaultCore.Reader COMMAND VideoVaultCoreReaderTests)

add_executable(VideoVaultCoreMediaTests
    media.cpp
)

target_link_libraries(VideoVaultCoreMediaTests PRIVATE VideoVault::Core)
target_link_libraries(VideoVaultCoreMediaTests PRIVATE ${MVP_FFMPEG_LIBS})
if(WIN32)
    target_include_directories(VideoVaultCoreMediaTests PRIVATE "${MVP_VCPKG_PREFIX}/include")
elseif(ANDROID)
    target_include_directories(VideoVaultCoreMediaTests PRIVATE ${MVP_DEP_INCLUDE_DIRS})
endif()
target_compile_features(VideoVaultCoreMediaTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreMediaTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreMediaTests)

add_test(NAME VideoVaultCore.Media COMMAND VideoVaultCoreMediaTests)
set_tests_properties(VideoVaultCore.Media PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The playback test target is defined in qt-app/ (it needs Qt + FFmpeg), but
# ctest registration must happen after enable_testing() at the top level.
# On Android the Qt app is not part of the core cross-build, so the target
# does not exist there.
if(TARGET VideoVaultPlaybackTests)
    add_test(NAME VideoVault.Playback COMMAND $<TARGET_FILE:VideoVaultPlaybackTests>)
    set_tests_properties(VideoVault.Playback PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

# Defined in qt-app/ for the same reason as the playback test above.
if(TARGET VideoVaultRecordFormatTests)
    add_test(NAME VideoVaultRecords.Format COMMAND $<TARGET_FILE:VideoVaultRecordFormatTests>)
endif()

add_executable(VideoVaultCoreTagTests
    tags.cpp
)

target_link_libraries(VideoVaultCoreTagTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreTagTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreTagTests PROPERTIES FOLDER "Tests")

add_test(NAME VideoVaultCore.Tags COMMAND VideoVaultCoreTagTests)

add_executable(VideoVaultCoreRecordTests
    records.cpp
)

target_link_libraries(VideoVaultCoreRecordTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreRecordTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreRecordTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreRecordTests)

add_test(NAME VideoVaultCore.Records COMMAND VideoVaultCoreRecordTests)

# The encrypted package boundary used by self-hosted sync. The sender and
# receiver use different local vault passwords and share only a package secret.
add_executable(VideoVaultSyncPackageTests sync_packages.cpp)
target_link_libraries(VideoVaultSyncPackageTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultSyncPackageTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultSyncPackageTests PROPERTIES FOLDER "Tests")
mvp_copy_runtime_dlls(VideoVaultSyncPackageTests)
add_test(NAME VideoVaultCore.SyncPackages COMMAND VideoVaultSyncPackageTests)

# Provider audio enters through the shared memory-only import boundary on every
# platform. The test generates its audio fixture in RAM, never as a source file.
add_executable(VideoVaultPluginMemoryTests plugin_memory.cpp)
target_link_libraries(VideoVaultPluginMemoryTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultPluginMemoryTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultPluginMemoryTests PROPERTIES FOLDER "Tests")
mvp_copy_runtime_dlls(VideoVaultPluginMemoryTests)
add_test(NAME VideoVaultCore.PluginMemory COMMAND VideoVaultPluginMemoryTests)

find_package(Threads REQUIRED)
add_executable(MegaVaultExtensionTests extension_runtime.cpp)
target_link_libraries(MegaVaultExtensionTests PRIVATE MegaVaultExtensions Threads::Threads)
add_test(NAME VideoVault.ExtensionRuntime COMMAND MegaVaultExtensionTests)
set_tests_properties(VideoVault.ExtensionRuntime PROPERTIES TIMEOUT 20)

# Memory-security regression suite. It reads this process's own committed memory
# to prove that a hard lock leaves no usable master key behind, so it needs the
# core's internal headers (SensitiveBuffer / PackageReader / derive_master_key).
# It writes only to throwaway temporary vaults and never prints a secret.
add_executable(VideoVaultSecurityMemoryTests security_memory.cpp)
target_link_libraries(VideoVaultSecurityMemoryTests PRIVATE VideoVault::Core Threads::Threads)
target_include_directories(VideoVaultSecurityMemoryTests PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src")
if(WIN32)
    target_include_directories(VideoVaultSecurityMemoryTests PRIVATE
        "${MVP_VCPKG_PREFIX}/include")
elseif(ANDROID)
    target_include_directories(VideoVaultSecurityMemoryTests PRIVATE
        ${MVP_DEP_INCLUDE_DIRS})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SECMEM_SODIUM REQUIRED IMPORTED_TARGET libsodium)
    target_link_libraries(VideoVaultSecurityMemoryTests PRIVATE PkgConfig::SECMEM_SODIUM)
endif()
target_compile_features(VideoVaultSecurityMemoryTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultSecurityMemoryTests PROPERTIES FOLDER "Tests")
mvp_copy_runtime_dlls(VideoVaultSecurityMemoryTests)
add_test(NAME VideoVaultCore.SecurityMemory COMMAND VideoVaultSecurityMemoryTests)
set_tests_properties(VideoVaultCore.SecurityMemory PROPERTIES TIMEOUT 300)

# Progressive (streamed) provider import: bytes are encrypted into the vault as
# they arrive (no plaintext staging file, no whole-track buffer), and only a
# complete stream becomes a library item with provider provenance.
add_executable(VideoVaultStreamImportTests stream_import.cpp)
target_link_libraries(VideoVaultStreamImportTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultStreamImportTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultStreamImportTests PROPERTIES FOLDER "Tests")
mvp_copy_runtime_dlls(VideoVaultStreamImportTests)
add_test(NAME VideoVaultCore.StreamImport COMMAND VideoVaultStreamImportTests)
set_tests_properties(VideoVaultCore.StreamImport PROPERTIES TIMEOUT 180)

# Ordered music collections in SQLCipher, real media membership validation,
# migration rollback and whole-playlist transaction failure injection.
add_executable(VideoVaultMusicPlaylistTests music_playlists.cpp)
target_link_libraries(VideoVaultMusicPlaylistTests PRIVATE VideoVault::Core)
target_include_directories(VideoVaultMusicPlaylistTests PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
if(WIN32)
    target_include_directories(VideoVaultMusicPlaylistTests PRIVATE "${MVP_VCPKG_PREFIX}/include")
elseif(ANDROID)
    target_include_directories(VideoVaultMusicPlaylistTests PRIVATE ${MVP_DEP_INCLUDE_DIRS})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PLAYLIST_DEPS REQUIRED IMPORTED_TARGET sqlcipher libsodium libavformat libavutil)
    pkg_get_variable(PLAYLIST_SQLCIPHER_INCLUDEDIR sqlcipher includedir)
    if(EXISTS "${PLAYLIST_SQLCIPHER_INCLUDEDIR}/sqlcipher/sqlite3.h")
        target_include_directories(VideoVaultMusicPlaylistTests PRIVATE "${PLAYLIST_SQLCIPHER_INCLUDEDIR}")
    endif()
    target_link_libraries(VideoVaultMusicPlaylistTests PRIVATE PkgConfig::PLAYLIST_DEPS)
endif()
target_compile_definitions(VideoVaultMusicPlaylistTests PRIVATE SQLITE_HAS_CODEC)
target_compile_features(VideoVaultMusicPlaylistTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultMusicPlaylistTests PROPERTIES FOLDER "Tests")
mvp_copy_runtime_dlls(VideoVaultMusicPlaylistTests)
add_test(NAME VideoVaultCore.MusicPlaylists COMMAND VideoVaultMusicPlaylistTests)
set_tests_properties(VideoVaultCore.MusicPlaylists PROPERTIES TIMEOUT 180)
