"""重新生成 project-ip-float.html 用的截图素材。

网页上的两张图来自 exe 自带的自检模式，不联网、可重复：
    ip-float.exe --demo --shot       -> 卡片本体
    ip-float.exe --demo --shot-dlg   -> 设置窗口

两个坑（都踩过，别再踩一遍）：
1. 必须把不透明度设成 100 再抓屏。--shot 走的是 GetDC(NULL) + BitBlt 抓屏幕区域，
   卡片本身是分层窗口；半透明时桌面壁纸会整个透进截图（会看到别的窗口的标题栏按钮）。
2. SetWindowRgn 把卡片圆角切成硬边区域，圆角外面是桌面像素；而 GetWindowRect 又包含
   Win11 的不可见缩放边框。所以卡片要用"与区域几何一致的硬边圆角遮罩"，设置窗口要用
   "按内容推导的逐行边界"——两者都不能靠猜半径，试过 12/13 都对不齐 DWM 的抗锯齿圆弧。

用法（在 ip-float 仓库根目录）：
    python .site-assets/generate.py
然后把 out/ 里的 4 个文件拷到站点仓库的 assets/ip-float/。
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                 # ip-float 仓库根目录
BUILD, OUT = os.path.join(HERE, "build"), os.path.join(HERE, "out")

CARD_BG = (30, 31, 34)      # kBg(255, 30, 31, 34)
RADIUS = 20                 # CreateRoundRectRgn 椭圆直径 Scale(20)=40 → 半径 20
EDGE_BAND = 22              # 只在这么宽的外圈里做桌面像素兜底清理
LIGHT = 100                 # 卡片外圈没有亮内容：边框 66、底色 32，桌面 ~210


def is_light(px):
    return (px[0] + px[1] + px[2]) / 3 > LIGHT


def capture():
    """用自检模式抓两张原图到 build/。"""
    import os as _os
    _os.makedirs(BUILD, exist_ok=True)
    shutil.copy2(os.path.join(ROOT, "ip-float.exe"), os.path.join(BUILD, "ip-float.exe"))
    # 不写 x/y → 自动放到屏幕右上角；opacity=100 → 抓屏不会穿透桌面
    with open(os.path.join(BUILD, "ip-float.ini"), "w", encoding="ascii") as f:
        f.write("[ip-float]\nintervalMinutes=5\nopacityPercent=100\ntopMost=1\nautoStart=0\n")

    for args, name in ((["--demo", "--shot", "card.png"], "card.png"),
                       (["--demo", "--shot-dlg", "settings.png"], "settings.png")):
        subprocess.run([os.path.join(BUILD, "ip-float.exe")] + args,
                       cwd=BUILD, check=True)
        log = os.path.join(BUILD, name + ".log")
        msg = open(log, "rb").read().decode("utf-16-le", "ignore") if os.path.exists(log) else "(no log)"
        print(f"  {name:14} <- {msg.strip()}")
        if "FAILED" in msg:
            sys.exit(f"自检抓屏失败: {name}")


def process():
    from PIL import Image, ImageDraw
    os.makedirs(OUT, exist_ok=True)

    # ── 卡片：铺底色 + 与区域几何一致的硬边圆角遮罩 ──────────────────
    card = Image.open(os.path.join(BUILD, "card.png")).convert("RGBA")
    w, h = card.size
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], radius=RADIUS, fill=255)

    mp, cp = mask.load(), card.load()
    cleaned = 0
    for y in range(h):
        for x in range(w):
            if not (x < EDGE_BAND or x >= w - EDGE_BAND or y < EDGE_BAND or y >= h - EDGE_BAND):
                continue
            if mp[x, y] and is_light(cp[x, y]):   # 外圈亮像素 = 桌面残留
                mp[x, y] = 0
                cleaned += 1
    card.putalpha(mask)
    card = Image.alpha_composite(Image.new("RGBA", (w, h), CARD_BG + (255,)), card)
    card.putalpha(mask)                            # 合成会带回 alpha，重新扣一遍
    card.save(os.path.join(OUT, "card.png"))
    card.save(os.path.join(OUT, "card.webp"), quality=92, method=6)
    print(f"card.png     {w}x{h}   兜底清掉桌面像素 {cleaned}")

    # ── 设置窗口：先裁掉不可见边框，再按内容推导逐行边界 ─────────────
    st = Image.open(os.path.join(BUILD, "settings.png")).convert("RGB")
    sw, sh = st.size
    sp = st.load()
    col_dark = lambda x: sum(1 for y in range(sh) if not is_light(sp[x, y])) / sh
    row_dark = lambda y: sum(1 for x in range(sw) if not is_light(sp[x, y])) / sw
    left = next(x for x in range(sw) if col_dark(x) > 0.9)
    right = next(x for x in range(sw - 1, -1, -1) if col_dark(x) > 0.9)
    top = next(y for y in range(sh) if row_dark(y) > 0.9)
    bottom = next(y for y in range(sh - 1, -1, -1) if row_dark(y) > 0.9)
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
    print("1) 用自检模式抓屏（opacity=100，不联网）")
    capture()
    print("2) 处理成网页素材")
    process()
    print(f"\n完成。把 {OUT} 里的 4 个文件拷到站点仓库的 assets/ip-float/ 即可。")
