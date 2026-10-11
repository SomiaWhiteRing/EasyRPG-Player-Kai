# 250921 运行兼容与三平台本地交付

2026-10-02 重置。唯一目标：**完整完成 `patch_selfvar_250921` 的运行兼容，交付 Windows、Web、Android 三个平台可复现的本地构建产物。**

## 范围契约

覆盖现有 [168 编号索引](maniac-250921-command-inventory.csv) 的有效运行行为，以及它们使用的 self/表达式、调用调度、字符串、图像消息、战斗、媒体输入、JS 宿主和存档机制，**但以当前交付范围为准**。Windows 固定 250921 原版提供语义参考；Web、Android 分别实现平台适配并验证。索引中的“已有 handler”不是通过证据，原版已证明的空操作可以登记为一致，不必创造功能。原生 `RPG_RT.rs1` packed-text 分支属于罕见的特殊格式，本次明确移出总交付门禁；其已知缺少 loader/producer 的状态保留为独立后续项，不阻止 Goal 完成。

不做 241028 差分、无消费者的内存布局复刻、原版崩溃/未定义行为逐项模拟、为搜集证据而搜集证据、无关引擎重构或对外发布。**可观察到的正常运行差异、数据损坏、存档续行错误必须修复**；不得以“边界”之名排除真实需求。对平台无法等价的系统能力写明适配行为和限制，未验证不能写成限制后直接算通过。

2026-10-11 用户明确统一启动策略：凡自动识别或手动启用的 Maniac 游戏，运行时均使用250921兼容分支，不按游戏使用的旧补丁版本分流。已有CLI/INI版本值不得使普通启动退回旧分支；显式关闭Maniac仍有效。当前修正与验收边界记录在看板 R-W12-02，历史指定版本样例不替代普通启动路径。

### 已核实的门禁误判更正（2026-10-11）

R-W02-03 旧“原版 livealias/clone producer”门禁不成立：固定原版 live+24 是地图ID（构造器current-map及LSDchunk11两处写入），并非克隆来源事件ID；正常创建/读档按同一事件ID校验定义。不能由12330存在一个definition-miss回退就推导出必须寻找的原版克隆功能。[字段与路径证据](../build/livealias-work-20261011/README.txt)。该推断撤销后，正常地图调用、稀疏页、错误、SELF及保存要求仍按既有证据验收；例外回退未宣称实测通过，具体正常工程/存档反例可按原ID重开。历史静态合同的旧字段命名不得重新形成无消费者门禁。

## 唯一执行入口

[执行看板](maniac-progress.md)保存固定工作包、剩余事项稳定ID、当前Goal/源码/候选状态、当前批次和检查点变更；168索引保存编号/handler/合同/证据定位。历史TSV、submode、implementation ledger只服务已登记的具体问题，不恢复为执行清单。`python docs/maniac-progress.py`只显示看板，不生成分母或改写状态。

旧计划与归档仍在`build/recovery-20261002/`，不可当普通缓存删除。其`historical-evidence.zip`保留原路径，`evidence-manifest.csv`提供哈希；仅按具体问题恢复所需文件。`retired-tools/`及清单保留一次性工具，不重新治理工作区。

## 执行与验收规则

