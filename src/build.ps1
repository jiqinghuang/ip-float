# 重新编译 ip-float.exe（MinGW g++ / windres）
#
# 默认只编译，不签名：构建不应该有“创建根证书”这种副作用。
# 需要签名时显式加 -Sign，或单独跑 sign.ps1。
#
# 用法：
#   pwsh -File src\build.ps1
#   pwsh -File src\build.ps1 -Test            # 编译 + 跑纯逻辑单测
#   pwsh -File src\build.ps1 -Sign            # 编译 + 签名
#   pwsh -File src\build.ps1 -MingwBin C:\msys64\mingw64\bin -OutDir dist -Werror
[CmdletBinding()]
param(
    # 留空时自动探测：-MingwBin > MINGW_BIN > MINGW_HOME\bin > PATH > 常见安装目录
    [string] $MingwBin = '',
    [string] $OutDir,
    [switch] $Sign,
    [switch] $Test,
    [switch] $Werror
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# 原生命令往 stderr 写东西时不要当成 PowerShell 异常（否则真实退出码会被掩盖）
$PSNativeCommandUseErrorActionPreference = $false

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here

# 版本号在三个地方各有一份（MSYS2 的 windres 不允许 version.h 预处理器 include）。
# 发版前最容易漏改，所以在每次构建前先把三处一致性钉死。
function Assert-VersionConsistency {
    $versionText = Get-Content -LiteralPath (Join-Path $here 'version.h') -Raw
    $match = [regex]::Match($versionText, '#define\s+IPFLOAT_VERSION_A\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
    if (-not $match.Success) { throw 'version.h 里找不到 IPFLOAT_VERSION_A "x.y.z"' }

    $version = $match.Groups[1].Value
    $parts = $version.Split('.')
    $major = [int]$parts[0]
    $minor = [int]$parts[1]
    $patch = [int]$parts[2]

    $rcText = Get-Content -LiteralPath (Join-Path $here 'app.rc') -Raw
    $rcFileVersion = 'FILEVERSION {0},{1},{2},0' -f $major, $minor, $patch
    if ($rcText -notmatch [regex]::Escape($rcFileVersion)) {
        throw "app.rc 的 FILEVERSION 与 version.h 不一致（期望 $rcFileVersion）"
    }
    $rcProductVersion = 'PRODUCTVERSION {0},{1},{2},0' -f $major, $minor, $patch
    if ($rcText -notmatch [regex]::Escape($rcProductVersion)) {
        throw "app.rc 的 PRODUCTVERSION 与 version.h 不一致（期望 $rcProductVersion）"
    }
    foreach ($key in @('FileVersion', 'ProductVersion')) {
        $valueLine = 'VALUE "' + $key + '", "' + $version + '"'
        if ($rcText -notmatch [regex]::Escape($valueLine)) {
            throw "app.rc 的 $key 与 version.h 不一致（期望 $version）"
        }
    }

    $manifestText = Get-Content -LiteralPath (Join-Path $here 'app.manifest') -Raw
    $manifestIdentity = 'assemblyIdentity type="win32" name="ip-float" version="' + $version + '.0"'
    if ($manifestText -notmatch [regex]::Escape($manifestIdentity)) {
        throw "app.manifest 的 assemblyIdentity 与 version.h 不一致（期望 $version.0）"
    }

    Write-Host "版本一致性检查通过：$version"
}

Assert-VersionConsistency

if (-not $OutDir) {
    $OutDir = $root
} elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    # 相对路径按调用者当前目录解析，然后在 Push-Location src 之前固定成绝对路径
    $OutDir = Join-Path (Get-Location).Path $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

function Test-MingwDir([string] $dir) {
    if (-not $dir) { return $false }
    return (Test-Path -LiteralPath (Join-Path $dir 'g++.exe')) -and
           (Test-Path -LiteralPath (Join-Path $dir 'windres.exe'))
}

if ($MingwBin) {
    if (-not [System.IO.Path]::IsPathRooted($MingwBin)) {
        $MingwBin = Join-Path (Get-Location).Path $MingwBin
    }
    $MingwBin = [System.IO.Path]::GetFullPath($MingwBin)
} else {
    $candidates = @()
    if ($env:MINGW_BIN)  { $candidates += $env:MINGW_BIN }
    if ($env:MINGW_HOME) { $candidates += (Join-Path $env:MINGW_HOME 'bin') }
    $gppCmd = Get-Command g++.exe -CommandType Application -ErrorAction SilentlyContinue |
              Select-Object -First 1
    if ($gppCmd) { $candidates += (Split-Path -Parent $gppCmd.Source) }
    $candidates += @(
        'D:\Mingw64\bin',
        'C:\msys64\mingw64\bin',
        'C:\msys64\ucrt64\bin',
        'C:\msys64\clang64\bin'
    )
    foreach ($candidate in $candidates) {
        if (Test-MingwDir $candidate) { $MingwBin = $candidate; break }
    }
    if (-not $MingwBin) {
        throw '找不到包含 g++.exe / windres.exe 的 MinGW 目录。请用 -MingwBin 指定，或把 MinGW 的 bin 放进 PATH。'
    }
}
if (-not (Test-MingwDir $MingwBin)) {
    throw "MinGW 目录里缺少 g++.exe 或 windres.exe：$MingwBin"
}

$gpp     = Join-Path $MingwBin 'g++.exe'
$windres = Join-Path $MingwBin 'windres.exe'
foreach ($tool in @($gpp, $windres)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "缺少工具：$tool" }
}

$exe = Join-Path $OutDir 'ip-float.exe'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$sources = @(
    'main.cpp', 'app.cpp', 'config.cpp', 'http.cpp', 'json.cpp', 'ipinfo.cpp',
    'card.cpp', 'settings_dlg.cpp', 'detail_dlg.cpp', 'tray.cpp', 'selftest.cpp', 'util.cpp',
    'netinfo.cpp', 'probe.cpp', 'diag.cpp'
) | ForEach-Object { Join-Path $here $_ }

$warn = @('-Wall', '-Wextra')
if ($Werror) { $warn += '-Werror' }

Push-Location $here
# windres 通过 popen 调用 "gcc -E" 预处理 .rc；在纯 PowerShell/CI 环境里
# 既找不到 gcc（PATH 里没有 MinGW），popen 也可能被拦。两件事一起解决：
#   · 把 MinGW 目录临时放进 PATH，gcc 能被原生找到
#   · --use-temp-file 让 windres 走临时文件而不是 popen（MSYS2 的已知坑）
$oldPath = $env:PATH
$env:PATH = "$MingwBin;$env:PATH"
try {
    & $windres --use-temp-file -I $here app.rc -o app.res.o
    if ($LASTEXITCODE -ne 0) { throw "windres 失败（exit $LASTEXITCODE）" }

    & $gpp -std=gnu++17 -O2 -DUNICODE -D_UNICODE @warn -mwindows -static -static-libgcc -static-libstdc++ `
        -o $exe $sources (Join-Path $here 'app.res.o') `
        -lgdiplus -lwinhttp -lshell32 -luser32 -lgdi32 -lole32 -liphlpapi -lws2_32 -ldwmapi -ldnsapi
    if ($LASTEXITCODE -ne 0) { throw "编译失败（exit $LASTEXITCODE）" }

    Unblock-File -LiteralPath $exe -ErrorAction SilentlyContinue
    Write-Host "已重新编译: $exe"
} finally {
    Remove-Item (Join-Path $here 'app.res.o') -Force -ErrorAction SilentlyContinue
    $env:PATH = $oldPath
    Pop-Location
}

if ($Test) {
    # 每个套件单独编、单独跑；日志写 TEMP，避免默认 OutDir=仓库根目录时污染 git status
    $suites = @(
        @{ Name = 'test_ipinfo'; Src = 'tests\test_ipinfo.cpp'; Extra = @('ipinfo.cpp', 'http.cpp') },
        @{ Name = 'test_probe';  Src = 'tests\test_probe.cpp';  Extra = @('probe.cpp') },
        @{ Name = 'test_config'; Src = 'tests\test_config.cpp'; Extra = @('config.cpp') }
    )

    foreach ($suite in $suites) {
        $testExe = Join-Path $OutDir ($suite.Name + '.exe')
        $outLog = Join-Path $env:TEMP ('ip-float-' + $suite.Name + '-build.log')
        $testArgs = @('-std=gnu++17', '-O2', '-DUNICODE', '-D_UNICODE') + $warn + @(
            '-o', $testExe,
            (Join-Path $root $suite.Src))
        foreach ($extra in $suite.Extra) { $testArgs += (Join-Path $here $extra) }
        $testArgs += @((Join-Path $here 'json.cpp'), (Join-Path $here 'util.cpp'),
                       '-lgdi32', '-luser32', '-lshell32', '-lws2_32', '-lwinhttp', '-ladvapi32')

        Write-Host "测试编译：$($suite.Name)"
        Remove-Item $outLog -Force -ErrorAction SilentlyContinue
        # 走 cmd /c 重定向：Start-Process -PassThru 在这台机器上 ExitCode 恒为空，
        # 而 PowerShell 直接把原生命令输出重定向到文件时又会踩命名管道限制。
        $quoted = $testArgs | ForEach-Object { if ($_ -match '\s') { '"{0}"' -f $_ } else { $_ } }
        $cmdLine = '"{0}" {1} > "{2}" 2>&1' -f $gpp, ($quoted -join ' '), $outLog
        cmd /c $cmdLine
        $testCode = $LASTEXITCODE
        if (Test-Path $outLog) { Get-Content -LiteralPath $outLog | ForEach-Object { Write-Host "  $_" } }
        if ($testCode -ne 0) { throw "$($suite.Name) 编译失败（exit $testCode）" }

        & $testExe
        if ($LASTEXITCODE -ne 0) { throw "$($suite.Name) 运行失败（exit $LASTEXITCODE）" }
    }
}

if ($Sign) {
    & (Join-Path $here 'sign.ps1') -ExePath $exe
}
