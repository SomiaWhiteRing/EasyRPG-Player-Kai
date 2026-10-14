# R1–R3 有限执行链的实现与结项

2026-10-15。依据[固定行动准则](ACTION-GUIDELINES.md)及[六条消费链、24项义务的范围报告](next-bounded-scope-20261015.md)，按 W1 → W2 → W3 → W4 → W5 完成。本轮产品提交为 `1831230078581afe564ef805feca6f1559713908`，范围报告提交为 `8d49eb6bb`。

本轮关闭的是这24项限定义务：修正 D1–D5，连接资源暂停后的真实消费者，保护调用返回和存档骨架，并将新增状态接入退出点。产品改动16个文件，新增261行、删除114行；没有重写参数系统、调度器或存档 codec。检查采用源码核验、504个既有非UI用例和20个任务内临时消费者用例；没有进行原版对照、实际游戏或平台/UI验收。

## 1. 按顺序完成的实现

|阶段|最终改动|保留的行为|
|---|---|---|
|W1：参数及保存入口|E输出按数字/字符串base分别抑制根读取；保存请求独立保留result-enabled；3002对slot16+保留last-slot|嵌套ID仍求值；禁用结果不额外刷新；普通保存后端继续按默认规则更新last-slot|
|W2：同帧生成命令|3019只在YieldRepeat时暂存本次物化的命令，恢复直接分派；Clear取消暂存|busy=false仍重新物化，保留再次发生的表达式副作用；不新增调用帧或opcode执行器|
|W3：消息消费|窗口持有因宿主资源等待而暂停的局部hook及待提交阶段；接通create/user/destroy/blit四个消费者|wait、busy、局部预算耗尽结束本次hook；下一次触发从新调用开始；字体、输入和窗口仍使用EasyRPG|
|W4：存读档及退出|实际读档在替换银行/图片前取消旧消息消费及前景解释器；Cleanup在重建对象前取消旧hook|沿用已有银行、帧、kind1/kind2及地图恢复；不新增LSD字段、hook栈存档或通用continuation codec|
|W5：检查及交付|完整汇总通过，移除临时检查和入口替身，重新配置/构建，代码与报告分别提交|不运行851矩阵，不逐opcode验收，不扩入其他旧行为包|

### D1：生成命令的物化与恢复

[CommandManiacCallCommand](../../../src/game_interpreter.cpp)在解释器内保存一个可选的已物化命令。只有分派成功并产生YieldRepeat才保留；恢复直接分派该命令，完成或busy失败便释放。解释器Clear同时取消3007 Operation及该物化结果。

因此，宿主暂停期间GLOBAL中的opcode、Text或参数表达式来源改变，不会改变这次尚未返回的分派，也不会再次递增变量或推进随机状态。busy重试仍遵守原合同。既有Update(false)、分派尾及预算承担续行，没有另造执行轮。

### D2：局部hook与窗口提交属于同一次消费

[RunManiacMessageHook](../../../src/game_interpreter.cpp)改为由当前窗口消费持有局部解释器。仅eYield/eYieldRepeat保留调用；其余停止释放解释器。恢复使用Update(false)，保留这次调用的预算，达到正常停止条件后仍丢弃局部后缀。

[Window_Message](../../../src/window_message.cpp)分别保留create初始化、user位置读回、destroy关闭、blit绘制的提交位置。资源等待期间不提前分配最终字形画布、不读回最终结果、不关闭窗口、不绘制或推进当前字形。完成后读取当前注册base并提交一次。E参数只解析一次；glyph消费保留本次token及现有Font/系统位图引用。

外层ShowMessage按已消费命令返回Yield，内层hook仍在自己的资源指令处暂停，避免重建消息。[Game_Map::Update](../../../src/game_map.cpp)先恢复该窗口消费，再回到原前景、并行或消息阶段；保持原async context，不重复此前的世界和窗口更新。文字内部恢复只继续UpdateMessage，不重跑整次Window_Message::Update。

消息替换、强制关闭、读档和Cleanup清除局部调用与提交。窗口generation只用于拒绝旧消息的提交，执行中的解释器由局部owner持有，避免替换消息时销毁正在调用Update的对象。它不是原版共享栈重入机制的实现，也没有新增“重入跳过”规则。

### D3：E根读取按输出类别启用

[UserHookParser](../../../src/game_message.cpp)继续解析完整结构及嵌套ID，根V按数字输出base控制，根T/N按字符串输出base控制。关闭hook或关闭该类别base时，不额外读取对应根对象。数字、字符串输出游标仍独立，末项赋值和present-empty规则保留。

### D4/D5：保存请求的两个独立事实

[AsyncOp](../../../src/async_op.h)保留result-enabled，与结果地址分离。invalid-slot、局部hook同步保存以及普通Scene_Map异步保存，都先完成保存/结果写回，再根据启用事实调用现有im/pf刷新接口。启用但地址0仍刷新，地址0仍不写变量；负地址继续使用保存请求捕获的caller SELF身份。