1. 同时一批。修改产品、增加验收或调查原版前，关联看板已有ID，登记实际问题、允许修改面、预期结果与停止点。新发现先写既定必需行为/实际反例或缺分支/现有证据为何不覆盖/最小判定工作；有依据的范围内问题登记后直接推进，猜测先列待判定，范围外只记备注。改变交付范围需用户决定。
2. 按交付距离排序：正常运行错误、数据损坏、保存续行、阻塞后续验收优先；其次收口剩余条件少的包。共用路径说明证据复用依据，独立行为/平台适配不能未经验证合并。没有测过某组合本身不新增门禁；不得逐编号、逐子模式重新逆向。
3. 复用现有有效案例、冻结工具链和依赖。日常只构建受影响目标/运行既有doctest过滤；功能组稳定后必要三平台整合及一次完整非UI。失败只复跑失败及直接受影响项；同源码有有效结果不重复，纯文档不跑Player回归。Windows当前大翻译单元用parallel1。构建可并行，状态性运行串行；辅助工具问题修现有入口，不另造通用框架。
4. 行为必须实际执行目标路径；handler、构建或无目标路径的绿色测试不能替代。保存须完整结束、另一个PID或平台持久化后的新实例读档续行，不能只看文件出现或内存round-trip。分别记录产品/夹具/旧二进制/环境/取证时机错误；不得降低期望制造通过。
5. W01–W11满足本包必需行为实现/验收、登记必需缺口及影响关闭判断的疑问解决、所需平台运行保存齐备且源码有效、无已知阻断缺陷且必需共享依赖已满足，就立即已关闭。本包独立验收通过后立即记录，共享阻塞单列；看板总表使用工作包/总体状态/本包独立验收/剩余及共享事项ID/关闭条件/下一步。最终W12打包不阻止功能包完成自身验收并按条件关闭。仅新反例、遗漏的必需行为或相关源码使证据失效才重开，记录原因并只补受影响部分。
6. 每批结束或90分钟更新新增行为/修复、事项新增/关闭/合并/重开及事项内部独立工作扩大原因、本包独立验收/正式关闭判断与具体剩余ID或有限子问题、唯一下一步。沿用既有42项基线和稳定ID，不重新编号或重置余额。工作余额不是兼容百分比；不以测试/断言/代码/批次数表示兼容进度。连续两个检查点无有效推进，停止当前方法，不自动把问题改为外部阻塞；现有材料支持不同且可检验原因/方法时，写明预期观察与停止点，开展一次有界排障，否则保留未解决结论及具体缺失信息。所有剩余必需工作均无有依据且获授权的下一步时，按用户明确授权实际暂停Goal，保存现场和最小解除条件。
7. 当前用户授权固定范围必要Windows UI、Web浏览器、可用Android设备/模拟器验收及少量build临时工程/存档/辅助材料。复用项目测试，新增验证默认只放build，不自动增加项目测试或组合矩阵。子代理、本地模型翻译、推送、部署、Release、替换用户游戏程序未授权。历史“已授权”不代替当前用户授权；其它保护见[AGENTS](../AGENTS.md)。

平台适配沿用范围契约；模拟器/native bridge/noaudio/辅助启动与正常入口、物理设备证据分别写明。不得把未验证写成不适用/限制关闭。剩余事项分类为缺实现/待验收/待排障/外部阻塞/待判定/已关闭：设备、软件或材料已有而启动/运行/工具/验证失败且原因未定位，列待排障，由代理负责有依据诊断；样例或语义尚未查明列待判定；外部阻塞仅用于已确认现有授权/环境内无法取得的具体资源或用户决定，须列证据、不能自行解决原因和用户最小输入。一个ID内有限列已完成/剩余独立子问题，不按组合增叶项。共享问题阻止多个包关闭时优先一个有停止点的排障批次，区分产品/启动配置/工具/工程错误及资源确实缺失，利用已有材料建立最小成功基线；只有确认外部条件缺失或当前诊断无有依据下一步才暂时转向其它必需工作。历史失败方法无新依据不重复，不无依据换配置/设备/驱动。

## 总完成与最终交付

W01–W11按关闭规则全部关闭，当前交付范围内的固定168编号及共享机制无遗漏必需行为，且三个平台各自适用运行/保存门禁关闭后，冻结最终同一Player与dirty liblcf。`RPG_RT.rs1` packed-text 分支不属于本次总完成门禁，必须在交付说明中作为未覆盖的特殊格式限制保留。复用冻结依赖在三个新目录重建Win x64/完整Web必需组件与许可/Android arm64-v8a APK，对最终产物执行必要启动/代表行为/保存确认，统一交付源码快照、依赖/工具链来源、复现命令、清单、哈希及准确运行限制。

