#!/usr/bin/env python3
"""生成播客机中文字库子集(main/podcast_font_16.c, main/podcast_font_20.c)。

字形来源:Noto Sans SC(Variable, OFL 授权),与 LEO RADIO 同款思路:
lv_font_conv 按实际用字生成子集,而不是把整个 CJK 字库塞进 8MB Flash。

用字表 = UI 固定文案(从源码提取,保证全覆盖)
       + jieba 词频表前 N 个高频汉字(覆盖网络下发的动态节目标题)。
动态标题中的生僻字会显示缺字占位框(配置里保留 PLACEHOLDER,问题可见)。

用法:
    python3 tools/gen_podcast_font.py --font /tmp/NotoSansSC-VF.ttf
    # 输出 main/podcast_font_16.c 与 main/podcast_font_20.c
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_SOURCES = [
    "main/podcast_ui.c",
    "main/podcast.h",
    "main/main.c",
]
TOP_N_CHARS = 2500  # 高频汉字数;16px 约 260KB,20px 约 420KB,合计 <1MB

CJK_RE = re.compile(r"[\u4e00-\u9fff]")


def ui_chars():
    """从 UI 源码提取全部 CJK 字符(含中文标点)。"""
    chars = set()
    for rel in UI_SOURCES:
        with open(os.path.join(REPO, rel), encoding="utf-8") as f:
            text = f.read()
        chars.update(CJK_RE.findall(text))
        # 中文标点与常用符号(源码里以 UTF-8 直接写出)
        for ch in text:
            if "\u3000" <= ch <= "\u303f" or "\uff00" <= ch <= "\uffef":
                chars.add(ch)
    # 界面里用的图形符号,确保有字形(回退链兜底,此处也收录)
    chars.update("▶⏸…")
    return chars


def freq_chars(top_n):
    """用 jieba 词频表按词频展开取前 top_n 个汉字。"""
    import jieba

    dict_path = os.path.join(os.path.dirname(jieba.__file__), "dict.txt")
    words = []
    with open(dict_path, encoding="utf-8") as f:
        for line in f:
            parts = line.strip().split(" ")
            if len(parts) >= 2 and parts[1].isdigit():
                words.append((parts[0], int(parts[1])))
    words.sort(key=lambda w: -w[1])

    seen = set()
    for word, _ in words:
        for ch in word:
            if CJK_RE.match(ch) and ch not in seen:
                seen.add(ch)
                if len(seen) >= top_n:
                    return seen
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, help="NotoSansSC-VF.ttf 路径")
    ap.add_argument("--top-n", type=int, default=TOP_N_CHARS)
    ap.add_argument("--compress", action="store_true",
                    help="启用 LVGL 字体压缩(需 sdkconfig 开 CONFIG_LV_USE_FONT_COMPRESSED)")
    args = ap.parse_args()

    must = ui_chars()
    out_16 = os.path.join(REPO, "main", "podcast_font_16.c")
    out_20 = os.path.join(REPO, "main", "podcast_font_20.c")
    print(f"UI 固定用字: {len(must)} 个")

    # 16px 只画固定 UI 文案(日期/时间/状态/提示,动态部分只有 ASCII),
    # 用字量小;20px 画动态节目标题,需要高频字覆盖。
    sizes = ((16, out_16, "podcast_font_16", 0),
             (20, out_20, "podcast_font_20", args.top_n))
    # 覆盖率自检:确认 UI 固定用字确实都在源字体里(转换器也会报,双保险)
    for size, out, name, top_n in sizes:
        freq = freq_chars(top_n) if top_n > 0 else set()
        all_chars = must | freq
        symbols = "".join(sorted(all_chars))
        print(f"[size={size}] 用字 {len(all_chars)} 个")
        cmd = [
            "lv_font_conv",
            "--font", args.font,
            "--symbols", symbols,
            "--range", "0x20-0x7E",          # ASCII 可打印区
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "--lv-include", "lvgl.h",
            "--lv-font-name", name,
            "--output", out,
        ]
        if not args.compress:
            cmd.append("--no-compress")      # 先不压缩,避解码器/配置不匹配
        print("运行:", " ".join(cmd[:6]), "...")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[-2000:], file=sys.stderr)
            sys.exit(f"lv_font_conv 失败(size={size})")
        # 转换器会把缺字形打到 stderr,截一段出来人工看
        missing = [l for l in r.stderr.splitlines() if "missing" in l.lower()]
        if missing:
            print(f"[size={size}] 转换器报告缺字形 {len(missing)} 条,前 5 条:")
            for l in missing[:5]:
                print("   ", l[:120])
        print(f"生成 {out} ({os.path.getsize(out)//1024} KB)")


if __name__ == "__main__":
    main()
