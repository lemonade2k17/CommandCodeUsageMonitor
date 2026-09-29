# ---------------------------------------------------------------------------
#  build-static.ps1 —— 用【自建的静态 Qt】构建本项目，产出单文件 exe
#
#  前置：先运行 .\scripts\build-static-qt.ps1 得到静态 Qt 前缀（默认 D:\qt-static\install）
#
#  用法：
#    .\scripts\build-static.ps1
#    .\scripts\build-static.ps1 -QtStaticPrefix D:\qt-static\install
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$QtStaticPrefix = 'D:\qt-static\install',
    [string]$BuildDir = 'build-static',
    [switch]$KeepGoingOnDll
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path (Join-Path $QtStaticPrefix 'lib\cmake\Qt6'))) {
    throw "未找到静态 Qt：$QtStaticPrefix。请先运行 .\scripts\build-static-qt.ps1"
}

# 反例检查：如果前缀下有 Qt6Core.dll，那它其实是共享版
if (Test-Path (Join-Path $QtStaticPrefix 'bin\Qt6Core.dll')) {
    Write-Warning "$QtStaticPrefix 下存在 Qt6Core.dll —— 这是共享版 Qt，产物仍会依赖 Qt DLL"
    if (-not $KeepGoingOnDll) { throw "请改用静态 Qt，或显式加 -KeepGoingOnDll 继续" }
}

# ---- 工具链 PATH ----
$extraPaths = @()
foreach ($p in @('D:\Qt\Tools\mingw1310_64\bin', 'D:\Qt\Tools\Ninja', 'D:\Qt\Tools\CMake_64\bin', (Join-Path $QtStaticPrefix 'bin'))) {
    if (Test-Path $p) { $extraPaths += $p }
}
$env:PATH = ($extraPaths + $env:PATH) -join ';'

# 静态构建必须用【全新的空目录】，否则 CMake 缓存里的 Qt6_DIR 仍指向动态 Qt，
# 会出现"以为在静态构建、实际链的仍是 Qt6*.dll"的假通过。
$buildPath = Join-Path $root $BuildDir
if (Test-Path (Join-Path $buildPath 'CMakeCache.txt')) {
    $cached = Select-String -Path (Join-Path $buildPath 'CMakeCache.txt') -Pattern '^CMAKE_PREFIX_PATH' |
        Select-Object -First 1
    if ($cached -and $cached.Line -notmatch [regex]::Escape($QtStaticPrefix)) {
        Write-Host "检测到缓存指向其它 Qt，删除旧构建目录以保证干净配置" -ForegroundColor Yellow
        Remove-Item $buildPath -Recurse -Force
    }
}

Write-Host "配置（静态 Qt: $QtStaticPrefix）..." -ForegroundColor Cyan
cmake -S $root -B $buildPath -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtStaticPrefix"
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

Write-Host "编译..." -ForegroundColor Cyan
cmake --build $buildPath
if ($LASTEXITCODE -ne 0) { throw "编译失败" }

$exe = Join-Path $buildPath 'commandcode-usage.exe'
Write-Host "`n产物: $exe" -ForegroundColor Green

# ---- 验收：导入表必须没有 Qt6*.dll ----
$objdump = 'D:\Qt\Tools\mingw1310_64\bin\objdump.exe'
if (Test-Path $objdump) {
    Write-Host "`n=== 依赖的 DLL ===" -ForegroundColor Cyan
    $imports = & $objdump -p $exe | Select-String 'DLL Name' | ForEach-Object { $_.Line.Trim() }
    $imports | ForEach-Object { Write-Host "  $_" }
    $qtDeps = $imports | Where-Object { $_ -match 'Qt6' }
    if ($qtDeps) {
        Write-Warning "仍依赖 Qt DLL: $($qtDeps -join ', ') —— 静态链接未生效"
    } else {
        Write-Host "`n通过：不依赖任何 Qt6*.dll，可单文件分发" -ForegroundColor Green
    }
}

# ---- 交付前把 exe 的完整性级别显式设为 Medium ----
# 背景：开发机的会话沙箱给项目所在目录打了低完整性（Low Integrity Level，低完整性级别）
# 强制标签并被子项继承，Windows 会按"映像文件的强制标签"降权启动进程——即使从
# 资源管理器（中完整性）双击，本 exe 启动的进程也是低完整性。后果有三：
#   ① 任务栏 ITaskbarList3（COM，组件对象模型）调用被 UIPI（User Interface Privilege
#      Isolation，用户界面特权隔离）拒绝，进度条与角标全部失效（E_ACCESSDENIED）；
#   ② QSystemTrayIcon 的注册可能被外壳拒绝，托盘图标不出现；
#   ③ QStandardPaths 把配置目录重定向到 LocalLow，配置位置随启动方式漂移。
# 显式的文件级标签优先于从目录继承的标签，因此构建末尾统一打一次 Medium 标签，
# 使双击运行恢复为正常的中完整性进程。每次重新链接都会生成新文件（重新继承目录
# 标签），所以这一步必须放在每次构建的最后，不能只做一次。
$icacls = Join-Path $env:SystemRoot 'System32\icacls.exe'
Write-Host "`n=== 设置 exe 完整性级别为 Medium ===" -ForegroundColor Cyan
& $icacls $exe /setintegritylevel M
if ($LASTEXITCODE -ne 0) { throw "icacls /setintegritylevel 失败（退出码 $LASTEXITCODE）" }
# 回显标签行，便于在构建日志里直接核对（应出现 Medium Mandatory Level）
& $icacls $exe | Select-String 'Mandatory'
