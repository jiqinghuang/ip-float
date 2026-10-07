<#
    sign.ps1 — 给 ip-float.exe 打本地代码签名

    为什么需要：未签名的 exe 会触发弹窗 ——
      · 附件管理器「打开文件 - 安全警告」（"此文件没有包含有效的数字签名"）— 签名能挡住
      · SmartScreen「Windows 已保护你的电脑」— 按云端信誉判定，自签名挡不住

    做三件事：
      1. 找 / 造一张 CN=ip-float self-signed 的代码签名证书（存 CurrentUser\My）
      2. 把它的公钥放进 CurrentUser\Root，让本机信任它
      3. 签名 exe，并顺手清掉「从互联网下载」标记

    注意：写证书库（%APPDATA%\Microsoft\SystemCertificates 与 Crypto）在受限/沙箱
    环境里可能被拒；被拒时用管理员身份跑本脚本。

    用法：
      pwsh -File src\sign.ps1                    # 签根目录的 ip-float.exe
      pwsh -File src\sign.ps1 -ExportCer         # 顺便导出一份 ip-float-signing.cer
#>
[CmdletBinding()]
param(
    [string] $ExePath,
    [switch] $ExportCer
)

$ErrorActionPreference = 'Stop'

if (-not $ExePath) {
    $ExePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'ip-float.exe'
}
if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "找不到要签名的文件：$ExePath"
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path

$OF = [System.Security.Cryptography.X509Certificates.OpenFlags]
$Subject = 'CN=ip-float self-signed'

# ⚠ PowerShell 变量名不区分大小写：曾经这里有个 $KU 存 X509KeyUsageFlags 类型，
#   而下面又用 $ku 存 KeyUsage 扩展数组 —— 两者是同一个变量，$ku 赋值后
#   $KU::DigitalSignature 取到 $null，128 -band $null = 0，校验永远失败。
#   所以静态成员一律写完整类型字面量，别用短变量转存类型。
$KeyUsageOid = '2.5.29.15'
$EkuOid      = '2.5.29.37'
$CodeSignOid = '1.3.6.1.5.5.7.3.3'

# Set-AuthenticodeSignature 要求证书同时满足：
#   · EKU 含 1.3.6.1.5.5.7.3.3 (Code Signing)
#   · KeyUsage 含 DigitalSignature
# 返回 '' 表示可用，否则返回不可用的原因（便于定位）
function Test-UsableForCodeSigning($cert) {
    if (-not $cert) { return '证书为空' }
    if (-not $cert.HasPrivateKey) { return '没有私钥' }

    $ekus = @($cert.Extensions | Where-Object { $_.Oid.Value -eq $EkuOid })
    if ($ekus.Count -eq 0) { return '缺少 EKU 扩展' }
    if (@($ekus[0].EnhancedKeyUsages | Where-Object { $_.Value -eq $CodeSignOid }).Count -eq 0) {
        return 'EKU 不含 Code Signing'
    }

    $kus = @($cert.Extensions | Where-Object { $_.Oid.Value -eq $KeyUsageOid })
    if ($kus.Count -eq 0) { return '缺少 KeyUsage 扩展' }
    $digitalSignature = [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature
    if (($kus[0].KeyUsages -band $digitalSignature) -eq 0) {
        return "KeyUsage 不含 DigitalSignature（实际 $($kus[0].KeyUsages)）"
    }
    return ''
}

function Get-SigningCert {
    Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $Subject } |
        Where-Object { (Test-UsableForCodeSigning $_) -eq '' } |
        Select-Object -First 1
}

function Remove-OldCerts {
    # 清掉同名的旧证书（尤其是缺 KeyUsage 的坏证书），免得被反复复用
    foreach ($storeName in 'My', 'Root') {
        $s = [System.Security.Cryptography.X509Certificates.X509Store]::new($storeName, 'CurrentUser')
        $s.Open($OF::ReadWrite)
        foreach ($old in @($s.Certificates | Where-Object { $_.Subject -eq $Subject })) {
            $s.Remove($old)
        }
        $s.Close()
    }
}

function New-SigningCert {
    # 用 PKI 模块的 New-SelfSignedCertificate，而不是手搓 CertificateRequest：
    # CodeSigningCert 模板会自动写对 EKU=Code Signing 和 KeyUsage=DigitalSignature，
    # 也避免了「PFX 往返后私钥/扩展丢失」那类坑。证书直接落进 CurrentUser\My。
    New-SelfSignedCertificate -Type CodeSigningCert `
        -Subject $Subject `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(20) `
        -ErrorAction Stop
}

# ── 1. 证书 ────────────────────────────────────────────────────────────────
$cert = Get-SigningCert
if (-not $cert) {
    Remove-OldCerts
    $cert = New-SigningCert
    $why = Test-UsableForCodeSigning $cert
    if ($why) { throw "新建的证书不可用于代码签名：$why" }
    Write-Host "已创建代码签名证书：$($cert.Thumbprint)" -ForegroundColor DarkGray
} else {
    Write-Host "复用已有代码签名证书：$($cert.Thumbprint)" -ForegroundColor DarkGray
}

# ── 2. 本机信任（CurrentUser\Root）─────────────────────────────────────────
# 先用只读方式判断是否已信任，只有确实没信任才去写证书库。
# 写证书库在受限/沙箱环境里会被拒；而每次重编都要重新签名 ——
# 已经信任过的情况下不该再要求提权。
function Test-TrustedLocally($cert) {
    $s = [System.Security.Cryptography.X509Certificates.X509Store]::new('Root', 'CurrentUser')
    $s.Open($OF::ReadOnly)
    $found = [bool]($s.Certificates | Where-Object Thumbprint -eq $cert.Thumbprint)
    $s.Close()
    return $found
}

if (Test-TrustedLocally $cert) {
    Write-Host "已在本机受信任：CurrentUser\Root" -ForegroundColor DarkGray
} else {
    $rootStore = [System.Security.Cryptography.X509Certificates.X509Store]::new('Root', 'CurrentUser')
    $rootStore.Open($OF::ReadWrite)
    $rootStore.Add([System.Security.Cryptography.X509Certificates.X509Certificate2]::new($cert.RawData))
    $rootStore.Close()
    Write-Host "已加入本机信任：CurrentUser\Root" -ForegroundColor DarkGray
}

if ($ExportCer) {
    $cerPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'ip-float-signing.cer'
    [System.IO.File]::WriteAllBytes($cerPath, $cert.RawData)
    Write-Host "已导出证书：$cerPath" -ForegroundColor DarkGray
    Write-Host "  换机器时双击它 →「安装证书」→「当前用户」即可。" -ForegroundColor DarkGray
}

# ── 3. 清 MOTW + 签名 ──────────────────────────────────────────────────────
# Zone.Identifier（“从互联网下载”）单独也会触发弹窗，先清掉
Unblock-File -LiteralPath $ExePath -ErrorAction SilentlyContinue

$sig = Set-AuthenticodeSignature -FilePath $ExePath -Certificate $cert -HashAlgorithm SHA256
if ($sig.Status -ne 'Valid') {
    throw "签名未生效：$($sig.Status) — $($sig.StatusMessage)"
}

$ver = Get-AuthenticodeSignature -FilePath $ExePath
Write-Host "已签名：$ExePath" -ForegroundColor Green
Write-Host "  签名者：$($ver.SignerCertificate.Subject)" -ForegroundColor DarkGray
Write-Host "  状态　：$($ver.Status)" -ForegroundColor DarkGray
Write-Host "  指纹　：$($ver.SignerCertificate.Thumbprint)" -ForegroundColor DarkGray
