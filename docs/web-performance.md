# Web 高负载修复流程

网页性能问题先确认实际发布的源码、运行包和游戏归档。获得本轮运行授权后，在真实关卡使用相同逻辑帧输入、视觉随机种子和 Maniac 随机种子比较；`--seed` 不固定 Maniac 的独立随机数状态。教程、静止画面和构建通过不能代替高负载关卡验收。

CPU profile 用于定位调用路径。正式计时关闭采样器，并区分逻辑更新成本、整个主循环回调、画面提交间隔及实际对象数量。避免与编译、压缩同时测量。采样器可能显著改变 Wasm 的执行成本，不能把采样时的倍率直接当作实际 FPS。

事件和表达式优化必须保留求值次数、读写顺序、随机推进、SELF、槽零、短记录补齐和输入别名。表达式读取仍需快照，不能直接引用可能在求值期间被写入的输入存储。快路径未覆盖的模式继续进入通用实现；每个省略补齐的模式必须逐项核对其最大参数下标。

## 异常与暂停机制

Web Player 和内置 liblcf 统一使用 `-fwasm-exceptions`，链接保留 `WASM_LEGACY_EXCEPTIONS=1`。libpng 和 FreeType 必须按相同 SDK 版本重新编译为 `SUPPORT_LONGJMP=wasm`；不能只切换 Player 链接选项后复用 JavaScript longjmp 的旧库。不要通过关闭异常绕过分配失败的既有处理。

支持 `WebAssembly.promising` 和 `WebAssembly.Suspending` 的浏览器使用 JSPI，其余浏览器使用同源码的 Asyncify 包。宿主只选择一次运行时，资源预取、游戏 Worker 和音频 Worker 必须使用同一个选择。JSPI 的 `web_main_loop` 是异步导出；下一次动画帧必须等本次 Promise 完成后再调度，防止等待输入时重入解释器。

保留 Asyncify 的等待语义，不使用未经调用链证明的 `ASYNCIFY_REMOVE` 或忽略间接调用来换速度。Native EH 与 Asyncify 混用的编译警告仍保留；当前 C++ catch 块不执行异步等待。新增 catch 内等待时应重新检查该限制。

Binaryen 对单一调用者默认可能无限内联。发布构建限制 `one-caller-inline-max-function-size=200`，使常见事件分派与庞大的通用指令路径保持分离。

## Maniac 图片准备

普通资源图片在逻辑更新时记录帧、色调、闪光和源版本，绘制或读取处理后像素时才生成对应结果；读取必须使用该逻辑更新捕获的状态。透明度恰为 100、无效帧保留此前的处理结果。可修改图片仍及时更新处理缓冲，并保留当前帧之外的历史像素。资源转为可修改图片前，先完成待处理结果，并将共享帧视图脱离为独立缓冲。

无效果的资源帧使用持有原图的只读视图；调色帧复用缓冲，以批量内核保持 Maniac 的整数截断、饱和度、闪光、透明色及直通 Alpha 规则。性能修改不能以普通 EasyRPG 调色公式替换这些规则。缓存只回收无人引用的位图；单张超过缓存预算的大图不享受最近 50 毫秒的额外保留，以免连续预载累积大图。

等待中的预读仍限制为 16 条、静态文件名和一个待解码图片；可跨过擦图、切换远景及文字图片，不跨控制流。实测使用原始游戏颜色、等待和动画速度，对比音乐启动偏移与之后新增的画面偏移；二者不能混为同一个数值。

## 构建与发布

使用 `builds/ci/package-ubuntu-nightly.sh web`（或既有 Docker 包装入口）。它先准备匹配 ABI 的两个 C 依赖，再从相同核心对象构建 Asyncify 和 JSPI 两个入口。发布 ZIP 必须包含 `easyrpg-player.{js,wasm}`、`easyrpg-player-asyncify.{js,wasm}` 和唯一的 `easyrpg-player.data`；两种入口共用同一份 SoundFont。Worker 的 `locateFile` 只为 WASM 选择变体，网站预加载也始终请求共用 `.data`，不能再复制 `easyrpg-player-asyncify.data`。手工配置前可运行 `builds/emscripten/build-native-exception-deps.sh <SDK目录> <输出目录>`，并将输出的 `lib/libpng16.a`、`lib/libfreetype.a` 分别传给 `PNG_LIBRARY_RELEASE`、`FREETYPE_LIBRARY_RELEASE`。

获得运行授权后，除密集关卡外还检查实际异步等待、异常捕获、PNG 错误跳转、音频及停止退出。比较实际游戏状态，不以少执行事件、减少对象、增加逻辑预算或降低刷新要求冒充优化。测试钩子、固定 Maniac 种子和压力场景参数只用于任务内验证，发布前移除。

网站更新还需同步资源白名单、Brotli 路由、启动共享模块和 Android 的两个引擎适配；保留已有宿主补丁、字体声明和存档协议。确认当前任务有部署授权后，发布唯一的新版本化 URL，并读取线上元数据和两种运行时资源验证结果。
