# Zfull small default fonts

11px 及以下的内置默认字体以 Zfull-GB 为主。8、9、10、11px 各自使用
对应的原生点阵设计；9px 汉字的原生横向步进为 8px，绘制和测量均保留此度量。
1—7px 按目标尺寸最近邻缩小 8px 字形。

缺字时依次查 Zfull 的 10、11、9、8px 表，并按请求尺寸缩小或留白；
只有四档 Zfull 都缺失的常用符号才从开放字库补入，其他字符仍使用 Zfull 的问号。
8、9px 原生字库未覆盖的 GBK 汉字由 10px 字形补全，这部分不是原生小字号设计。
各档实际嵌入的字符数量和原生 GBK 汉字覆盖见 `coverage.json`。

补字范围固定为前次常用字符审计缺失的 35 个符号，包括 `・〜ゔ`、
`♡♥♪♫♬`、勾叉、部分箭头及键盘符号、货币符号、`㊗㊙` 等。
优先使用 Fusion Pixel 的原生 8px/10px 字形；9px/11px 添加留白，
只有较大原生字形可用时才缩小。剩余符号用 GNU Unifont 按目标字号栅格化。
不覆盖任何已存在的 Zfull 字形。每个补字的来源记录在 `coverage.json`。

Unicode 特殊空白按其含义提供空白字形和步进；BOM、零宽空格、连接符、
双向排版控制和变体选择符不显示，也不额外增加字距。
这不提供双向排版、连字或 emoji 序列塑形能力。

标准 12px 内置字形保持原样；大于 12px 时按目标尺寸最近邻放大原有点阵，
测量、字形掩码和步进同步缩放。游戏附带字体和用户指定字体的优先级保持原有行为。
FreeType 字体缺字时向后备字体传递当前字号。字体数据直接编译进 Player，
不依赖系统字体、运行时下载或 FreeType。

## 来源

- [Zfull 整理项目](https://github.com/pixel-font-legacy/zfull-pixel-font)
- [2026.08.11 发布包](https://github.com/pixel-font-legacy/zfull-pixel-font/releases/tag/2026.08.11)：
  `zfull-pixel-font-gb-otf.woff2-v2026.08.11.zip`
- 使用其中 `Zfull-GB-8px.otf.woff2` 至 `Zfull-GB-11px.otf.woff2`。
- 保留原始版权元信息 `Zfull-GB.2009`。整理项目标注原字体作者及版权不明，
  其构建程序的 MIT 许可不适用于字体数据；完整原文保存在 `SOURCE-NOTICE.md`。
  本项目没有将这份字体数据重新标注为 OFL、MIT 或公有领域。
- 补充符号的衍生名称为 **Small Pixel Symbols**，使用 SIL OFL 1.1：
  [Fusion Pixel 2026.10.25](https://github.com/TakWolf/fusion-pixel-font/releases/tag/2026.10.25)
  及 [GNU Unifont 18.0.01](https://unifoundry.com/pub/unifont/unifont-18.0.01/font-builds/)。
  补字来源许可与版权说明位于 `licenses/`，不改变 Zfull 部分的版权状态。

## 再生成

将下列文件放在输入目录：

- `zfull-pixel-font-gb-otf.woff2-v2026.08.11.zip`
- `fusion-pixel-font-8px-monospaced-otf.woff2-v2026.10.25.zip`
- `fusion-pixel-font-10px-monospaced-otf.woff2-v2026.10.25.zip`
- `unifont-18.0.01.otf`

再生成需要 Python、fonttools[woff]、Pillow；
本次使用 fontTools 4.55.0、Pillow 10.4.0。普通 Player 构建不需要这些工具。

```powershell
python resources/smallfonts/generate.py C:/path/to/font-inputs
```

生成器以每档原生尺寸栅格化，保留字体的字形位置和步进，
排除无法完整放进字框的字形，不裁掉超框笔画。
原始 Zfull 缺失的半角空格（U+0020）、不换行空格（U+00A0）及
全角空格（U+3000）补入空白字形及其步进，避免空白被替换成问号。
生成 `src/generated/bitmapfont_small.h`、`coverage.json`、`SOURCE-NOTICE.md`，
并提取 Fusion Pixel 许可。Unifont 的许可与字体元信息单独保留在 `licenses/unifont/`。

本次完成资源生成与静态检查，未构建、运行游戏或进行画面验收。
