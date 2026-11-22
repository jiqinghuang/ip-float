"""校验站点仓库里的项目页是否符合该站点的既有约定。

站点自己没有校验脚本（只有 html-and-css-tutorial 仓库有 check.py），所以这里按现有
13 个页面的实际写法逐项核对，避免引入肉眼看不出的低级错误。

用法：
    python .site-assets/validate.py                       # 默认校验 project-ip-float.html
    python .site-assets/validate.py projects.html         # 也可校验被改动过的其他页面
    python .site-assets/validate.py --site-root D:\\repo\\jiqinghuang.github.io

站点根目录默认取 ip-float 仓库旁边的 jiqinghuang.github.io，可用 --site-root
或 IPFLOAT_SITE_ROOT 覆盖（不再写死某个用户目录）。
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_SITE = os.path.abspath(os.path.join(ROOT, "..", "jiqinghuang.github.io"))
EXTERNAL = ("http://", "https://", "#", "mailto:")


def validate(name, site_root):
    page = os.path.join(site_root, name)
    with open(page, encoding="utf-8") as f:
        html = f.read()
    problems = []

    def check(cond, msg):
        if not cond:
            problems.append(msg)

    # 1. h1 唯一（站点每页正好一个）
    h1 = re.findall(r"<h1[ >]", html)
    check(len(h1) == 1, f"h1 数量应为 1，实际 {len(h1)}")

    # 2. data-lang 严格 cn,en 交替。
    #    用顺序不变量而不是正则跨标签配对：导航/页脚写的是
    #    <a data-lang="cn">首页</a>，闭合标签是 </a>，且 data-lang 后面还可能跟 style=。
    langs = re.findall(r'data-lang="(cn|en)"', html)
    expected = ["cn", "en"] * (len(langs) // 2)
    check(len(langs) % 2 == 0, f"data-lang 总数为奇数: {len(langs)}")
    check(langs == expected, "data-lang 顺序不是严格 cn,en 交替，首个偏差位置="
                             f"{next((i for i, (a, b) in enumerate(zip(langs, expected)) if a != b), len(langs))}")

    # 3. 页内锚点
    ids = set(re.findall(r'\sid="([^"]+)"', html))
    for anchor in re.findall(r'href="#([^"]+)"', html):
        check(anchor in ids, f"锚点 #{anchor} 没有对应的 id")

    # 4. 站内链接目标存在
    for href in re.findall(r'href="([^"]+)"', html):
        if href.startswith(EXTERNAL):
            continue
        target = os.path.join(site_root, href.split("#")[0].split("?")[0])
        check(os.path.isfile(target), f"站内链接目标不存在: {href}")

    # 5. 图片存在 + 有 alt（png/webp 成对）
    for src in re.findall(r'<img[^>]*\ssrc="([^"]+)"', html) + \
               re.findall(r'<source[^>]*\ssrcset="([^"]+)"', html):
        if not src.startswith("http"):
            check(os.path.isfile(os.path.join(site_root, src)), f"图片不存在: {src}")
    for tag in re.findall(r"<img\b[^>]*>", html):
        check('alt="' in tag, f"img 缺少 alt: {tag[:70]}")

    # 6. 站点既有结构
    for must in ("js/main.js", "css/style.css", "favicon.svg"):
        check(must in html, f"缺少站点既有结构: {must}")

    # 7. 不留占位内容
    for bad in ("TODO", "FIXME", "占位"):
        check(bad not in html, f"残留占位内容: {bad}")

    print(f"[{'OK' if not problems else 'FAIL'}] {name}  {len(html):,} chars  "
          f"h1={len(h1)}  data-lang pairs={len(langs)//2}")
    for p in problems:
        print("   -", p)
    return not problems


def main():
    ap = argparse.ArgumentParser(description="校验站点页面是否符合既有约定")
    ap.add_argument("pages", nargs="*", default=["project-ip-float.html"])
    ap.add_argument("--site-root",
                    default=os.environ.get("IPFLOAT_SITE_ROOT", DEFAULT_SITE),
                    help="站点仓库根目录（默认 ../jiqinghuang.github.io）")
    args = ap.parse_args()

    site_root = os.path.abspath(args.site_root)
    if not os.path.isdir(site_root):
        sys.exit(f"站点根目录不存在: {site_root}（用 --site-root 指定）")

    pages = args.pages or ["project-ip-float.html"]
    sys.exit(0 if all(validate(n, site_root) for n in pages) else 1)


if __name__ == "__main__":
    main()