[Scene_Save](../../../src/scene_save.cpp)增加本次是否更新last-slot的入口参数，默认保持普通保存后端行为。3002调用仅对slot1..15更新last-slot，slot16+仍保存并增加一次计数。元数据在收集system快照前完成；文件仍包含结果写回前的值及普通保存指令后的游标。

Scene_Battle的保存消费者仅同步采用共享AsyncOp契约和last-slot参数。这是同一3002入口的必要连接，不代表本轮开发或验收了战斗worker、行动循环和目标效果。

## 2. 24项义务的关闭依据

“关闭”表示限定义务已接到当前实现并有下列依据。源码保护、既有检查、任务内消费者检查仍是不同层级；没有把每个分支都宣称为新的运行实测。

|义务|本轮结论与证据|
|---|---|
|C1-01 短记录/presence|保留原count门槛及缺字段分支；既有fresh fields、typed return、GLOBAL/SELF空串与presence检查通过|
|C1-02 参数/别名|既有CallEvent顺序写入、caller/callee银行及分类游标不变，相关既有用例通过|
|C1-03 返回接收者|既有正/负及间接返回检查保护；实际Player载入后子帧wait结束，向caller SELF负接收者交付77|
|C1-04 返回/清理|既有END、typed return及即时刷新检查通过；实际载入后的返回先交付，再继续caller后缀|
|C1-05 SELF身份|实际载入恢复common/caller帧；OnMapChange直接消费者保留map-definition SELF，清除的普通event_id不改变其银行身份|
|C2-01 物化规则|既有3019同帧、参数模式、SELF/Text及name-cache用例通过，未增加保存帧|
|C2-02 busy重试|既有busy重新求值及再次副作用用例通过；新增缓存仅涵盖YieldRepeat|
|C2-03 资源恢复|im/pf临时消费者更改opcode/Text来源并多次恢复；表达式只递增一次，旧文本挂到原图片一次|
|C2-04 游标/预算|资源pending时原3019游标未消费、loop_count为0；完成后marker一次并停于后续wait，im/pf均通过|
|C3-01 参数包/回读|核对四类packet的caller归属及局部common身份；临时E消费者检查独立SELF数字/字符串游标及hook更换base后的实时读回|
|C3-02 E根值门控|禁用根N/V、启用字符串类别中的嵌套N ID、present-empty及末项输出均通过|
|C3-03 正常停止|wait与局部预算消费者证明旧后缀不续跑、下次新调用；busy无async的释放分支源码核验，既有busy分派检查保护|
|C3-04 资源后提交|create尺寸/画布、user位置、blit像素/文字推进、destroy关闭四个真实窗口消费者分别通过；前景ShowMessage恢复不重建消息|
|C3-05 取消|消息覆盖及强制关闭取消旧Operation，迟到DownloadDone不提交旧窗口；实际load及新游戏进一步覆盖该边界|
|C4-01 结果启用/归属|invalid-slot及有效slot的地址0刷新/禁用分支通过，包含普通场景与hook消费者；既有负结果caller身份检查通过|
|C4-02 快照时点|实际Scene_Map保存文件含写回前结果99及current_command=2；运行时写回1，实际载入只执行保存后缀，计数不重复|
|C4-03 元数据|slot16文件与运行时保留last-slot=3、计数1；slot1更新为1；普通保存后端默认调用仍可将last-slot改为16|
|C4-04 执行状态选择|hook3002即时写回并继续局部后缀，文件保存等待中的普通前景栈，不保存hook栈；已有公共/地图并行收集骨架不变|
|C5-01 银行/帧恢复|实际Player载入替换旧SELF表、恢复嵌套返回与wait；旧额外地图银行消失；presence/私有字段由既有集合保护|
|C5-02 图片有效状态|实际载入的kind1解析后快照和200×30文字位图一致；kind2保留尺寸及alpha0的RGB(17,29,43)；不重新解释插入的转义|
|C5-03 新状态续行|地图请求pending时旧前景已清理且新帧未启动；资源完成才恢复；A3两路径不执行旧marker，保存的wait及后缀继续|
|C6-01 地图边界|OnMapChange直接消费者保留前景SELF身份；erase-on-map-change实际图片消费者取消文字请求；旧map并行对象退役沿现有Dispose路径核验|
|C6-02 Clear/标题/新游戏|Clear取消物化命令及文字操作；ResetGameObjects实际取消旧hook并重建空银行；返回标题按既有场景退出及窗口析构退役，未新建全局任务平台|
|C6-03 迟到/同号对象|实际load释放旧图片holder，旧hook迟到完成不能挂入新对象；换号及JS弱引用沿用P1已验证合同，不宣称本轮重验完整JS|

## 3. 检查结果与证据边界

