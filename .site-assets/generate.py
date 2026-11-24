"""重新生成 project-ip-float.html 用的截图素材。

网页上的三张图来自 exe 自带的自检模式，不联网、可重复：
    ip-float.exe --render card.png --render-scale 2   -> 卡片本体（离屏渲染）
    ip-float.exe --demo --shot-dlg settings.png        -> 设置窗口（屏幕抓图）

三个坑（都踩过，别再踩一遍）：
1. 卡片不要再用屏幕抓图。--shot 走 GetDC(NULL)+BitBlt，卡片是分层窗口；半透明时
   桌面壁纸会整个透进截图。现在 --render 直接把卡片画进 32bpp 位图，输出自带透明
   圆角和抗锯齿，跟桌面/壁纸/不透明度/DPI 全部解耦，sidecar .json 里还有几何和配色。
   设置窗口仍是屏幕抓图（要保留 Win11 的圆角和标题栏），所以下面按内容裁边时
   仍然假设桌面是浅色的；深色壁纸请先换一张浅色壁纸再跑。
2. 不要硬编码卡片半径。--render 按 --render-scale 算几何，sidecar 里的
   cornerRadius/width/height 是唯一真相；设置窗口的裁边靠像素推导，不猜半径。
3. 设置窗口抓图前要把配置隔离。generate.py 传 --ini build/ip-float.ini，
   exe 不会读你的 %APPDATA% 配置，也不会把自检结果写回真实配置。

用法（在 ip-float 仓库根目录）：
    python .site-assets/generate.py
然后把 out/ 里的文件拷到站点仓库的 assets/ip-float/（card / diag / settings 各 png+webp）。
依赖 Pillow：pip install pillow
"""
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                 # ip-float 仓库根目录
BUILD, OUT = os.path.join(HERE, "build"), os.path.join(HERE, "out")

RENDER_SCALE = 2      # 2x 素材；卡片 340x80 -> 680x160
LIGHT = 100           # 设置窗口裁边：只对屏幕抓图有效，假设桌面是浅色


def is_light(px):
    return (px[0] + px[1] + px[2]) / 3 > LIGHT


def exe_path():
    # 默认用仓库根目录的成品；开发时可以 IPFLOAT_EXE=dist\ip-float.exe
    env = os.environ.get("IPFLOAT_EXE")
    if env:
        return env
    root_exe = os.path.join(ROOT, "ip-float.exe")
    if os.path.exists(root_exe):
        return root_exe
    return os.path.join(ROOT, "dist", "ip-float.exe")


def read_log(image_path):
    log = image_path + ".log"
    if not os.path.exists(log):
        return "(no log)"
    return open(log, "rb").read().decode("utf-16-le", "ignore").strip()


def run(args, image_name, cwd):
    subprocess.run([os.path.join(cwd, "ip-float.exe")] + args, cwd=cwd, check=True)
    image = os.path.join(cwd, image_name)
    msg = read_log(image)
    print(f"  {image_name:14} <- {msg}")
    if "FAILED" in msg or not msg:
        sys.exit(f"自检失败: {image}: {msg}")
    return image


def capture():
    """用自检模式抓两张原图到 build/。"""
    os.makedirs(BUILD, exist_ok=True)
    shutil.copy2(exe_path(), os.path.join(BUILD, "ip-float.exe"))

    # --ini 强制隔离配置：不读也不写用户的 %APPDATA%
    ini = os.path.join(BUILD, "ip-float.ini")
    with open(ini, "w", encoding="ascii") as f:
        f.write("[ip-float]\nintervalMinutes=5\nopacityPercent=100\n"
                "topMost=1\nautoStart=0\n")

    card = run(["--render", "card.png", "--render-scale", str(RENDER_SCALE),
                "--ini", ini], "card.png", BUILD)

    # 核对 sidecar：几何/缩放必须和这次渲染一致
    with open(card + ".json", encoding="utf-8") as f:
        meta = json.load(f)
    print(f"  card sidecar   {meta['width']}x{meta['height']} "
          f"scale={meta['scale']:g} radius={meta['cornerRadius']}")

    # 诊断面板同样走离屏渲染：演示数据、不联网、不暴露真实 IP
    run(["--render-dlg", "diag.png", "--render-scale", str(RENDER_SCALE)], "diag.png", BUILD)

    run(["--demo", "--shot-dlg", "settings.png", "--ini", ini], "settings.png", BUILD)


