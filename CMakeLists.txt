cmake_minimum_required(VERSION 3.14)

project(LocalAppDataCleaner VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 离线构建：优先使用 EUI-NEO 仓库内 3rd/ 自带的第三方源码，不触发网络下载。
# 需要联网自动拉取时改用 -DEUI_DEPS_MODE=auto。
set(EUI_DEPS_MODE "bundled" CACHE STRING "Third-party dependency mode: bundled, auto, or fetch")

# -----------------------------------------------------------------------------
# 依赖：EUI-NEO
#
# 三种情况按顺序处理，保证"clone 下来直接编译"和"离线本地编译"都能用：
#   1. -DEUI_NEO_SOURCE_DIR=<路径> 显式指定；
#   2. 自动在当前目录下找 EUI-NEO-main / 3rd/EUI-NEO / EUI-NEO；
#   3. 都没有就用 FetchContent 拉取（GitHub 不通时自动切 AtomGit 镜像）。
# 想离线构建：把 EUI-NEO clone 到本目录下的 EUI-NEO-main/ 即可。
# -----------------------------------------------------------------------------
set(EUI_NEO_SOURCE_DIR "" CACHE PATH "EUI-NEO 源码目录；留空则自动查找或联网拉取")

if(NOT EUI_NEO_SOURCE_DIR)
    foreach(_eui_candidate "EUI-NEO-main" "3rd/EUI-NEO" "EUI-NEO")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_eui_candidate}/CMakeLists.txt")
            set(EUI_NEO_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${_eui_candidate}")
            break()
        endif()
    endforeach()
endif()

if(EUI_NEO_SOURCE_DIR)
    message(STATUS "Using local EUI-NEO source: ${EUI_NEO_SOURCE_DIR}")
    add_subdirectory("${EUI_NEO_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/EUI-NEO")
else()
    message(STATUS "Local EUI-NEO not found; fetching it (GitHub, fallback AtomGit mirror)")
    include(FetchContent)

    set(eui_neo_repository "https://github.com/sudoevolve/EUI-NEO.git")
    execute_process(
        COMMAND git ls-remote ${eui_neo_repository} HEAD
        RESULT_VARIABLE eui_neo_github_status
        OUTPUT_QUIET ERROR_QUIET
        TIMEOUT 8
    )
    if(NOT eui_neo_github_status EQUAL 0)
        set(eui_neo_repository "https://atomgit.com/sudoevolve/EUI-NEO.git")
    endif()

    FetchContent_Declare(eui_neo
        GIT_REPOSITORY ${eui_neo_repository}
        GIT_TAG main
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(eui_neo)
endif()

# -----------------------------------------------------------------------------
# 应用图标
#
# EUI-NEO 在 configure 阶段会写一个 eui_app_icon.rc，指向它自己的
# EUI-NEO-main/assets/icon.ico；而且这个 .ico 没有被登记成 .rc 的依赖，
# 所以只替换 ico、不重新 configure 的话，windres 不会重编，exe 图标看起来
# “换了没用”。这里改成用本工程自己的图标：
#   1) 先清掉框架准备的图标资源，避免两份 ICON 资源打架；
#   2) 把 19icon.ico 登记为 configure 依赖，ico 一变就重新生成 .rc；
#   3) 自己写 .rc 并加进目标。
# 运行时窗口图标用 19icon.png（见 main.cpp 的 .iconPath）。
# -----------------------------------------------------------------------------
set(APP_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/assets/19icon.png")
set(APP_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/19icon.ico")

if(WIN32 AND EXISTS "${APP_ICON_ICO}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${APP_ICON_ICO}")
endif()

set_property(GLOBAL PROPERTY EUI_NEO_APP_ICON_RESOURCE "")

add_executable(localappdata_cleaner "src/main.cpp")
eui_neo_configure_app(localappdata_cleaner)

if(WIN32 AND EXISTS "${APP_ICON_ICO}")
    # 图标 + VERSIONINFO（产品名 / 版本 / 版权）一起由模板生成，
    # 版本号跟随 project(... VERSION x.y.z)。
    file(TO_NATIVE_PATH "${APP_ICON_ICO}" APP_ICON_ICO_NATIVE)
    string(REPLACE "\\" "\\\\" APP_ICON_ICO_RC "${APP_ICON_ICO_NATIVE}")
    set(APP_VERSION_RC
        "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")

    set(APP_RESOURCES_RC "${CMAKE_CURRENT_BINARY_DIR}/localappdata_cleaner_resources.rc")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/app_resources.rc.in"
        "${APP_RESOURCES_RC}"
        @ONLY
    )
    # .rc 按 UTF-8 保存，告诉 windres 用同样的代码页读取，
    # 否则版权符号 © 会被按系统 ANSI（中文 Windows 下是 GBK）解释成 "?"。
    set_source_files_properties("${APP_RESOURCES_RC}" PROPERTIES
        COMPILE_OPTIONS "--codepage=65001")
    target_sources(localappdata_cleaner PRIVATE "${APP_RESOURCES_RC}")
endif()

# 原生「保存文件」「选择文件夹」对话框。
if(WIN32)
    target_link_libraries(localappdata_cleaner PRIVATE comdlg32 shell32 ole32)
endif()

# 应用自带素材（文件夹图标、窗口图标）复制到可执行文件旁的 assets/，
# 与 EUI-NEO 自己部署的字体、图标资源放在一起。
add_custom_command(
    TARGET localappdata_cleaner POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/folder.png"
            "$<TARGET_FILE_DIR:localappdata_cleaner>/assets/folder.png"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${APP_ICON_PNG}"
            "$<TARGET_FILE_DIR:localappdata_cleaner>/assets/19icon.png"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/19icon.ico"
            "$<TARGET_FILE_DIR:localappdata_cleaner>/assets/19icon.ico"
    COMMENT "Deploying app assets and icons"
    VERBATIM
)
