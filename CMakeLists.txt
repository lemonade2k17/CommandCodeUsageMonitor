cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
#  Command Code 套餐用量查看器
#  目标：Qt 6.11.2 + 静态链接（单文件 exe，不依赖任何 Qt6*.dll）
# ---------------------------------------------------------------------------
project(CommandCodeUsageMonitor
        VERSION 1.0.0
        DESCRIPTION "Command Code plan usage monitor (static Qt build)"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

qt_add_executable(commandcode-usage
    WIN32
    src/main.cpp
    src/AppConfig.h
    src/AppConfig.cpp
    src/AppTheme.h
    src/AppTheme.cpp
    src/UsageTypes.h
    src/CommandCodeApi.h
    src/CommandCodeApi.cpp
    src/SegmentedBar.h
    src/SegmentedBar.cpp
    src/SettingsDialog.h
    src/SettingsDialog.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/TrayController.h
    src/TrayController.cpp
    src/TaskbarProgress.h
    src/TaskbarProgress.cpp
)

target_include_directories(commandcode-usage PRIVATE src)
target_link_libraries(commandcode-usage PRIVATE Qt6::Widgets Qt6::Network)

# 桌面集成所需的 Win32 系统库：
#   ole32   —— CoInitializeEx / CoCreateInstance / CoUninitialize（任务栏 COM 接口）
#   uuid    —— IID_ITaskbarList3 / CLSID_TaskbarList 的 GUID 符号解析（静态导入库）
#   shell32 —— 任务栏相关辅助 API
#   user32 / gdi32 —— CreateDIBSection / CreateIconIndirect / GetDC 等图标与绘制 API
# 这些全部是 Windows 自带组件，不会破坏"单文件、不依赖第三方 DLL"的静态链接目标。
if(WIN32)
    target_link_libraries(commandcode-usage PRIVATE ole32 uuid shell32 user32 gdi32)
endif()

# 静态链接：GCC 运行时 + Qt 全部静态化，并 strip 掉符号
if(MINGW)
    target_link_options(commandcode-usage PRIVATE
        -static -static-libgcc -static-libstdc++ -s)
endif()

# 判断本次使用的是共享版还是静态版 Qt（以安装目录里有没有 Qt6Core.dll 为准）
set(CCUM_STATIC_QT FALSE)
if(DEFINED QT6_INSTALL_PREFIX)
    message(STATUS "Qt 安装目录 -> ${QT6_INSTALL_PREFIX}")
    if(EXISTS "${QT6_INSTALL_PREFIX}/bin/Qt6Core.dll")
        # 本工程只做静态链接：一旦发现共享版 Qt 前缀就直接终止配置，避免误产出
        # 依赖 Qt6*.dll 的动态产物（那类产物还会被 PATH 上其它软件自带的旧版
        # Qt 干扰，实测会报"无法定位程序输入点"）。
        message(FATAL_ERROR
            "本工程只支持【静态链接】：${QT6_INSTALL_PREFIX} 是共享版 Qt"
            "（存在 bin/Qt6Core.dll），继续配置会产出依赖 Qt6*.dll 的产物。\n"
            "请改用静态 Qt 前缀（默认 D:/qt-static/install，"
            "由 scripts/build-static-qt.ps1 生成），或直接运行 scripts/build-static.ps1。")
    else()
        set(CCUM_STATIC_QT TRUE)
        message(STATUS "本次使用【静态 Qt】：产物应为单文件 exe。")
    endif()
elseif(NOT BUILD_SHARED_LIBS)
    # 没有 QT6_INSTALL_PREFIX 时退而求其次：项目自身 BUILD_SHARED_LIBS 为假
    set(CCUM_STATIC_QT TRUE)
endif()

# 静态 Qt 会禁用插件的动态加载，必须把平台插件与 TLS 后端显式链进可执行文件，
# 否则程序能编译链接通过，但运行时报 "no Qt platform plugin could be initialized"
# 或 HTTPS 请求报 "TLS initialization failed"。
if(CCUM_STATIC_QT)
    find_package(Qt6 QUIET OPTIONAL_COMPONENTS Gui Network)
    if(TARGET Qt6::QWindowsIntegrationPlugin)
        qt_import_plugins(commandcode-usage
            INCLUDE_BY_TYPE platforms Qt6::QWindowsIntegrationPlugin)
        message(STATUS "已显式导入平台插件 QWindowsIntegrationPlugin")
    else()
        message(STATUS "未找到 Qt6::QWindowsIntegrationPlugin 目标，依赖 Qt 默认静态插件导入")
    endif()
    if(TARGET Qt6::QSchannelBackendPlugin)
        qt_import_plugins(commandcode-usage
            INCLUDE_BY_TYPE tls Qt6::QSchannelBackendPlugin)
        message(STATUS "已显式导入 TLS 后端 QSchannelBackendPlugin")
    else()
        message(STATUS "未找到 Qt6::QSchannelBackendPlugin 目标，依赖 Qt 默认静态插件导入")
    endif()
endif()

install(TARGETS commandcode-usage RUNTIME DESTINATION bin)
