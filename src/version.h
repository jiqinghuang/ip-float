#pragma once

// Single source of truth for the version number —— **但有例外**。
// app.rc 里那份 VERSIONINFO 是硬编码的：MSYS2 的 windres 预处理 #include 会失败，
// 所以升版本时**必须同时改 app.rc** 的 FILEVERSION / FileVersion / ProductVersion，
// 以及 app.manifest 的 assemblyIdentity。build.ps1 会在编译前检查三处是否一致。
// （User-Agent 用的是 IPFLOAT_VERSION_W，跟着这里走。）
#define IPFLOAT_VERSION_A "3.4.0"
#define IPFLOAT_VERSION_W L"3.4.0"

#define IPFLOAT_VERSION_MAJOR 3
#define IPFLOAT_VERSION_MINOR 4
#define IPFLOAT_VERSION_PATCH 0
#define IPFLOAT_VERSION_BUILD 0
