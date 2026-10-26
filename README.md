# EasyRPG Player - Kai

EasyRPG Player 的苍旻白轮个人魔改造版。基于 EasyRPG Player 0.8.1.1。

当前维护 Windows 11 x64、Web/Emscripten 和 Android 版。
软件本身的功能请参阅[官方项目](https://easyrpg.org/)说明。

## 主要改动

### 追加功能开关

按 `F1` 打开系统设置菜单后，可以进入 `追加功能` 单独开关本分支新增功能。

这些开关默认启用，也会写入配置文件的 `[Player]` 段：

- `ExtraMessageHistory`：对话历史记录
- `ExtraMouseSupport`：鼠标功能补完
- `ExtraMoviePlayback`：兼容视频播放
- `ExtraRecommendedSoundFont`：使用作者推荐的SoundFont
- `ExtraNameInputChoices`：输入式解谜自动选项化
- `ExtraHideManiacLogs`：隐藏 Maniac 相关日志
- `ExtraForceTestPlay`：强制启用 EasyRPG 调试模式

### 隐藏Maniac相关的错误提示

顾名思义。毫无意义又烦人。

### 强制TestPlay

开启 `ExtraForceTestPlay` 后，会强制启用 EasyRPG 的 TestPlay / Debug 模式，效果等同于启动时传入 `--test-play`。

关闭该开关后，仅在显式使用 `--test-play`、`TestPlay` 或在游戏浏览器中以调试方式启动游戏时启用调试模式。

### Web 随附 SoundFont

Web 版默认启用 `ExtraRecommendedSoundFont`，使用 `resources/soundfonts/recommended.sf2`。自动构建将它打包到 `easyrpg-player.data`，启动时预加载为虚拟文件 `/builtin/recommended.sf2`；无需给每个游戏重复添加音色库。

本分支的 Web ZIP 专供 VIPRPG-ZH-Archive 使用。站点通过 `player-host.js` 的 `createEasyRpgPlayer({ runtimeBase, workId, packages })` 启动游戏和音频两个 Worker，将已经安装的 OPFS pack 挂载为 WORKERFS。文件读取使用有容量上限的内存缓存；游戏 Worker 输出 OffscreenCanvas/WebGL2，音频 Worker 复用原生解码器与混音器并直接供给 AudioWorklet，游戏帧阻塞不会停止持续供音（见[音频架构](docs/web-audio.md)）。部署时须完整保留 ZIP 中的 JS、WASM、data 和许可文件，包括按需加载的视频模块（见[视频部署说明](docs/web-movies.md)）。不需要 SharedArrayBuffer 或跨源隔离。销毁播放文档前必须等待返回对象的 `stop()` 完成，以确认 IDBFS 存档已经写入并结束两个 Worker。

初始化脚本保留 Emscripten 音色库预加载回调，等待资源挂载后才启动播放器。浏览器已保存的设置仍然有效：若之前关闭过额外功能中的「MIDI音效改良」或音频设置中的 FluidSynth，请在 `F1` 设置中重新启用；首次使用默认开启。

初始化 MIDI 合成器后，调试日志中的 `Fluidsynth: Using soundfont /builtin/recommended.sf2` 表示随附音色库已成功加载。

### 添加对话历史记录

在对话状态下，按下`~`键（ESC键下方的那个）或鼠标滚轮上滚可以打开对话历史记录界面，查看之前的对话内容。  
支持键盘和鼠标滚轮翻页。取消键和鼠标右键退出。

### 鼠标功能补完

开启 `ExtraMouseSupport` 后，在对话状态/标题界面/存读档界面下，鼠标左键能够起到和确定键相同的效果。

普通对话等待继续时，鼠标滚轮下滚也等同于确定；选项选择和数值输入不会把滚轮下滚当作确定。

### 加速倍率支持小数

`Fast Forward A/B` 的倍率现在支持一位小数，范围为 `0.1` 到 `100.0`。

小于 `1.0` 时会作为减速使用，例如 `0.1` 表示十分之一速度。

### 兼容视频播放

兼容RPG Maker事件中的播放视频功能。  
会从游戏目录下的`Movie`文件夹查找`.avi`和`.mpg`文件，并在游戏窗口内播放。  
视频播放结束后会继续执行后续事件。

Web 版优先使用浏览器原生视频播放，失败时按需加载精简的 FFmpeg/WASM 解码器，支持 AVI 中的 DivX/Xvid、Microsoft Video 1、Cinepak、Indeo 3、Motion JPEG，以及 MPEG-1/2 等旧格式。解码器不包含编码器或转码流程，在独立 Worker 中逐帧解码，直接读取本地 Blob 切片；无需修改原游戏资源。Web 还会查找 `.mpeg`、`.mp4`、`.webm`、`.ogv` 和 `.mov`。具体范围、内存限制和构建方法见[Web 视频说明](docs/web-movies.md)。Android 尚未实现对应的视频播放后端。

### 兼容 `ChangePartyMember(actor 0)`

部分 RM2000 游戏会在“重建队伍”时先批量执行“移除队伍成员”，并且从 `actor 0` 开始。  
原版 `RPG_RT` 会静默忽略这条无效命令；本分支现在也按同样方式处理，不再在顶部弹出
`ChangePartyMember: Invalid actor ID 0` 警告。

已确认这能兼容 `もしもコレクション7` 一类使用该写法的工程。

## 版本与自动发布

在仓库根目录编辑 [RELEASE.md](RELEASE.md)：第一行填写 `# 年.月.当月序号`（例如 `# 2026.9.1`），下面填写当前版本的 Markdown 更新日志。月份为 1–12，序号从 1 开始，均不补零；CI 会校验格式和非空日志。

每次提交到 `my-feature-stable`，或在该分支手动运行 **Build and Release**，都会构建并更新两类 GitHub Release：

- `nightly`：始终跟随发布分支的最新成功构建，标记为预发布。
- `RELEASE.md` 指定的版本：只有版本号高于已发布的最高正式版本时才新建正式 Release，并标记为 GitHub **Latest**。版本号不变或降低时只更新 Nightly；已有正式版的标签、日志和下载文件保持不变。

三个平台固定使用同一个提交，全部成功后才发布；过时构建会跳过发布，等待新提交的构建。升级版本时，正式版和 Nightly 使用同一批二进制；其余构建只更新 Nightly。Web ZIP 与 Android APK 的正式版文件名包含版本号，Windows 保持 `Player.exe`。

每个 Release 附带 `release-manifest.json`，记录提交 SHA、工作流运行、版本、文件大小和 SHA-256，发布日志也包含这些校验值。Nightly 覆盖不会保留旧包，复现问题时请同时记录版本号与提交 SHA；历史构建另受 GitHub Actions 产物保留期限制。单个附件上传限时 5 分钟、最多尝试 3 次，发布任务总限时 30 分钟；仍失败时工作流会失败，可在分支仍指向该提交时重跑失败任务；正式版草稿只允许在标签仍指向同一提交时继续上传，已发布正式版不覆盖。GitHub 对 Nightly 的多个附件替换不提供原子操作。

这里的年月版本表示 Kai 分发版本；EasyRPG 上游基础版本以及 Android 的递增 versionCode 仍由各自构建配置管理。Android 自动发版使用 Release 构建和独立的正式签名，配置方法见 [Android 发布签名](builds/android/release-signing.md)。

Android 从旧 Debug 签名切换到正式签名后，首次安装不能覆盖旧版。请先备份存档及需要保留的数据，再卸载旧版并安装新版；此后的正式签名版本可继续覆盖更新。

## 本地打包

本地标准打包入口以 Docker Desktop 为准，和 `.github/workflows/nightly-release.yml` 共用同一套仓库内脚本，不再维护旧的本地 VS / CMake 手动 zip 流程。先启动 Docker Desktop，然后在仓库根目录运行：

```powershell
.\builds\package-docker.ps1 all
```

也可以只打某个平台：

```powershell
.\builds\package-docker.ps1 web
.\builds\package-docker.ps1 android
```

脚本会先构建本地打包镜像 `easyrpg-player-kai-package:ubuntu24.04`，再把仓库挂载到容器里的 `/workspace`，调用 `builds/ci/package-ubuntu-nightly.sh` 执行和 GitHub Workflow 相同的 Web / Android Nightly 构建、校验和打包逻辑。首次构建会下载并生成 EasyRPG 工具链，时间会比较长；后续会复用 `external/local-docker/` 下的工具链缓存。

默认输出到 `build/artifacts/`：

- `EasyRPG-Player-Kai-nightly-web.zip`
- `EasyRPG-Player-Kai-nightly-android.apk`

Web ZIP 由 VIPRPG-ZH-Archive 的运行时导入脚本接入网站。网站负责将游戏安装到 OPFS，并把 pack 与文件切片索引交给 `createEasyRpgPlayer`；不再提供独立的 `games/default/` 页面或 `indexgen.php`。

Windows x64 的 `Player.exe` 由 GitHub Workflow 的 `windows-2022` job 调用 `builds/ci/package-windows-nightly.ps1` 生成；它依赖 Visual Studio 2022 runner 和 `x64-windows-static` vcpkg 工具链，不在本地 Linux Docker 容器里另建一套交叉编译流程。标准分发产物以 Nightly Action 上传的 `Player.exe` 为准。

Android 本地打包和 CI 均需要四个 `ANDROID_RELEASE_*` 签名环境变量，见 [Android 发布签名](builds/android/release-signing.md)。缺少签名配置时构建会直接报错。普通开发仍可在 `builds/android` 中使用 `assembleDebug`。

Android APK 保留 armeabi-v7a、arm64-v8a、x86 和 x86_64 四种架构，使用压缩的原生库将下载体积控制在 100 MB 以下；构建步骤会检查压缩方式与包大小。Android 安装时会解压原生库，因此下载体积减小不代表安装后占用按同比例减小。
