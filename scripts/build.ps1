# ---------------------------------------------------------------------------
#  build.ps1 —— 用【已安装的 Qt】构建本项目（共享版，最快）
#
#  用法：
#    .\scripts\build.ps1                       # 自动探测 Qt 与工具链
#    .\scripts\build.ps1 -QtDir D:\Qt\6.11.2\mingw_64
#    .\scripts\build.ps1 -Deploy               # 额外执行 windeployqt 把 DLL 拷到产物旁
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$QtDir = '',
    [string]$BuildDir = 'build',
    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot     # 项目根目录

# ---- 1. 定位 Qt ----
if (-not $QtDir) {
    $candidates = @()
    foreach ($base in @('D:\Qt', 'C:\Qt')) {
        if (Test-Path $base) {
            $candidates += Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^\d+\.\d+' } |
                ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
                Where-Object { $_.Name -match 'mingw|msvc' } |
                Select-Object -ExpandProperty FullName
        }
    }
    $QtDir = $candidates | Select-Object -First 1
}
if (-not $QtDir -or -not (Test-Path "$QtDir\lib\cmake\Qt6")) {
    throw "未找到可用的 Qt6 安装目录，请用 -QtDir 指定（例如 -QtDir D:\Qt\6.11.2\mingw_64）"
}
Write-Host "使用 Qt: $QtDir" -ForegroundColor Cyan

# ---- 2. 组装工具链 PATH（Qt 自带的 MinGW / Ninja / CMake 优先）----
$extraPaths = @()
if (Test-Path 'D:\Qt\Tools') {
    foreach ($p in @('mingw1310_64\bin', 'mingw_64\bin', 'Ninja', 'CMake_64\bin')) {
        $full = Join-Path 'D:\Qt\Tools' $p
        if (Test-Path $full) { $extraPaths += $full }
    }
}
if ($QtDir -match 'mingw') { $extraPaths += (Join-Path $QtDir 'bin') }
$env:PATH = ($extraPaths + $env:PATH) -join ';'

foreach ($tool in @('cmake', 'ninja')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "找不到 $tool，请确认 Qt 工具链已安装或加入 PATH"
    }
}

# ---- 3. 配置 + 构建 ----
$buildPath = Join-Path $root $BuildDir
cmake -S $root -B $buildPath -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtDir"
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

cmake --build $buildPath
if ($LASTEXITCODE -ne 0) { throw "编译失败" }

$exe = Join-Path $buildPath 'commandcode-usage.exe'
Write-Host "`n构建完成: $exe" -ForegroundColor Green

# ---- 4. 可选：部署 Qt 运行库 ----
# 没有这一步时，exe 旁没有任何 DLL，加载器会顺着 PATH 找 Qt6Core.dll——PATH 上
# 其它软件自带的旧版 Qt（实测 Snipaste 目录里的 6.2.4 会被命中）缺少新版导出符号，
# 程序会直接报"无法定位程序输入点"而无法启动。部署后 exe 旁的 DLL 优先级最高。
if ($Deploy) {
    $windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
    if (Test-Path $windeployqt) {
        # --compiler-runtime 一并部署 MinGW 编译器运行时（libstdc++-6 / libgcc_s_seh-1 /
        # libwinpthread-1）：PATH 上没有任何目录提供这三个 DLL，缺了它们下一个错就是
        # "丢失 libstdc++-6.dll"。
        & $windeployqt --release --compiler-runtime --no-translations --no-system-d3d-compiler --no-opengl-sw $exe
        if ($LASTEXITCODE -ne 0) { throw "windeployqt 部署失败（退出码 $LASTEXITCODE）" }
        Write-Host "已用 windeployqt 部署 Qt 运行库与编译器运行时" -ForegroundColor Green
    } else {
        Write-Warning "未找到 windeployqt，跳过部署"
    }
} else {
    Write-Warning "未部署运行库（-Deploy）：此目录缺少 Qt DLL，直接双击 exe 会加载到 PATH 上其它软件自带的旧版 Qt 而报错。分发或本机运行请加 -Deploy，或改用静态版。"
}

# ---- 5. 交付前把 exe 的完整性级别显式设为 Medium ----
# 与 build-static.ps1 相同的理由：项目目录被会话沙箱打了低完整性（Low Integrity
# Level）强制标签并被子项继承，双击本 exe 会得到低完整性进程，导致任务栏角标被
# UIPI 拒绝（E_ACCESSDENIED）、托盘注册不可靠、QStandardPaths 把配置重定向到
# LocalLow。显式的文件级标签优先于继承标签；每次重新链接都会生成新文件（重新继承
# 目录标签），因此这一步必须放在每次构建的最后（部署动作之后），不能只做一次。
$icacls = Join-Path $env:SystemRoot 'System32\icacls.exe'
Write-Host "`n=== 设置 exe 完整性级别为 Medium ===" -ForegroundColor Cyan
& $icacls $exe /setintegritylevel M
if ($LASTEXITCODE -ne 0) { throw "icacls /setintegritylevel 失败（退出码 $LASTEXITCODE）" }
# 回显标签行，便于在构建日志里核对（应出现 Medium Mandatory Level）
& $icacls $exe | Select-String 'Mandatory'
