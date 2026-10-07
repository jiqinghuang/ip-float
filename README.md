# ip-float — 出口 IP 桌面悬浮窗

无边框置顶小卡片，两行显示：

```
203.0.113.42 · JP Tokyo Chiyoda
AS20473 Vultr · 2分钟前
```

|路径|说明|
|-|-|
|**`ip-float.exe`**|★ 成品。单文件、双击即开、零运行时依赖（≈710 KB）。**不进 git**，分发走 [Releases](https://github.com/jiqinghuang/ip-float/releases/latest)|
|`src/`|C++ 源码 + 图标 + `build.ps1` 一键重编|
|`preview-exe.png`|实拍|

## 用法

直接双击 `ip-float.exe`。

* 纯 Win32 + GDI+，MinGW 静态编译，**不依赖 Python / .NET / Node / VCRedist**
* 只调 Windows 自带库：`gdiplus`、`winhttp`、`user32`、`gdi32`、`shell32`、`kernel32`
* 无控制台窗口、不占任务栏（只在托盘）、DPI 100%\~200% 自适应（**包括拖到另一块不同缩放的屏幕**）
* 半透明圆角卡片，拖到哪记到哪
* **单实例**：已经有一个在跑时，再双击只会把已有卡片闪一下，不会开出第二张
* 点卡片**不抢焦点**（`WS\\\_EX\\\_NOACTIVATE`），你正在打的字不会被打断

**右键菜单**：状态行（含数据源与更新时间） · 立即刷新 · 刷新间隔（1/3/5/10/30/60 分钟） · 设置… · 复制 IP · 总在最前 · 开机自启 · 退出
**托盘图标**：双击显示/隐藏，右键菜单同上
**设置窗口**：刷新间隔（1–1440 分钟）、不透明度（0–100）、总在最前、开机自启（写 `HKCU\\\\...\\\\Run`）

> 不透明度拉到 \\\*\\\*0\\\*\\\* 卡片就完全透明了（还占着位置、点不到），这时从\\\*\\\*托盘图标\\\*\\\*右键 → 设置… 改回来。

配置位置：`%APPDATA%\\\\ip-float\\\\settings.ini`；如果那目录不可写，自动退到 exe 同目录的 `ip-float.ini`
（若 APPDATA 里已有配置会先搬过去，不会默默丢设置；右键菜单最底下会显示当前实际生效的路径）。
删掉配置就恢复默认（右上角、5 分钟、94% 不透明）。

> **受限环境会静默切到便携模式。** 在沙箱、低权限账户或被杀软拦截的 shell 里运行时，
> `%APPDATA%\ip-float` 可能写不进去，程序会退到 exe 同目录的 `ip-float.ini` ——
> 界面上没有任何提示，只有右键菜单最底下那行路径能看出来。
> 最快的判据：**exe 旁边一旦出现 `ip-float.ini`，就等于那一次启动没能写进 APPDATA**
> （注意这和账户权限无关 —— 受限的是那一次启动所用的令牌，正常双击通常写 APPDATA 没问题）。
> 要留心的是：退化时**只有当 exe 目录还没有 ini，才会把 APPDATA 的配置搬过去**；
> 如果那里已经躺着一份旧的，程序会直接沿用旧值（间隔、不透明度、坐标都可能是很久以前的），
> 而 APPDATA 里那份真实配置被完全忽略。便携模式下改的设置也**不会**回流到 APPDATA，
> 下次正常双击仍然读 APPDATA。
> 它已在 `.gitignore` 里，反复生成也不会污染仓库；但别让这份副本长期停在旧值上 ——
> 删掉它，下一次退化启动会重新从 APPDATA 迁移一份过来。

命令行（平时用不到）：

```powershell
ip-float.exe --interval 10                       # 临时改间隔
ip-float.exe --source https://你的镜像/ip.json    # 换数据源（企业内网/自建）
ip-float.exe --demo --shot out.png               # 假数据渲染一张卡片自检图
ip-float.exe --demo --shot-dlg dlg.png           # 给设置窗口拍一张自检图
```

两个 `--shot` 都不联网、且**不受单实例限制**，改完 UI 可以随时验证渲染。
截图会在同名位置写一个 `.log`（因为是 `-mwindows` 构建，没有控制台可输出）。

## 数据源

`https://ipinfo.io/json`，失败自动换 `https://ipapi.co/json/`；都不需要 key，
超时是 DNS / 连接 / 发送 / 接收各 8 秒。
失败时保留上次结果、状态点变橙，并在卡片上写明原因：

* WinHTTP 错误码翻成了中文，例如 `12185：证书用途不匹配（常见于代理/中间人拦截）`、`12029：无法连接（端口不通、被墙，或该走代理）`
* HTTP 状态码也会给建议，例如 `429：请求太频繁，被数据源限流（调大间隔，或用 --source 换源）`、`403：被拒绝（数据源要 token，或你的出口 IP 被限流封禁）`

## 重新编译

源码在 `src/`，需要 `D:\\\\Mingw64\\\\bin` 下的 `g++` 与 `windres`：

```powershell
pwsh -File src\\\\build.ps1
```

编译产物会覆盖根目录的 `ip-float.exe`。构建开了 `-Wall -Wextra`，**应当零警告**；
真出警告了先修，别习惯性忽略。

## 「打开文件 - 安全警告」/「Windows 已保护你的电脑」弹窗

双击 exe 要先点两次才能运行 —— 这是本机踩过的坑。**有两层互相独立的拦截，
必须分别处理**：只解决一层，另一层会立刻顶上来，表现为「弹窗换了个样子」。

### 第一层：SmartScreen「Windows 已保护你的电脑」

> Microsoft Defender SmartScreen 阻止了无法识别的应用启动。…发行者：发布者未知

按**文件哈希**做云端信誉判定。本机刚编译出来的 exe 微软服务器从没见过，
所以每次重编（哈希变了）都会拦。这一层**自签名挡不住**。

关掉它 ——「检查应用和文件」（需要管理员）：

```
[HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer]
"SmartScreenEnabled"="Off"
```

界面路径：Windows 安全中心 → 应用和浏览器控制 → 基于信誉的保护 →
关掉「检查应用和文件」。

* 想改回来：把值改成 `"Warn"`（系统默认），或直接删掉这个值。
* **别改错键**：`HKLM\SOFTWARE\Policies\Microsoft\Windows Defender\SmartScreen\EnableSmartScreen`
  是**另一套**东西（Defender / Edge 的 SmartScreen 策略），对这个弹窗没有作用。

### 第二层：附件管理器「打开文件 - 安全警告」

> 无法验证发布者。你确定要运行此软件吗? / 你想运行此文件吗?
> 发行商：未知发布者 … 此文件没有包含有效的数字签名以验证其发布者。

这是**附件管理器（Attachment Manager）**，跟 SmartScreen 是两套独立机制。
下面三步**缺一不可**：

**① 给 exe 签名**（`sign.ps1` 负责）

附件管理器**认本机信任的证书**。自签名证书放进 `CurrentUser\Root` 之后，
「发行商」就从「未知发布者」变成 `CN=ip-float self-signed`，红色警告随之消失。

> 注意这和 SmartScreen 恰好相反：**自签名对附件管理器有效，对 SmartScreen 无效。**

**② 把 `.exe` 加进低风险包含列表**

```
[HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Policies\Associations]
"LowRiskFileTypes"=".exe"
```

文档说明：这样 Windows 会忽略这些扩展名文件上的 ADS 标记 ——
"run them without warning ... regardless of whether the NTFS Zone.Identifier stream exists"。
（界面等价物：gpedit → 用户配置 → 管理模板 → Windows 组件 → 附件管理器 →
「低风险文件类型的包含列表」→ 填 `.exe`。）

**③ 重启资源管理器（这一步最关键，也最容易漏）**

`LowRiskFileTypes` 是**策略值，Explorer 只在启动时读一次**。
本机就是卡在这里：值写进去了，但 Explorer 从早上开机后一直没重启，
于是看起来"完全没效果"。

任务管理器（`Ctrl+Shift+Esc`）→ 找到「Windows 资源管理器」→ 右键 →「重新启动」。
之后再双击就正常了。

> 不要用「注销 / 重启电脑」以外的暴力手段去杀 Explorer —— 由受限进程拉起的新
> Explorer 可能继承限制，把桌面搞成打不开文件的状态。用任务管理器那个按钮最稳。

### 别再试了

| 方法 | 为什么没用 |
|---|---|
| 只清 MOTW（`Unblock-File`） | 这个 exe 本来就没有 `Zone.Identifier`；此弹窗不是 MOTW 触发的 |
| Defender 排除项（文件夹 / exe） | 跟 SmartScreen、附件管理器都是彼此独立的机制 |
| 改 `...\Defender\SmartScreen\EnableSmartScreen` | 改错键了，对这个弹窗无效 |
| 改完注册表不重启 Explorer | 策略值不会被读到，看起来像"无效" |
| 反复重编 exe | 每重编一次哈希就变一次，SmartScreen 那层的信誉归零 |

### 重编之后

`build.ps1` 默认在编译完成后调用 `sign.ps1` 自动重新签名（`-NoSign` 可关掉）。
证书已在 `CurrentUser\Root` 受信任的情况下，这一步**不需要管理员权限**。

## 网站素材

`.site-assets/` 是给个人网站 `project-ip-float.html` 生成截图素材的工具，跟 exe 本身无关：

```powershell
python .site-assets\generate.py            # 抓屏 + 合成 → .site-assets\out\
python .site-assets\validate.py            # 校验站点页面是否符合该站既有约定
```

`generate.py` 用 exe 自带的自检模式（`--demo --shot` / `--shot-dlg`）抓屏，再把桌面像素
清干净、扣出真正的圆角透明区域；产物拷到站点仓库的 `assets/ip-float/`。
两个脚本的文档字符串里写了为什么必须"先把不透明度设成 100"等三个坑。

## 已知限制

* 走 Windows **schannel**（WinHTTP）。如果网络里有中间人/企业代理导致证书校验失败，
会看到 `12175/12182/12185` 之类的错误码 —— 这类环境需要换用 OpenSSL 栈的客户端。
* 每 N 分钟会向 ipinfo.io / ipapi.co 发一次请求，等于把出口 IP 报给第三方；介意就调大间隔或换自建源。
* exe 是**自签名**的：证书放在本机 `CurrentUser\Root`，**只对这个账户有效**。
  换机器 / 换用户后弹窗会回来 —— 用 `pwsh -File src\sign.ps1 -ExportCer` 导出 `.cer`，
  在那台机器上双击安装到「当前用户 → 受信任的根证书颁发机构」即可。
* 双击弹窗的两层拦截与处理办法见上文专节；`build.ps1` 重编后会自动重新签名。
* 仓库已接上 git（`origin` = `https://github.com/jiqinghuang/ip-float.git`）；改源码时顺手多提交几次，
  别攒成一个大 commit —— 出问题才有得回退。
* **`ip-float.exe` 不进仓库**：分发走 [GitHub Releases](https://github.com/jiqinghuang/ip-float/releases/latest)，
  下载链接固定为 `/releases/latest`，不再随文件名或路径变化。重编一次哈希就变一次，把 700 KB 的二进制
  反复写进 git 历史没有意义。`build.ps1` 照旧把产物生成在仓库根目录，只是它已被 `.gitignore` 忽略。

