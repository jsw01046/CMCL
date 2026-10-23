cmake_minimum_required(VERSION 3.25)

project(CMCL VERSION 0.5.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CMCL_BUILD_TESTS "Build unit tests" ON)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

add_library(cl_core STATIC
    src/core/encoding.cpp
    src/core/log.cpp
    src/core/paths.cpp
    src/core/process.cpp
    src/core/archive.cpp
    src/core/crypto.cpp
    src/core/http.cpp
    src/mc/version.cpp
    src/mc/instance.cpp
    src/mc/java.cpp
    src/mc/launch.cpp
    src/mc/diagnostics.cpp
    src/mc/mirrors.cpp
    src/mc/download.cpp
    src/mc/jdk.cpp
    src/mc/loader.cpp
    src/mc/modrinth.cpp
    src/mc/modpack.cpp
    src/mc/auth.cpp
)

target_include_directories(cl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cl_core PUBLIC nlohmann_json::nlohmann_json ZLIB::ZLIB
    bcrypt crypt32 shell32 winhttp)
target_compile_definitions(cl_core PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

if (MSVC)
    target_compile_options(cl_core PUBLIC /utf-8 /W4 /permissive- /Zc:__cplusplus)
endif()

add_executable(cmcl src/app/main.cpp)
target_link_libraries(cmcl PRIVATE cl_core)

# WebView2 图形界面
add_executable(cmcl-ui WIN32
    src/ui/main.cpp
    src/ui/host.cpp
    src/ui/cmcl.rc
)
target_link_libraries(cmcl-ui PRIVATE
    cl_core
    unofficial::webview2::webview2
    ole32
    oleaut32
    shlwapi
    version
    dwmapi
    comdlg32
)
if (MSVC)
    target_link_options(cmcl-ui PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/ui/app.manifest")
endif()
add_custom_command(TARGET cmcl-ui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/web"
        "$<TARGET_FILE_DIR:cmcl-ui>/web"
    COMMENT "Copying web assets"
)

if (CMCL_BUILD_TESTS)
    enable_testing()
    add_executable(cl_tests tests/version_test.cpp)
    target_link_libraries(cl_tests PRIVATE cl_core)
    add_test(NAME version_test COMMAND cl_tests)

    add_executable(diagnostics_tests tests/diagnostics_test.cpp)
    target_link_libraries(diagnostics_tests PRIVATE cl_core)
    add_test(NAME diagnostics_test COMMAND diagnostics_tests)

    add_executable(instance_tests tests/instance_test.cpp)
    target_link_libraries(instance_tests PRIVATE cl_core)
    add_test(NAME instance_test COMMAND instance_tests)

    # 手动运行的真实下载/修复探针（需要联网，不加入 ctest）。
    add_executable(net_probe tests/net_probe.cpp)
    target_link_libraries(net_probe PRIVATE cl_core)

    # 微软设备码登录探针（需要联网并手动完成一次登录）。
    add_executable(auth_probe tests/auth_probe.cpp)
    target_link_libraries(auth_probe PRIVATE cl_core)
endif()
