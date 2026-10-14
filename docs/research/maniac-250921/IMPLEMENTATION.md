# Maniac 250921 报告范围内的兼容补完

本次以 [REPORT.md](REPORT.md) 为范围依据，目标制品仍为该报告固定的 250921 runtime，SHA-256 为 `082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f`。报告和静态证据先独立提交为 `86acf0eb2`；本文件记录后续代码改动及检查边界，不把审计快照中的源码行号更新成新的调查结果。

用户指定不运行原版行为对照、不逐条验证指令、不扩展报告范围。本次没有运行原版、实际游戏、浏览器或人工 UI 验收，没有增加永久测试代码。临时检查只针对共享合同，在本次任务内编译运行后移除源码。字体选择、字体回退、字形生成、用户输入和平台资源接口继续使用 EasyRPG。

## 实现对应关系

|报告项|本次处理|主要位置|
|---|---|---|
|D01 / S1|3007 保留真实参数数量，按 UTF-16 游标消费正文、两个配置及五位引用；共享转义参数解析器，区分 NULL 与 present-empty；保留 SELF caller|[game_interpreter.cpp](../../../src/game_interpreter.cpp)|
|D02 / S2|创建时解析 V/T/N，插入文本不递归；保存有效正文、system、font；从原版 kind1 和 chunk1 重建，恢复不重新读创建期变量；异步请求使用创建快照|[game_windows.cpp](../../../src/game_windows.cpp)|
|D03 / S2|原始整数分子、double target、double current angle 分开；实现 Move 低两位切换表、保留特效/时间/色调选项、相对位置及插值顺序|[game_pictures.cpp](../../../src/game_pictures.cpp)|
|D04 / S3|使用 100 尺度原版饱和度、RGB tone 和 flash 公式，保留各自取整及 byte 重组顺序；图片不再借用通用 Y'CbCr tone|[maniac_picture_renderer.cpp](../../../src/maniac_picture_renderer.cpp)|
|D05 / S3|实现四种 blend、chromakey、完整/部分 opacity 的组合；保留 257 权重、Overlay 截断位置以及逻辑 alpha|同上；[bitmap.cpp](../../../src/bitmap.cpp)|
|D06 / S3|native-size、整数 zoom、double rotation、wave 分别取样；包含 nearbyint、反向缩放、奇数尺寸、旋转的 -0.1 起点与 +1 边界、wave 查表和裁剪后的原始取样因子|同上；[sprite_picture.cpp](../../../src/sprite_picture.cpp)|
|D07 / S2|文字 style word 不经过普通 bottom-transparency 赋值/插值；移动保留字号 byte 和指定 blend/flip nibble|[game_pictures.cpp](../../../src/game_pictures.cpp)|
|D08 / S2|Normal 停止旋转余波按原版写回 power 字段，随后独立判断 angle|同上|
|D09 / S3|按整个 flip nibble 分派；未知值默认复制；whole-image 与 sheet 的小尺寸 V/HV 门槛分开|[maniac_picture_renderer.cpp](../../../src/maniac_picture_renderer.cpp)|
|D10 / S4|游戏逻辑的 Rand 整数入口与变量/表达式/数组使用同一 Maniac LCG；UTC 当天毫秒初始化；纯显示粒子、天气纹理和转场仍保留 EasyRPG 随机入口|[rand.cpp](../../../src/rand.cpp)、[main_data.cpp](../../../src/main_data.cpp)|

图片准备结果由显示、处理后像素读取和 PNG 输出共用。原始直通道像素另行保留，避免 premultiplied 表面丢失透明像素下的 RGB；显示用 alpha 与原版逻辑 alpha 分开。修改源像素和清除区域会使准备缓存失效，复制普通 EasyRPG bitmap view 不进入直通道重建。

## 同一报告合同中的入口和持久化细节

