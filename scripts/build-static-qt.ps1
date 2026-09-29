# ---------------------------------------------------------------------------
#  build-static-qt.ps1 —— 从源码构建【静态】qtbase（Core/Gui/Widgets/Network）
#
#  背景：Qt 官方安装器只提供共享版（qconfig.pri 中 static 属于 disabled features），
#        要做出不依赖 Qt6*.dll 的单文件 exe，必须自建一份静态 Qt。
#
#  用法：
#    .\scripts\build-static-qt.ps1
#    .\scripts\build-static-qt.ps1 -WorkDir D:\qt-static -Version 6.11.2 -Jobs 6
#
#  耗时参考：i7-1165G7（4 核 8 线程）约 45~90 分钟。
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$WorkDir = 'D:\qt-static',
    [string]$Version = '6.11.2',
    [int]$Jobs = 6,
    [string]$Mirror = 'https://mirrors.tuna.tsinghua.edu.cn/qt/archive/qt',
    # 官方 6.11.2 qtbase 源码包的大小与 SHA-256（实测值），用于校验下载完整性
    [long]$ExpectedSize = 78588198,
    [string]$ExpectedSha256 = '8F8C16703A8170B235361AACDF0EC97D2445AE4E3E3D127EB1576F498269EF79'
)

$ErrorActionPreference = 'Stop'
$shortVersion = ($Version -split '\.')[0..1] -join '.'      # 6.11.2 -> 6.11
$archive = "qtbase-everywhere-src-$Version.zip"
$url = "$Mirror/$shortVersion/$Version/submodules/$archive"
$dlDir = Join-Path $WorkDir 'dl'
$srcDir = Join-Path $WorkDir 'src'
$srcPath = Join-Path $srcDir "qtbase-everywhere-src-$Version"
$buildPath = Join-Path $WorkDir 'build-qtbase'
$prefix = Join-Path $WorkDir 'install'

Write-Host "工作目录: $WorkDir" -ForegroundColor Cyan

# ---- 1. 下载源码（带大小与 SHA-256 校验，防止截断的同名文件被当成有效包）----
New-Item -ItemType Directory -Force $dlDir, $srcDir | Out-Null
$zip = Join-Path $dlDir $archive

function Test-SourceArchive {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $false }
    $item = Get-Item $Path
    if ($ExpectedSize -gt 0 -and $item.Length -ne $ExpectedSize) {
        Write-Warning "源码包大小不符：$($item.Length) B，期望 $ExpectedSize B"
        return $false
    }
    if ($ExpectedSha256) {
        $actual = (Get-FileHash $Path -Algorithm SHA256).Hash
        if ($actual -ne $ExpectedSha256.ToUpper()) {
            Write-Warning "源码包 SHA-256 不符：$actual，期望 $($ExpectedSha256.ToUpper())"
            return $false
        }
    }
    return $true
}

if (-not (Test-SourceArchive -Path $zip)) {
    if (Test-Path $zip) {
        Write-Warning "已存在的源码包未通过校验，删除后重新下载"
        Remove-Item $zip -Force
    }
    Write-Host "下载 $url" -ForegroundColor Cyan
    curl.exe -L --retry 3 --retry-delay 3 -o $zip $url
    if ($LASTEXITCODE -ne 0) { throw "下载失败（curl exit=$LASTEXITCODE）" }
    if (-not (Test-SourceArchive -Path $zip)) { throw "下载完成但校验仍不通过，请检查镜像或网络" }
} else {
    Write-Host "源码包已存在且校验通过，跳过下载: $zip"
}

# ---- 2. 解压（必须检查 tar 的退出码，否则解压失败会被误报成"找不到目录"）----
$fileCount = (Get-ChildItem $srcPath -Recurse -File -ErrorAction SilentlyContinue).Count
if ($fileCount -lt 20000) {
    Write-Host "解压源码（当前 $fileCount 个文件，需重新解压）..." -ForegroundColor Cyan
    Remove-Item $srcPath -Recurse -Force -ErrorAction SilentlyContinue
    tar.exe -xf $zip -C $srcDir
    if ($LASTEXITCODE -ne 0) { throw "解压失败（tar exit=$LASTEXITCODE）" }
    $fileCount = (Get-ChildItem $srcPath -Recurse -File -ErrorAction SilentlyContinue).Count
    if ($fileCount -lt 20000) { throw "解压后文件数异常（$fileCount），源码树不完整" }
}
Write-Host "源码文件数: $((Get-ChildItem $srcPath -Recurse -File).Count)"

# ---- 3. 组装工具链 PATH ----
$extraPaths = @()
foreach ($p in @('D:\Qt\Tools\mingw1310_64\bin', 'D:\Qt\Tools\Ninja', 'D:\Qt\Tools\CMake_64\bin')) {
    if (Test-Path $p) { $extraPaths += $p }
}
$env:PATH = ($extraPaths + $env:PATH) -join ';'
foreach ($tool in @('cmake', 'ninja', 'g++')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "找不到 $tool" }
}

# ---- 4. configure（静态）----
Write-Host "`n配置静态 qtbase ..." -ForegroundColor Cyan
cmake -S $srcPath -B $buildPath -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_INSTALL_PREFIX=$prefix `
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ `
    -DBUILD_SHARED_LIBS=OFF `
    -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF `
    -DFEATURE_sql=OFF -DFEATURE_testlib=OFF `
    -DFEATURE_schannel=ON -DFEATURE_openssl=OFF `
    -DFEATURE_system_zlib=OFF -DFEATURE_system_png=OFF -DFEATURE_system_jpeg=OFF `
    -DFEATURE_system_freetype=OFF -DFEATURE_system_harfbuzz=OFF -DFEATURE_system_pcre2=OFF
if ($LASTEXITCODE -ne 0) { throw "configure 失败" }

# ---- 5. 编译 + 安装 ----
Write-Host "`n编译（-j$Jobs）... 预计 45~90 分钟" -ForegroundColor Cyan
cmake --build $buildPath -- -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "编译失败" }

cmake --install $buildPath
if ($LASTEXITCODE -ne 0) { throw "安装失败" }

# ---- 6. 结果自检 ----
Write-Host "`n=== 静态 Qt 安装结果 ===" -ForegroundColor Green
Get-ChildItem (Join-Path $prefix 'lib') -Filter 'libQt6Core*' -ErrorAction SilentlyContinue |
    Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 2) } } | Format-Table -AutoSize
$hasDll = Test-Path (Join-Path $prefix 'bin\Qt6Core.dll')
if ($hasDll) {
    Write-Warning "install\bin 下存在 Qt6Core.dll —— 这不是静态构建，请检查 configure 参数"
} else {
    Write-Host "install\bin 下无 Qt6Core.dll —— 静态构建成功" -ForegroundColor Green
}
Write-Host "静态 Qt 前缀: $prefix" -ForegroundColor Green