只有全部条件有当前有效证据，实际标记Goal complete并停止。旧快照构建不覆盖最终源码，构建不替代运行；可复现指冻结输入后可成功重建，未证明不宣称逐字节一致。阶段完成、预算耗尽、仅文档或外部阻塞不得冒充完成；达到实际预算限制时保存状态并停止实质工作。

## 本地构建入口

### 仓库内的固定依赖（2026-10-11 单提交整合）

Windows/Web/Android 的现有打包入口均启用 `PLAYER_BUILD_LIBLCF=ON`。已有 `lib/liblcf` 开发副本保持原样；没有该副本时，CMake 在构建目录获取 SomiaWhiteRing/liblcf 的 `4056dbf533182dd46c0a9b1831da23bc16cc0ae8` 归档，校验 SHA256 `6df846fa13f5af62334dcc2715fbf785a45a3607d4b0bce9b960af1e9b1036a9`，再应用仓库中的 `builds/patches/liblcf-maniac-250921.patch`。补丁汇总原本只在本机的两个提交和未提交修改；基线与补丁共同固定存档结构及生成器输入。补丁内容变化会选择新的依赖源码目录，避免复用旧的已修补源码。

QuickJS-NG 核心、许可与逐文件来源哈希保存于 `lib/quickjs-ng/`，版本为 v0.10.1。Android 视频解码器首次构建会下载并校验固定的 FFmpeg 7.1.5 压缩包；离线构建可用 `-DPLAYER_MOVIE_FFMPEG_ARCHIVE=/path/to/ffmpeg-7.1.5.tar.xz` 指定已有文件。三个平台不再依赖开发者未提交的依赖修改或另一个平台的 build 残留。已有本地 liblcf 副本仍由开发者负责其版本和脏改动，不能把该覆盖入口当作干净依赖验收。

Windows 使用已核对指向本仓库的 `C:\Player`，配置/构建进程的实际工作目录也必须是该 ASCII 路径；仅设置 `-S`/`-B` 不能避免本机 CMake 的路径问题。本地 liblcf 不得被构建脚本删除。

```powershell
# 使用当前源树的 Windows 构建；已有非 UI 测试按改动影响运行。
Set-Location C:\Player
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --preset windows-x64-vs2022-release-local
& $cmake --build --preset windows-x64-vs2022-release-local --target EasyRPG_Player_exe --parallel 1

# Web / Android 复用仓库现有 Docker 打包入口和已安装镜像。
.\builds\package-docker.ps1 web -SkipImageBuild
$env:ANDROID_ABIS = 'arm64-v8a'
.\builds\package-docker.ps1 android -SkipImageBuild
```

上述是当前开发入口，最终复现必须保存实际工具链版本、Docker image ID、源码快照和依赖。Docker/CI 缓存可重用，但来源必须记录；不能在验收途中更新依赖。Windows 如运行 `builds/ci/package-windows-nightly.ps1`，传入本地 `C:\buildscripts`，脚本现在保留已有 liblcf。

最终本地交付目录必须含：`windows/Player.exe`、Web 的 JS/WASM/data/Workers/媒体解码器及许可文件、Android arm64-v8a APK（注明 debug 签名；其它 ABI 按需构建，不默认四套重跑），以及源码快照（含 dirty liblcf）、`manifest.json`、复现命令和已知限制。重建可复现指冻结输入和步骤后能成功重建；若未证明逐字节一致，不宣称 bit-for-bit reproducible。

### 已验证的新目录Windows复现

3020修正前源码快照的[Windows新目录材料](../build/repro-win-20261007/README.txt)包含独立source.zip、逐文件清单、冻结toolchain-windows.zip（SHA256 `2c2e621e4406bc2444edecffbf95f17ad377a2fbcbcd916f975db71d470e5dc4`）、prepare.py和build-windows.ps1。从新的ASCII目录还原源码及x64静态依赖后，空objects目录配置/编译通过；没有沿用开发对象文件、没有删除/重克隆liblcf。

