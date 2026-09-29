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

# ---- 4. 可选：部署 Qt DLL ----
if ($Deploy) {
    $windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
    if (Test-Path $windeployqt) {
        & $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw $exe
        Write-Host "已用 windeployqt 部署运行时 DLL" -ForegroundColor Green
    } else {
        Write-Warning "未找到 windeployqt，跳过部署"
    }
}