完整计数、临时消费者名单、摘要哈希及最终构建信息见[结构化记录](bounded-execution-validation-20261015.json)。计数不表示Maniac兼容率。

|检查|结果|实际验证层级|
|---|---|---|
|既有非UI集合|504用例，3,677,971断言，失败0，完整XML汇总|保护既有执行、银行、返回、预算、保存字段及普通逻辑|
|任务内临时消费者|20用例，268断言，失败0，完整XML汇总|真实Window_Message、Game_Map/Scene_Map、Scene_Save、Player::LoadSavegame、ResetGameObjects及图片消费者|
|Windows Release x64构建|Player及runner成功；临时检查移除后重新配置/编译|通过既有ASCII构建目录及build-windows.ps1；没有从非ASCII仓库工作目录直接调用CMake|
|最终runner库存|504个既有用例，临时suite不存在|测试源码和入口恢复；没有永久新增或修改测试/期望|
|源码范围检查|diff无空白错误，无临时定位日志，未引入ManiacVersion/GDI及新存档schema|产品16个文件；字体选择/字形和输入实现保留|

临时入口提供无窗口BaseUi、文件输出确认及普通输入源初始化，没有创建本机窗口或接收人工输入。资源pending由任务内夹具控制，完成后驱动现有消费者；没有修改产品的网络/资源宿主。存档和LMU来自任务内夹具，未写入用户游戏目录。

清理分两层确认：源码恢复后检查原入口SHA-256与备份相同、tests没有diff；重新构建后检查库存504且二进制不含临时suite或BoundedHeadless标记。Copy-Item恢复会保留旧文件时间戳，首次收尾构建因此复用了临时入口对象文件；本轮通过更新时间戳触发原入口重编译，再核对二进制。后续同类临时检查恢复也须执行这一步，不能仅凭源码已恢复或库存正确判断入口替身已移除。

保存/载入检查确实走文件、Player状态替换、地图请求、SetupFromSave和StartFromSave，不仅是直接codec往返。但它们仍是**同一进程的Windows非UI消费者检查**，不是重新启动进程后的游戏验收、原版双向存档互通或Web/Android持久化验收。

### 检查过程中修正的夹具问题

首次执行缺少位图格式初始化；演员数据库加入后未重建运行时演员，且缺少Actor::Setup；消息入口未设置转义字符；普通入场过渡未解除；嵌套调用夹具未注册地图公共事件对象；实际地图载入要求Input::IsRecording已有输入源，而临时入口未初始化。按实际宿主前置条件补齐后，所有消费者得到完整汇总。曾添加的读档定位日志已移除，没有据此更改产品输入逻辑。

保存夹具末尾显式加入END，与liblcf自动写入四字节终止符重复，产生chunk长度告警；实际保存场景改用正常终止的事件列表，未修改liblcf。早期夹具编译错误涉及DBString/DBArray接口及doctest布尔表达式写法，仅修正临时代码。

最后一个失败是错误的检查标准：私有快照会转义字面反斜杠，恢复后的canonical内部表示可能与原解析表示不同。按Parse与SaveSnapshot源码确认这是表示差异后，改为同时比较解析后token快照、尺寸和完整实际位图。没有静音告警、放宽产品结果或要求native/LSD字节相等。

## 4. 原版依据与未建设的部分

本轮复用范围报告保留的证据：[消息局部runner](runtime-modes/evidence/0x1401bbfb0-pseudocode.txt)、[保存入口](evidence/0x140024f10-pseudocode.txt)、[根保存的固定前景状态选择](evidence/0x140081c90-pseudocode.txt)，以及既有3029/3002合同。没有重新固定版本身份或扩大原版审计对象。

没有新增hook栈存档。原版根0x71选择固定前景状态，不能因为hook内保存就把局部后缀持久化。没有预先序列化pending Operation：重要资源pending时，正常场景暂停普通事件，不能把C++直接调用保存视为所有脚本可达状态。

以下继续具名保留，不纳入本轮结项：MSG-REENTRY-FLOW的原版共享栈重入；尚未证明可达的资源pending重入存档；战斗3009/3010/3037及旧G1–G4；额外Map/MoveRoute、Pack、SaveInfo、媒体、CRT/正则/完整JS；原版错误崩溃方式、LSD字节等价、全量版本及平台/UI等价。本轮少量Scene_Battle保存连接不关闭这些战斗问题。

## 5. 最终状态

W1–W5完成，24项限定义务按上述证据关闭，代码与报告分别提交，临时测试、入口替身及产品定位日志移除。当前有限工作包没有剩余开发项；实际弹珠游戏、原版对照及跨平台验收仍未执行，不能从本轮通过数推定其通过。

后续若出现新问题，先定位它属于哪条已交付消费链、哪项结果义务，或是否为新的行为包。现有未知项不会自动扩大本轮范围，也不恢复旧的全量移植工程。