复现时保留记录的MSVC19.44.35222.0/14.44.35207、Windows SDK10.0.26100.0、CMake3.31.6-msvc6，运行 `python prepare.py` 后运行 `pwsh -NoProfile -File <ASCII目录>/build-windows.ps1`。脚本从自身目录推导source/deps/objects；默认CMake位置见脚本。依赖包是冻结预编译包，编译器/SDK复用固定安装，不宣称重新构建全部依赖或逐字节一致。

本证据只关闭该快照的Windows新目录构建缺口；其它平台及运行/存档状态以看板为准。源码不变时不重跑Player回归。

### 已验证的新目录Web离线复现

同一3020修正前源码快照的[Web离线复现材料](../build/repro-web-20261007/README.txt)包含source.zip、逐文件清单、SDK/静态依赖归档web-toolchain.tar.gz（SHA256 `cd8f38f4a42a045e1f6b165fcf9e6fe4b4debd3e9e7e810de467b6657b90b191`）、固定FFmpeg7.1.5源码包和完整脚本/清单。新Docker卷、空对象目录、新编译器缓存与`--network none`下配置/核心及解码器编译、完整18文件包核对通过；没有沿用旧Player/FFmpeg对象或媒体二进制。

保留固定Docker镜像`sha256:3993602bb3c04fc6115176d756994fd1970cb574cacb2e38c078beb29d215c00`，按README把材料放到新目录，运行 `pwsh -NoProfile -File <材料目录>/build-web.ps1 -VolumeName <新的唯一Docker卷名>`。SDK5.0.5/Node22.16.0随归档提供，镜像提供Python3.12.3/CMake3.28.3/Ninja1.11.1；脚本仅确定地重定位27个安装路径元数据文件，二进制依赖和SDK保持，源码包包含dirty liblcf。

本证据只关闭该快照的Web新目录构建/打包缺口；预编译第三方库/SDK使用冻结包，不宣称全部依赖源码重建或逐字节一致。其它平台及实际运行/存档以看板为准；源码不变不重跑Player回归。

### 已验证的新目录Android离线复现

同一3020修正前源码快照的[Android离线材料](../build/repro-android-20261007/README.txt)包含source.zip、SDK/NDK/ARM64静态依赖归档、Gradle8.13分发/Maven下载依赖、SDL3.4.8 AAR及脚本/清单。固定镜像、新Docker卷、空项目/原生目录、关闭ccache/Gradle构建与配置缓存，`--network none`/`--offline`实际assembleDebug成功；APK3份ARM64 ELF/当前剥离核心/SoundFont/SDL合并与剥离链、同一debug证书及v1/v2签名核对。

保留与Web相同的固定Docker镜像，按README准备材料后运行 `pwsh -NoProfile -File <材料目录>/build-android.ps1 -VolumeName <新的唯一Docker卷名>`。SDK36/NDK28.2.13676358/CMake3.22.1、Gradle8.13/AGP8.13.2/SDL3.4.8冻结，JDK17.0.20.1来自固定镜像；同一source.zip包含dirty liblcf，不能删除或重克隆。

安装路径配置仅确定重定位；SDK自动生成16字节安装包索引指纹另存前后SHA，其它SDK和二进制输入不变。Gradle分发/Maven下载依赖可重用，旧项目/原生对象及转换/构建缓存未复制；不宣称预编译依赖源码重建或逐字节一致。

三个平台该快照的新目录本地构建材料现已齐备；完整兼容及平台运行/原版存档互通/新进程门禁继续按看板开放。源码不变不重跑Player回归。

上述新目录证据固定对应source.zip SHA256 `ad5c4ca920f9b2700b38675cef61d205b4f6c9d5a1b5ead096d431474e15eb81`。后续源码修正采用同源增量构建；旧新目录证据不自动覆盖新快照。全部必需功能的最终源码稳定后，再复用冻结依赖更新三个新目录门禁一次。
