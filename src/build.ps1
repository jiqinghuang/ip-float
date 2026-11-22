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
    [string] $MingwBin = 'D:\Mingw64\bin',
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
if (-not $OutDir) {
    $OutDir = $root
} elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    # 相对路径按调用者当前目录解析，然后在 Push-Location src 之前固定成绝对路径
    $OutDir = Join-Path (Get-Location).Path $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

if (-not [System.IO.Path]::IsPathRooted($MingwBin)) {
    $MingwBin = Join-Path (Get-Location).Path $MingwBin
}
$MingwBin = [System.IO.Path]::GetFullPath($MingwBin)
if (-not (Test-Path -LiteralPath $MingwBin)) {
    throw "找不到 MinGW 目录：$MingwBin（用 -MingwBin 指定）"
}

$gpp     = Join-Path $MingwBin 'g++.exe'
$windres = Join-Path $MingwBin 'windres.exe'
foreach ($tool in @($gpp, $windres)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "缺少工具：$tool" }
}

$exe = Join-Path $OutDir 'ip-float.exe'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$sources = @(
    'main.cpp', 'app.cpp', 'config.cpp', 'http.cpp', 'ipinfo.cpp',
    'card.cpp', 'settings_dlg.cpp', 'tray.cpp', 'selftest.cpp', 'util.cpp'
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
        -lgdiplus -lwinhttp -lshell32 -luser32 -lgdi32 -lole32
    if ($LASTEXITCODE -ne 0) { throw "编译失败（exit $LASTEXITCODE）" }

    Unblock-File -LiteralPath $exe -ErrorAction SilentlyContinue
    Write-Host "已重新编译: $exe"
} finally {
    Remove-Item (Join-Path $here 'app.res.o') -Force -ErrorAction SilentlyContinue
    $env:PATH = $oldPath
    Pop-Location
}

if ($Test) {
    $testExe = Join-Path $OutDir 'test_ipinfo.exe'
    # 日志写到 TEMP，避免默认 OutDir=仓库根目录时污染 git status
    $outLog = Join-Path $env:TEMP 'ip-float-test-build.log'
    $testArgs = @('-std=gnu++17', '-O2', '-DUNICODE', '-D_UNICODE') + $warn + @(
        '-o', $testExe,
        (Join-Path $root 'tests\test_ipinfo.cpp'),
        (Join-Path $here 'ipinfo.cpp'),
        (Join-Path $here 'util.cpp'),
        '-lgdi32', '-luser32', '-lshell32')

    Write-Host "测试编译：$gpp"
    Remove-Item $outLog -Force -ErrorAction SilentlyContinue
    # 走 cmd /c 重定向：Start-Process -PassThru 在这台机器上 ExitCode 恒为空，
    # 而 PowerShell 直接把原生命令输出重定向到文件时又会踩命名管道限制。
    $quoted = $testArgs | ForEach-Object { if ($_ -match '\s') { '"{0}"' -f $_ } else { $_ } }
    $cmdLine = '"{0}" {1} > "{2}" 2>&1' -f $gpp, ($quoted -join ' '), $outLog
    cmd /c $cmdLine
    $testCode = $LASTEXITCODE
    if (Test-Path $outLog) { Get-Content -LiteralPath $outLog | ForEach-Object { Write-Host "  $_" } }
    if ($testCode -ne 0) { throw "测试编译失败（exit $testCode）" }

    & $testExe
    if ($LASTEXITCODE -ne 0) { throw "测试失败（exit $LASTEXITCODE）" }
}

if ($Sign) {
    & (Join-Path $here 'sign.ps1') -ExePath $exe
}
