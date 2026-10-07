# 重新编译 ip-float.exe（需要 D:\Mingw64\bin 下的 g++ 与 windres）
#
# 编译完会自动做两件收尾：
#   1. Unblock-File —— 清掉可能残留的「来自互联网」标记（MOTW / Zone.Identifier）
#   2. sign.ps1     —— 自签名。这是消除「打开文件 - 安全警告」弹窗的必要一环：
#                      附件管理器认本机信任的证书，签名后「发行商」不再显示为
#                      「未知发布者」。详见 README。
#   （SmartScreen「Windows 已保护你的电脑」是另一层，自签名挡不住，需要单独关。）
#
# 证书已在 CurrentUser\Root 受信任时，签名这一步不需要管理员权限。
#
# 用法：
#   pwsh -File src\build.ps1            # 编译 + 清标记 + 签名
#   pwsh -File src\build.ps1 -NoSign    # 只编译，不签名
[CmdletBinding()]
param([switch] $NoSign)

$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$root  = Split-Path -Parent $here
$mingw = 'D:\Mingw64\bin'
$exe   = Join-Path $root 'ip-float.exe'

Push-Location $here
try {
    & "$mingw\windres.exe" app.rc -o app.res.o
    & "$mingw\g++.exe" -std=gnu++17 -O2 -Wall -Wextra -mwindows -static -static-libgcc -static-libstdc++ `
        -o $exe ip-float.cpp app.res.o `
        -lgdiplus -lwinhttp -lshell32 -luser32 -lgdi32 -lole32
    if ($LASTEXITCODE -ne 0) { throw "编译失败（exit $LASTEXITCODE）" }

    # 有就清，没有也不报错
    Unblock-File -LiteralPath $exe -ErrorAction SilentlyContinue

    Write-Host "已重新编译: $exe"
} finally {
    Remove-Item (Join-Path $here 'app.res.o') -Force -ErrorAction SilentlyContinue
    Pop-Location
}

if (-not $NoSign) {
    try {
        & (Join-Path $here 'sign.ps1') -ExePath $exe
    } catch {
        Write-Warning "签名失败（exe 仍可运行，但双击可能弹「打开文件 - 安全警告」）：$($_.Exception.Message)"
        Write-Warning "如果提示「拒绝访问」，用管理员身份重跑一次本脚本即可（第一次建证书时需要）。"
    }
}
