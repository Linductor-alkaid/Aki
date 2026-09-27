# Aki 应用图标资产

| 文件 | 用途 |
| --- | --- |
| `aki-icon.png` | 256×256 RGBA 窗口/任务栏图标，`dslAppConfig.iconPath`（main.cpp）运行期消费 |
| `aki-icon.ico` | 16/24/32/48/64/128/256 多尺寸 exe 内嵌图标（`IDI_APP_ICON`；根 CMakeLists 在 WIN32 下覆写 `EUI_NEO_APP_ICON_RESOURCE` 属性，替换 EUI-NEO 默认注入的上游图标） |

采用决定：2026-09-27 经用户确认（设计约束见
[aki_ui_design.md](../../docs/design/aki_ui_design.md) §2.6）。

## 溯源

- 源图：pinned `third_party/heyaki/docs/icon/heyaki-transparent.png`
  （1254×1254 RGBA；heyaki v1.0.1-38-ge114508，见
  `third_party/dependencies.lock.json`）。
- 许可：MIT（`third_party/heyaki/LICENSE`，Copyright Linductor-alkaid and
  Heyaki contributors）。
- 生成物为确定性输出（固定源图 + 固定缩放参数），入库后与源图一起版本管理。

## 再生成

```
python scripts/make_app_icons.py
```

依赖 Pillow 与 numpy。更换 heyaki pin 后重跑本脚本，并同步更新本说明与
设计文档中的版本描述。