- 普通 Show 的短参数记录使用原版 `0x61` 生命周期默认值，显式零仍为零；Show/Move 不套普通引擎的消息 busy 门槛。
- follow/chromakey 使用低 byte；独立高度使用对应 nibble 的 inherit/literal/variable/indirect 规则。文件名使用其独立字符串模式；后缀替换的 GLOBAL-only 读取和 UTF-16 长度不与一般 SELF helper 混用。非正截取长度的短名称保留确定的重复行为；原版未终止内存扫描不复制。
- sheet 支持独立列/行引用、静态 frame、自动完整范围和自动指定范围；起止帧按 signed32 转换，保持逆序范围，timer/end 比较使用 unsigned，只有结束模式恰为 1 才 erase。缩放原点先使用 signed32 百分比乘除，负半尺寸保留奇数像素。
- 同名且 sheet 状态未变时复用 backing，保留该 backing 的 chromakey；命令保存的透明 byte 仍独立更新。
- Maniac 总是使用图片 layer 排序；按完整 layer 值匹配已知绘制 pass。未知正 layer 继续更新状态，但不落入默认绘制层。
- liblcf 保存完整 follow/kind word、透明/间距 word、style、尺寸对、动画起止帧和完整结束模式。chunk `0x18` 的 Maniac 结束模式为四字节 little-endian 原始整数；普通引擎仍保留原 boolean 编码。未知 lifecycle bits 不丢失。
- 修改后的图片以 kind2 压缩 BGRA 源保存并恢复，透明 RGB 不经显示合成重新生成。frame flatten 同时清理动画范围。存档只把正的 remaining 保存为进行中状态，运行时保留 0 到 -1 的更新过程。
- 文字的布局、背景、边框、margin、字间距、行间距、图标尺寸、颜色采样、阴影与整块 clip 采用报告规则；字体掩码仍由 EasyRPG 产生。glyph 与 icon 的 system 颜色纵向取样偏移按各自静态路径处理。

### 固定角的入口区别

报告 §7.2 的 `denominator==0 ? 1 : denominator` 及负商加 360 适用于普通 Show/Move。3007 的既有静态入口 `036970`、特别是 `03741b..037454`，没有零分母替换，负商分支执行 `360-remainder`。本次对 3007 单独保留这一规则，包括非有限值进入后续数值转换的行为，不把三个入口归一化成“正确角度”。这属于报告已有 D03 和文字图片范围。

## 保留的共享消费者

报告 A01/A02 已有的实时 bounds、int32 位模式、CVTT 风格转换、表达式 pair/broadcast/重叠赋值、数组配对排序、UTF-16 字符串存在性、SELF 银行及原版存档框架继续复用。Pow 补齐 `pow → trunc → bounds → conversion` 的顺序；CMake 对相关数值/图片实现禁用 FP contraction，Autotools 同样禁用 contraction。

消息、动态指令、保存和战斗继续走现有消费者：3019 同次调用重新分派及重试、3002 快照后结果回写、消息 hook caller/参数包、战斗 hook 专用执行栈、raw ATB 与 action/target/替换 enemy 状态链。本次没有用一个统一包装器替换这些入口，也没有依据 handler 数量重写已经存在的机制。其既有源码和相关现有测试被纳入复核；这不等于完成全部原版战斗序列的行为验收。

## 构建和检查

验证使用固定 liblcf 基线的独立副本及项目内依赖补丁，没有借用参考 Player 工作区或本项目忽略目录内的未提交依赖修改。Windows Release 的 Player 和现有 test runner 均通过 MSVC 2022 编译、链接，必要新增文件同时列入 CMake 与 Autotools。

现有 504 个测试按文件顺序划分为 `1..288`、`289..331`、`332..504` 三个独立进程，覆盖全部范围；断言分别为 3,029,813、1,335、646,823，合计 **3,677,971**，均通过。分进程保留项目原有的验证组织，避免把测试之间的全局引擎配置当作当前改动的条件。

**172 项临时合同检查通过**，覆盖 blend/key/opacity/alpha 组合、tone、尺寸为 2 的 flip、未知 flip、整数 zoom、wave 裁剪、透明 RGB 的 PNG 往返、原版图片 word/尺寸/动画字段的 LSD 往返、共享随机状态、fractional angle 的逐帧更新、清除与缓存失效、kind2 源的恢复和再次保存、layer 排序和未知 layer。它们验证当前代码的有限合同，不运行原版，也不构成指令清单验收。

依赖补丁另在干净固定基线上检查应用，并核对生成的公开头文件与 reader 内容，避免只在本机修改过的依赖上编译成功。参考 Player 的 1,398 个基线文件经摘要复核保持不变。

## 本次结论的边界

已实现报告可确定的共享差异，保留报告明确允许的 EasyRPG 平台行为。原版数学内核仍使用宿主 `std::math` 实现；固定常数、步骤、转换和禁止 contraction 不能证明 Windows/Web/Android 的全输入域整数边界或 sin 表逐 bit 相同。报告列为后续工作的原版运行差分、regex 库差异、完整战斗/消息序列和三平台运行验收，本次按用户要求没有执行，也不以旧暂停任务的记录替代。

因此，这次提交提供报告范围内的实现和现有非 UI 检查结果；不声称所有游戏、所有原版 CPU 数值路径及所有运行序列已经获得等价性证明。