def process():
    from PIL import Image

    os.makedirs(OUT, exist_ok=True)

    # ── 卡片：--render 已经带透明圆角，直接转 webp ─────────────────────
    card = Image.open(os.path.join(BUILD, "card.png")).convert("RGBA")
    if card.getpixel((0, 0))[3] != 0:
        sys.exit("card.png 左上角不是透明的：--render 没有走离屏路径？")
    card.save(os.path.join(OUT, "card.png"))
    card.save(os.path.join(OUT, "card.webp"), quality=92, method=6)
    print(f"card.png     {card.width}x{card.height}")

    # ── 诊断面板：离屏渲染，不透明，直接转 webp ───────────────────────
    dlg = Image.open(os.path.join(BUILD, "diag.png")).convert("RGB")
    dlg.save(os.path.join(OUT, "diag.png"))
    dlg.save(os.path.join(OUT, "diag.webp"), quality=92, method=6)
    print(f"diag.png     {dlg.width}x{dlg.height}")

    # ── 设置窗口：先裁掉不可见边框，再按内容推导逐行边界 ─────────────
    st = Image.open(os.path.join(BUILD, "settings.png")).convert("RGB")
    sw, sh = st.size
    sp = st.load()
    col_dark = lambda x: sum(1 for y in range(sh) if not is_light(sp[x, y])) / sh
    row_dark = lambda y: sum(1 for x in range(sw) if not is_light(sp[x, y])) / sw

    def first(pred, start, stop, step):
        rng = range(start, stop, step)
        for i in rng:
            if pred(i):
                return i
        sys.exit("设置窗口裁边失败：桌面太暗或截图全亮，换浅色壁纸重试")

    left = first(lambda x: col_dark(x) > 0.9, 0, sw, 1)
    right = first(lambda x: col_dark(x) > 0.9, sw - 1, -1, -1)
    top = first(lambda y: row_dark(y) > 0.9, 0, sh, 1)
    bottom = first(lambda y: row_dark(y) > 0.9, sh - 1, -1, -1)
    st = st.crop((left, top, right + 1, bottom + 1)).convert("RGBA")

    # Win11 窗口自带圆角，圆弧外是桌面；逐行取最外侧暗像素，圆角几何完全由图像决定
    stp = st.load()
    row_l, row_r = [], []
    for y in range(st.height):
        dark = [x for x in range(st.width) if not is_light(stp[x, y])]
        row_l.append(min(dark) if dark else 0)
        row_r.append(max(dark) if dark else st.width - 1)

    smask = Image.new("L", st.size, 0)
    smp = smask.load()
    for y in range(st.height):
        for x in range(row_l[y], row_r[y] + 1):
            smp[x, y] = 255
    st.putalpha(smask)
    st.save(os.path.join(OUT, "settings.png"))
    st.save(os.path.join(OUT, "settings.webp"), quality=92, method=6)
    print(f"settings.png {st.width}x{st.height}  内容边界已自动裁切")

    for f in ("card.png", "card.webp", "settings.png", "settings.webp"):
        print(f"  {f:14} {os.path.getsize(os.path.join(OUT, f)):>8,} bytes")


if __name__ == "__main__":
    print("1) 用自检模式抓屏（--render 不联网，--shot-dlg 用演示数据）")
    capture()
    print("2) 处理成网页素材")
    process()
    print(f"\n完成。把 {OUT} 里的 4 个文件拷到站点仓库的 assets/ip-float/ 即可。")
