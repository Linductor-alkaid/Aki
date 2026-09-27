#!/usr/bin/env python3
"""生成 Aki 应用图标资产（assets/icons/）。

来源：pinned third_party/heyaki docs/icon/heyaki-transparent.png（MIT，见
assets/icons/README.md 溯源）。产物：

- aki-icon.png  : 256x256 RGBA 窗口图标（dslAppConfig.iconPath 消费，
                  EUI-NEO resolveIconPath + glfwSetWindowIcon）。
- aki-icon.ico  : 16/24/32/48/64/128/256 多尺寸 Windows exe 内嵌图标
                 （aki_app_icon.rc → IDI_APP_ICON，替换 EUI-NEO 默认
                  EUI_NEO_APP_ICON_RESOURCE）。

缩放采用 alpha 预乘（premultiplied）后 Lanczos，再逐像素还原直通 alpha，
避免浅色描边在深色底上出现暗边。PNG 帧编码的 ICO 需 Windows 10+ shell
（Aki 目标平台即此）。

用法：python scripts/make_app_icons.py [源 PNG 路径]
（默认源路径对应 pinned heyaki v1.0.1-38-ge114508；更换 pin 后重跑本脚本
并同步更新 README.md 溯源。依赖 Pillow 与 numpy。）
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = REPO_ROOT / "third_party" / "heyaki" / "docs" / "icon" / (
    "heyaki-transparent.png"
)
OUTPUT_DIR = REPO_ROOT / "assets" / "icons"

ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
WINDOW_ICON_SIZE = 256
# 裁边后四周保留内容边长的 2.5% 空白，避免圆角/任务栏底板贴边。
CONTENT_MARGIN_RATIO = 0.025


def premultiplied_resize(image: Image.Image, size: int) -> Image.Image:
    """alpha 预乘后缩放，再按缩放后 alpha 逐像素还原为直通 alpha。"""
    if image.size[0] != image.size[1]:
        raise ValueError(f"expected square input, got {image.size}")
    arr = np.asarray(image, dtype=np.uint32)
    premultiplied = arr.copy()
    premultiplied[:, :, :3] = arr[:, :, :3] * arr[:, :, 3:4] // 255
    premultiplied_image = Image.fromarray(premultiplied.astype(np.uint8), "RGBA")
    resized = premultiplied_image.resize((size, size), Image.Resampling.LANCZOS)
    result = np.asarray(resized, dtype=np.uint32)
    alpha = np.maximum(result[:, :, 3], 1)
    for channel_index in range(3):
        values = result[:, :, channel_index]
        result[:, :, channel_index] = np.minimum(
            255, (values * 255 + alpha // 2) // alpha
        )
    return Image.fromarray(result.astype(np.uint8), "RGBA")


def square_crop(image: Image.Image) -> Image.Image:
    """按 alpha bbox 裁边、补 2.5% 边距并居中到正方形画布。"""
    bbox = image.getchannel("A").getbbox()
    if bbox is None:
        raise SystemExit("源图 alpha 全透明，无法生成图标")
    content = image.crop(bbox)
    margin = round(max(content.size) * CONTENT_MARGIN_RATIO)
    side = max(content.size) + margin * 2
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    offset = ((side - content.width) // 2, (side - content.height) // 2)
    canvas.paste(content, offset)
    return canvas


def main() -> None:
    source_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SOURCE
    source = Image.open(source_path)
    if source.mode != "RGBA":
        raise SystemExit(f"源图需为 RGBA（含 alpha），实际 {source.mode}: {source_path}")
    if source.width != source.height:
        raise SystemExit(f"源图需为正方形，实际 {source.size}: {source_path}")

    square = square_crop(source)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    window_icon = premultiplied_resize(square, WINDOW_ICON_SIZE)
    window_icon_path = OUTPUT_DIR / "aki-icon.png"
    window_icon.save(window_icon_path, format="PNG", optimize=True)

    frames = [premultiplied_resize(square, size) for size in ICO_SIZES]
    ico_path = OUTPUT_DIR / "aki-icon.ico"
    # 以最大尺寸帧为主图（Pillow 以主图尺寸为帧上限过滤），其余尺寸经
    # append_images 按尺寸精确匹配；默认 PNG 帧编码（Windows 10+ shell）。
    frames[-1].save(
        ico_path,
        format="ICO",
        append_images=frames[:-1],
        sizes=[(size, size) for size in ICO_SIZES],
    )

    print(f"source: {source_path}")
    print(f"wrote {window_icon_path} ({window_icon_path.stat().st_size} bytes)")
    print(f"wrote {ico_path} ({ico_path.stat().st_size} bytes), sizes={ICO_SIZES}")


if __name__ == "__main__":
    main()
