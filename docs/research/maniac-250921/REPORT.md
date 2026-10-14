# Maniac 250921 原版机理与 Player 行为兼容补完报告

日期：2026-10-14。分析对象：`patch_selfvar_250921/English/RPG_RT.exe`。本轮只读取原版、当前源码和参考工作区，报告及研究辅助材料位于 `build/`；未修改 Player、Kai 或 liblcf 源码。

后续修订：`0d7d3301b` 对 MovePicture relative 的“只改位置”判断已被完整加法分支与汇编推翻；本报告 D11、§10.1.1 同步更正。新增时长生产者/更新消费者缺口、弹珠游戏触发链与系统性补完规模见 [消费链复核补充](consumer-contract-followup.md)。补充的分析基点为 `5c64d306c`；2026-10-15 有限修复提交 `dcdc3c3ca` 已完成 D11/D14 的代码修正与非 UI 检查，交付边界见补充 §7。差异表保留分析发现，不是当前所有未修复项的状态清单。

## 1. 结论与兼容边界

2026-10-15：后续筛选与实现采用用户确认的 [固定行动准则](ACTION-GUIDELINES.md)。旧代码已按这些准则提取三个有限行为包；当前交付与剩余工作评估见 [迁移记录](legacy-migration-20261015.md)。本报告历史差异表和原版内部规模不作为未修复总量或新的完整移植门槛。

**补完的主要工作应从“实现了哪些指令”转向“共享状态和运算规则是否一致”。** 当前项目已经实现了大量指令，也保留了一些原版很反直觉的细节；最明确的剩余差异集中在图片的解析、状态、像素绘制和恢复链。继续按游戏逐个打补丁，会反复遇到这些共享原因的不同表现。

本报告采用用户指定的目标：让游戏可观察的运算、事件顺序、资源状态、图片和存档尽可能一致，保留 EasyRPG 的字体选择、字形生成、输入与接收逻辑。原版的 Win32、GDI、DirectSound、线程池、堆分配和错误窗口不作为整体移植对象。

|对象|一致性目标|保留 EasyRPG 的部分|
|---|---|---|
|数值|整数位模式、求值顺序、浮点步骤、截断/舍入、随机状态一致|内存分配和跨平台实现方式|
|图片|源像素解释、取样、边界、色调、闪光、混合、逐帧状态一致|屏幕呈现、窗口、GPU/软件表面管理|
|文字图片|转义和配置游标、创建时快照、间距、图标、边框、颜色与合成规则一致|字体发现、选择、回退、实际字形及其度量由现有字体接口提供|
|事件/SELF|接收者、调用者、变量银行、命令指针、等待、写回时刻一致|解释器内部类型和容器|
|保存/读取|恢复后的游戏状态与继续执行一致，原版可识别字段正确|EasyRPG 扩展字段、文件系统与平台持久化实现|
|输入/媒体|命令与事件框架能正确接收现有 EasyRPG 返回值|键盘、鼠标、触屏、IME、控制器、焦点、文本输入及音视频后端|

这里的文字像素一致性是有边界的：**在给定 EasyRPG 字形掩码和字形 advance 的条件下，布局和合成遵循原版规则。** 不要求不同字体或不同字形光栅器产生相同的字形宽度、笔画和抗锯齿。这是用户明确保留的平台行为，不能据此不断扩大到 GDI 字体移植。

有确定结果、会影响正常游戏逻辑的原版怪异行为属于兼容对象。例如表达式范围广播、数组 MAX 的尾项规则、停止图片特效的状态切换。读越界内存、无效指针、堆损坏、资源耗尽及不可确定的崩溃结果不构成需要复制的游戏语义；合法的短指令、未知位、动态生成指令则不能一概当作垃圾输入丢弃。

## 2. 证据和本轮完成范围

### 2.1 固定原版

|制品|SHA-256|作用|
|---|---|---|
|English/RPG_RT.exe|`082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f`|x86-64 原版执行消费者，主要依据|
|English/cmdTos.dll|`e37282275682a7abfaeaedc8174212d5166e84e94eed74325d4570ce8dd95856`|原生显示/格式化辅助依据|
|Common/cmdcs.dll|`09e87127eeaaa71906de611d0e8f748cccd1e0eb5b3437fb164ccbd4ab51ec07`|托管编辑器的字段与生产者辅助依据|

原版路径：RPG_RT.exe（外部审计制品）。原始语料：REA 语料目录（外部审计制品）。

已有语料包括 168 条指令的静态路由、4,026 个运行时函数记录、1,954 个格式化器函数和 4,604 个经摘要校验的托管 IL 切片。它们是调查材料，**这些数量不表示所有分支都完成了语义审核，也不表示游戏行为通过验收。** 托管签名、间接调用、未被反编译器覆盖的代码仍有边界。

本轮重新校验实际读取的原版 Evidence 记录与目标摘要，并核对当前代码。旧 `Maniacs-Complete/evidence/*.md` 用于定位和对照；关键结论以原版反编译、汇编和字节为准，旧文件里的“complete”不作为兼容关闭证据。特别是反编译器的浮点参数、返回类型和间接跳转不能直接当作 C++ 源码。

补采了两个原版绘制跳转表，并从固定 PE 字节线性解码旋转、波形的 32 个分支代码区间，弥补 Ghidra 伪代码在大跳转表处结束的缺口。见 [绘制分支清单](draw-branch-manifest.json)。该材料是静态解码，不是运行观察。

### 2.2 审计对象与证据级别

本报告覆盖前面确定的六个审计对象：原始指令与参数，GLOBAL/SELF 寻址，数值与字符串计算，事件调度与状态更新，图片/消息/存档，平台适配边界。

正文中“确认”指本轮对固定原版与当前源码的静态确认；“待对照”指已有实现但尚不足以判断最终行为；“待验收”指需要执行消费者才能确认。没有启动原版游戏，没有人工 UI 或浏览器交互，没有新增项目测试，没有子代理，也没有重新编译项目。附录列出具体未关闭的问题，不能把这份研究报告当作三平台兼容完成证明。

完整 Evidence ID、地址、文件、摘要和本轮读取范围在 [读取清单](read-records.json)。后文使用 RVA，例如 `06d8c0`；对应 VA 为 `0x14006d8c0`，只适用于上述摘要的二进制。

## 3. 审计对象一：指令、参数与生产者

### 3.1 外层记录与运行时对象是两种布局

外层事件记录是 BER opcode、indent、Text 字节长度与字节内容、参数个数及 BER 参数。BER 每组 7 位，高组在前；负参数保留 uint32 位模式，不能改成 ZigZag。

原版 `056e10` 将其加载为 0x28 字节的命令对象：

|偏移|内容|
|---|---|
|+0x00|opcode，32 位|
|+0x04|indent，32 位|
|+0x08|运行时缓存，按消费者使用；不是额外序列化参数|
|+0x10|引擎 UTF-16 字符串指针，可为 NULL|
|+0x18|已解码的 int32 参数数组|
|+0x20|64 位逻辑参数个数，加载值为 `int64(max(int32(decoded_count),0))`|

复制器按低 32 位计数复制，再写回 64 位计数。不能根据分析者标注的结构类型推断写入宽度。容量、尾部初始化字节与逻辑个数要分开。

### 3.2 缺项返回 0，并不意味着“参数不存在”等于“显式参数 0”

多数普通取值在缺项时返回 0，但消费者会先判断实际个数，再决定是否执行某段逻辑。例：原版 `12310` 的返回传递需要至少两项；`13260` 读取某个已有 A3 的实际门槛是 count>=5。另一类差异是创建默认值不等于短记录缺省：`10120` 的新建默认 `[0,2,1]` 不能拿来补齐旧记录。

当前 [CmdSetup](../../../src/game_interpreter_shared.h#L211) 会把短记录拷贝并补到 MIN_SIZE。这个模式只有在消费者不依赖原始个数时才等价。不能把所有 CmdSetup 都判为缺陷，也不能用“只发生于畸形记录”的注释免除检查：原版编辑器自身就会生成短于后期消费者能力的记录，3019 也能生成短指令。

建议逐个消费者保留原始 `parameters.size()`；缺项值用局部 `arg(i)` 返回 0。没有必要引入新全局格式或重建所有命令类型。原始个数、Text 和未知尾项应继续由 liblcf 保存。

### 3.3 Text 不存在一种统一语法

|消费者|Text 的实际角色|
|---|---|
|3007|正文、系统图、字体配置的游标协议，配置中另含 5-bit 引用|
|12330、3036|无分隔符拼接的名字与字面量；参数记录切片预算|
|3030/3031|JavaScript 源码及 CRLF 续行|
|11330/3027|嵌套移动路线的资源字符串|
|3020|随操作变体变化的字符串输入，不能用显示器的标签推断写入协议|
|普通消息|运行时转义、混合参数、钩子 token|

生产者、格式化器和运行消费者必须分开。例如 3007 的旧原生 writer 只写 21 项，角度模式写 22 项；运行消费者还能读 A22、A23。显示工具支持某个字段，不证明运行时会执行它。

原版 `3022` 是最重要的反例：这份二进制里，该指令本身不求值表达式；表达式实际由 `10220`、`12010` 类型 16 和 `3019` 模式 4 使用。`3035` 虽有 DrawShape 标签，该原版路由却是跳过。以后不能以名称或“功能更多”判断兼容完成。

## 4. 审计对象二：GLOBAL / SELF / 字符串存在性

### 4.1 银行不是调用局部变量

原版保存三类全局向量，以及两层 SELF 存储：公共事件按 ID；地图事件按 map ID + definition event ID。每个 SELF 对象有 switches、numbers、strings 三个向量，分别位于 +0、+8、+16。

普通负 ID 从当前栈顶 frame 查找 SELF；`17d680` 优先用 frame+0x10 的缓存，否则按 frame 类型及地图/事件键查找并缓存。读数值 `17d7c0`、读字符串 `08cac0` 在缺失环境、0 或越界时分别给 0/NULL。读路径一般不因为读取本身创建完整银行；可写访问、连续范围和显式 SELF 指令有各自的创建规则。

SELF 持久于事件调用之间。调用相同公共事件不创建一份全新的局部变量。克隆地图事件的运行身份和其 definition/self 身份可能不同，不能只存一个“当前 event ID”混用。

### 4.2 符号、间接级数与物理连续槽位

普通 `V(id)` 的负 ID 表示 SELF。常见间接规则为：先读一次；若最初引用为负，再将中间结果强制为负绝对值，然后按所得 ID 再读。最初引用为正而得到负数也可以转入 SELF。

但规则不统一：

|路径|特有规则|
|---|---|
|表达式读 token16/17|先强制为 `-abs32`，SELF 的中间结果继续强制 SELF|
|表达式左值 selector16/17|先做 `neg32(raw)`；原始负数可以先转入全局，再由中间结果决定目标|
|3019 参数源模式1|只用 V(A2) 作为数组基址，不因 A2 原本为负而强制第二层 SELF|
|3036 Set 的显式环境|数值源可以在选定银行取值，ID 模式2却依赖当前银行；不能全都绑定源环境|
|3036 Clear/Init 模式5|一次读取，并按原引用的负号强制负绝对；与 Set 的解析不同|
|12310 负接收地址|显式取上一层 frame 的 SELF，最终用该银行的绝对槽位；不等价于普通有符号 setter|

连续 SELF 基址 -10 的物理序列是 -10、-11、-12。负号选择银行，不意味着物理索引倒序。数组复制、钩子参数包、聚合、返回值都必须遵守这一点。

### 4.3 slot0、银行长度与空对象可观察

普通读取 ID0 返回 0/NULL；指针访问、compound switch、数组或显式银行操作可访问物理 slot0。不能拿普通读取的无效 ID0 规则去替代所有路径。

原版连续范围会扩展源银行，也会扩展目标银行；新增槽位置零/NULL。分配顺序影响后续间接读取和别名。长度不等于容量，Clear、Init、Free 对长度、对象和缓冲区的作用不同。避免在修改结果看似相同的情况下改变这些副作用。

### 4.4 字符串 NULL 与非 NULL 空串

原版字符串既有带显式长度的存储，也有 `lstrlenW` 消费者。至少必须表达：

1. 不存在/NULL；
2. 已存在、长度 0；
3. 非空且可以包含 NUL 的 UTF-16 单元序列。

比较、配置回退、名称解析和保存时不一定把它们等同。`035360` 的系统图/字体回退检查的是 NULL，不能一律写成 `.empty()`。文字正文的创建解析用 `lstrlenW`，因此内嵌 NUL 在该阶段截断；银行本身的长度不会因此消失。

当前 [ReadString/IsManiacStringPresent](../../../src/game_interpreter.cpp#L642) 已有取值与存在性分开处理的基础；[字符串存储](../../../src/maniac_string_storage.h) 和 SELF 状态也有相应表示。需要修的是忽略存在性、上下文或 UTF-16 单元的消费者，不能宣称整个项目尚无此能力。

### 4.5 调用与参数赋值的顺序

`12330` 构造 frame、选择命令列表、顺序赋实参，最后才压栈。三类参数各自从槽1计数，number/string/number 对应 numeric1/string1/numeric2。

实参求值仍在调用者上下文。如果调用者与被调用者指向同一持久银行，较早的赋值可以影响较后的引用。先把全部实参求值成一个 snapshot 再写入，会改变递归和同事件调用。

接收地址是延迟协议：`12330` 保存原始 A5 和接收类型；`12310` 才解析接收者并交付结果。正间接地址与负 caller SELF 地址的解析时机不同。交付发生在 callee 清理和退出前，普通 opcode0 退出不自动制造返回值。

3036 名称缓存还存在确定的原版怪异行为：两个字面量环境名共享同一 command+8；命中缓存时不推进名称游标。12330 命中名称缓存仍推进自己的名字切片。不能用一个“名称缓存 helper”擅自统一二者。

## 5. 审计对象三：数值、表达式、数组与字符串

### 5.1 数值要定义四种不同语义

|语义|原版规则|
|---|---|
|原样赋值|保留 int32 位模式，不自动套实时上下限|
|加减乘除余|有符号 int64 中间值，随后限制到实时 min/max；0 除数改为1|
|位运算/位移|uint32 位模式；左移和逻辑右移，计数 `&31`|
|浮点转整数|先按具体调用点执行 double 运算与可能的 bounds/trunc，再执行转换；不能统一为 round/clamp|

原版 `037bd0` 和表达式 mutation 明确分开这些规则。实时上下限在 RVA `257f34`/`257f30`，是每次读取的状态，初始 INT_MIN/INT_MAX 不能被当成永久常量。`abs32(INT_MIN)`、`neg32(INT_MIN)` 保持 32 位回绕，不应让 C++ 有符号溢出成为跨平台未定义行为。

`CVTTSD2SI` 在通常的 masked invalid 环境下，对 NaN 和不可表示值返回 INT_MIN；这不是饱和转换。原版浮点 bounds 使用 COMISD 的标志，NaN 的 unordered 分支不能从反编译器的 C 比较直接推断。本轮核对了 `058a74/058a8c` 及 `038fb4/038fcc`：JNC 不会把 unordered 截在 bounds 分支，随后仍走转换。若外部代码改变浮点异常屏蔽，异常行为另有环境边界。

### 5.2 数值函数逐项规则

记 `I(x)` 为按原版转换序列得到 int32；`B` 为实时 bounds。以下不是“数学上更好的等价式”，操作顺序也属于合同。

|函数|250921 的实际步骤|
|---|---|
|Pow|原版 double pow → trunc → 与实时上下限比较 → 整数转换|
|Sqrt|sqrt(abs32(a))*scale；对未截断 double 比较上下限，然后截断转换，再按 a 原符号做 32 位取负|
|Sin/Cos|`double(a)/(b==0?1:b)` 转角，再调用原版三角 helper，再乘 scale；截断转换，无显式实时 bounds|
|Atan2|第一个实参为 y，第二个为 x；乘原版常数 `57.2957795131` 再乘 scale，截断；不能换成 180/pi 后假定所有结果相同|
|Min/Max|有符号 int32 比较|
|Clamp|先 max(value,lower) 后 min(result,upper)，不排序上下限|
|MulDiv|int64(a*b)/(c==0?1:c)，随后实时 bounds|
|DivMul|double(a)/(b==0?1:b)*c，比较实时 bounds，再截断转换|
|Between|闭区间、原始顺序上下界；不排序|
|Lerp|`a+(b-a)*(c/(d==0?1:d))` 的 double 步骤，然后 trunc/I，无实时 bounds|
|Sum|count=max(raw,0)，准备原版范围；uint32 回绕求和|
|Min 序列|count=max(raw,1)，初始化 INT_MAX；寻址与普通数值取值不同|
|10220 Max 序列|count=max(raw,1)，但实际读取 count-1 项；count1 的结果是 INT_MIN|

以上各函数的参数模式与前面的地址规则同样重要。例如 10220 的 SUM 基址和 MIN/MAX 基址解析不同；Sqrt 的第二模式不是所有情况下都按常规 bitfield 规则处理。不能只让数学 helper 正确而忽略入口解析。

当前 [数值 helper](../../../src/game_interpreter_control_variables.cpp#L404)、[聚合入口](../../../src/game_interpreter.cpp#L1341) 和 [表达式](../../../src/maniac_patch.cpp#L322) 已保留大量这些细节，包括 MAX 少读一项、broadcast、数值 switch copy 和 LCG。它们应作为需要保护的既有实现。

### 5.3 浮点内核仍须设立明确的一致性门槛

`1f8220` 的 pow 是原版携带的数值内核，有按能力状态分派的 AVX/SSE 路径、指数/尾数分解、表格与多项式计算以及特殊值分支。不同系统的 `std::pow/std::sin/std::cos/std::atan2` 不保证逐 bit 一致；一个很小的误差经过截断可能让整数差1，足以改变分歧和坐标。

不能现在声称当前 std::math 已完全等价；也不应为了这个问题移植整套 CRT。补完应先把游戏可观察的常数、步骤、bounds、转换和 FP contraction 固定，再对原版内核的整数输出边界做差分。必要时只实现发生差异的被调用内核或特定路径，保留输入输出合同。不要把整数幂先改成“精确大整数幂”：原版可能在 double 阶段先出现近似和溢出。

编译验收要包括 Windows/Web/Android 的这些结果，禁止用 fast-math 或随意 FMA 合并来改变原版拆开的乘加。报告没有将原版所有 CPU 内核证明为全输入域 bit-equivalent，也没有运行原版数值探针；这是补完关闭前仍需核验的明确项。

### 5.4 同一随机状态和推进次数

原版公共状态 `269828`：

```text
s = (s * 0x08088405 + 1) mod 2^32
lo = min(a,b); hi = max(a,b)
r = lo + high32(uint64(s) * (int64(hi)-lo+1))
```

随机函数、10220 范围求值、3013 shuffle 必须共用这一状态。区间两端包含；不能换成 `% range`、另一个 PRNG 或每事件独立种子。

shuffle 从 remaining-1 向下处理，配对 shuffle 用同一个 j 同时交换两列。最后的单元素步骤也推进状态。结果数组看起来一样，不代表后面的随机数相同。当前 Main_Data 中的 Maniac PRNG 已实现这条LCG，不需要另外发明新的随机算法。

**但随机源的消费者尚未统一。** 原版 `061b20` 更新地图事件时也从同一 `269828` 推进LCG，例如设置随机停步时间；移动、战斗等多处原版消费者同样引用这个状态。当前 [Main_Data::NextManiacRandom](../../../src/main_data.cpp#L68) 与 [Rand::GetRandomNumber](../../../src/rand.cpp#L59) 使用不同状态及区间算法，[Game_Event](../../../src/game_event.cpp) 仍使用通用Rand。这是确认的共享状态差异：一个地图事件可以改变原版后继变量随机结果，却不推进Player的Maniac seed。

补完需列出所有影响游戏状态的原版随机消费者，按其原始转换和推进次数接到同一个Maniac状态；不能简单将全部调用替换为同一种inclusive helper，因为有些原版消费者直接使用高位或不同区间映射。纯平台呈现的随机效果是否影响后继游戏随机也要从原版引用证明，不能随意加入消耗。普通非Maniac的Rand行为继续保留。

初始seed来源已确认：`059680` 用 `GetSystemTime` 的UTC时分秒毫秒组合成一天内毫秒数。相同算法不要求两台机器不同启动时刻得到相同种子；对照时应先固定seed和事件调度，再比较消费序列。保存是否持久化这个seed仍需按实际序列化器确认，不能臆造原版持久化保证。

### 5.5 表达式字节流及返回 pair

求值器 `057500` 从 **已解码 int32 Args 的 little-endian 内存字节**读取；它不是 BER，也不是 Text。游标是字节偏移，表达式可跨 int32 字边界。

|token|行为|
|---|---|
|0|0|
|1/4、2/5、3/6|无符号8、无符号16、有符号32字面量；窄负数需外部 neg 节点|
|8/9、11/12|普通 numeric/switch、强制 SELF numeric/switch|
|13/14、16/17|普通/SELF 风格的间接读取，符号规则有区别|
|19|读取 count，逐个求值所有子节点产生副作用，结果(0,0)|
|20/21、22/23、24/25/26|abs、负绝对、取负/逻辑非/按位非|
|34..44|赋值及10种 compound 操作，含专用 range/list 路径|
|48..57|算术、位运算、位移|
|58..63|有符号比较，返回0/1|
|64/65|OR/AND 短路，未选部分结构跳过|
|66|返回 pair(first.primary, second.primary)|
|67|先求列表中所有 pair，再求索引；负索引经无符号检查失败，结果(0,0)|
|72|条件/then/else；只求选中的 arm，另一 arm 结构跳过|
|78|函数 selector，固定 arity 分派；编码 count 不决定执行 arity|

primary 在 EAX，secondary 在共享状态 `269620`。两者不是始终相等，地址范围依赖 pair。求值可写数值/开关并推进 PRNG，不能预先折叠为无副作用 AST。

token19 的 count marker<128 是单字节；否则读随后的 signed32 count。token78 的求值路径遇到高位 marker 会多跳4字节；其结构 skip 路径只跳 marker1字节，再按 selector arity 递归。这是原版的游标不对称，不要把两个解析器“整理”为相同函数。

入口边界各自不同：10220 有表达式长度头；3019 从 byte20 到实际 count*4；12010 类型16从 byte24 开始，原版声明的 end 又多6词。后者导致超边界读取的部分不需要复制为内存漏洞，但合法表达式的起点不能因此被改错。

### 5.6 assignment 不能向量化成统一规则

直接左值先求地址 pair、取绝对端点、按有符号排序。单地址路径先求 RHS，再取得写指针。间接左值在求 RHS **之后**读取最终目的 ID，RHS 的写入可以改变地址。

|路径|原版行为|
|---|---|
|数值 scalar broadcast|RHS 只求一次；循环 count 次修改同一首指针；银行符号由 peek 的 RHS token 决定|
|开关 scalar broadcast|遍历 low<=i<high，漏掉最后端点；银行符号同样由 RHS token 决定|
|list19 RHS|逐子项求值后写入，剩余目标执行“同一个操作 RHS0”；额外子项仍全部求值产生副作用|
|optimized numeric range RHS|边读边写、正向物理索引；重叠会受先前写入影响|
|optimized switch range RHS|源读的是 numeric bank，不是 switch bank，再交给 switch mutation|

这些确定行为应保留。`switch XOR` 的原版运算是 `oldByte != rhsInt`，先把 rhs 规范成 bool 后 XOR 会改变 raw byte 输入的结果。赋值 setter 与 compound 指针在 slot0 上也不相同。

### 5.7 3013 数组、别名与排序

复制类使用原版 bulk move 的重叠语义；swap、算术、dereference 是即时、正向循环，不能统一成 snapshot copy。源范围也可扩容。配对数组完全重合时，两次 swap 可能互相撤销。

原版配对 sort 使用中间 pivot 的非稳定 quicksort；相等 key 的伴随 value 顺序可观察。普通 sort 只比较最终单列有序值；配对 sort 需要比较 permutation，不能用“都不是 stable_sort”证明不同算法相同。

enum 采用 uint32 回绕。dereference、reverse 与其他操作的地图 refresh 标志也不同。将所有操作无条件标记刷新可能改变并行事件可见时刻。

### 5.8 字符串计算的共同合同

字符串操作要以原版 UTF-16 单元位置、明确的 NULL/present 状态、NUL 消费方式和具体操作的替换顺序为依据。Unicode scalar、UTF-8 字节和 UTF-16 单元不是可交换的计数单位。

数值转字符串、字符串转数值、比较、insert/replace、regex、文件输出的入口分别消费不同标志与变量地址；Regex 的库规则不能由编辑器标签推断。当前已有字符串实现和保存辅助字段，应按原版消费者复核各操作，避免全局替换字符串类型。

本轮对共享存在性、SELF、3007 的字符串生成和存档生命周期深入核对；3020 的全部 regex/library 分支不在本轮逐分支重审覆盖内。其既有契约和源码是继续补完的输入，不能把这一段概括为“全部字符串操作已证明一致”。

## 6. 审计对象四：调度、重试、消息与战斗状态

### 6.1 dispatcher 先推进再执行

`045660` 在 dispatch 前推进 frame cursor；busy retry 分支 `046eed` 将其减1并返回 false。其他路径可能立即成功、在同一 invocation 继续取下一条、推 frame、退出 frame 或发起异步 scene。

因此“handler 返回 true/false”只有结合 cursor 和 owner 状态才有意义。实现中必须观察当前 frame、栈顶变化、wait/message 状态以及下一次 dispatch 的位置。不能只比较最后变量值。

### 6.2 3019 动态指令在同一 invocation 重新分派

构造顺序：复制 indent → 解析 opcode → 如 opcode=3019 则直接跳过 → 捕获 Text 引用 → 构造 Args → 切到共享 synthetic command → 在同一 frame/owner 重新计算分派键。

|参数源模式|机理|
|---|---|
|数组|先按 signed base/count 准备 numeric range，再 materialize 到共享命令缓冲；base0或count<1逻辑 count0，但保留一个 storage sentinel|
|inline3|从 A5+i取值，每4项只用一个 mode-word 的低4个 nibble；缺项0，模式区可与数值区重叠|
|expression4|从 byte20 开始反复求值到 end，每次 primary追加；结果0也不是停止标志，末尾零字节可能继续产生0参数|

没有新事件 frame，也没有独立的“调用完成等待”。生成命令的 busy retry 回退真实 frame 的 cursor，因此 3019 下一次会重新生成参数，表达式副作用可以重复。不能为了 exactly-once 给它增加隐藏事务。

### 6.3 等待、刷新与异步资源必须分清

原版某些 hook 在专用栈切换下运行，然后恢复调用者栈再读回参数。专用栈仍会受预算、wait、message-busy、退出条件和 dispatcher false 限制，不能承诺一个 hook 中所有命令同步执行完。

图片创建本身没有消息 busy 重试，但 EasyRPG 资源加载可能异步。异步完成时要消费此前已解析的图片状态；不能重新从最新变量取 Text 或把 SELF 上下文换成现在的栈顶。

3002 的原版顺序：slot校验 → last-slot/保存计数更新 → 当前状态序列化 → 写文件正常返回 → 结果变量赋值/refresh。结果变量的新值在这次快照之后才出现。EasyRPG 普通保存通过 update loop 完成，hook 内有特殊同步保存路径；需要保持该观察顺序，不能把“快照前标记成功”当成优化。

### 6.4 消息 hook 的最小必要状态

3029 低字节 mask 的1/2/4/8对应 user/create/destroy/blit。四个数值包不同，不能把它们合成同一通用包：

|hook|关键输出/回读|
|---|---|
|user|窗口、头像、行和当前文字坐标；只回读部分窗口/头像坐标|
|create|窗口X/Y/W/H、头像、索引、transition；W/H回读限制1..2000；某些fallback依赖已经更新的X/Y|
|destroy|读取关闭模式，其他坐标不回读|
|blit|字符坐标和两种颜色；读回坐标限制到当前窗口W/H|

用户 E 参数包的 numeric/string 有独立游标，最后一个 token 可在“无后续分隔符”的 false返回前已经写入。SELF 使用 caller，而不是执行钩子的公共事件银行。

这些属于游戏脚本逻辑，不需要改 EasyRPG 输入、确认按键或文字录入逻辑。消息文字字体保留现有接口，hook 的语义和行位置消费则需按原版。

### 6.5 战斗不能以 handler 数量关闭

3009、3010、3037 已有源码接入；完整补完还要核对它们的状态消费者：battle hook 的调用/恢复与等待，ATB 的累计与 action 队列，替换 enemy 的运行 ordinal、definition ID、方向、目标和生命周期。

原版 actor/enemy ATB 数值读取以 raw gauge 上限300000后除3000；还需保证这个数值与实际消费者使用的状态相同。5003 XOR bit0 的状态不是总暂停开关，非0只参与指定的 gauge 累计门槛。

本轮没有重新运行以前暂停的战斗消费者，亦没有从旧日志把某个 worktree 的验收状态套到当前树。战斗的完整关闭仍应按原版 action/target/phase 消费链逐项完成，保留 EasyRPG 接受用户战斗操作的接口。

## 7. 审计对象五：图片的完整状态与像素链

### 7.1 原版图片状态不可只用整数 effect_power 表示

|原版偏移|状态|语义|
|---|---|---|
|+0x10/+0x18|当前X/Y，double|与base和target分开|
|+0x20/+0x28|当前宽/高百分比，double|独立缩放轴|
|+0x30|当前透明度，double|不是已量化alpha|
|+0x38..+0x50|当前RGBA色调，double|绘制准备时再舍入|
|+0x58|当前旋转/波强度，或固定角的double target|随effect mode改变含义|
|+0x60/+0x68|目标X/Y，double|相对移动与地图滚动涉及三套坐标|
|+0x70|当前angle，double|固定角以degree，rotate draw按256周期换算|
|+0x98|图片名/文字图片已解析描述|文字图片保存快照来源|
|+0xa0|滚动、文字样式、图片kind合并word|高byte区分类型|
|+0xa4|透明及文字间距word|不只是bool|
|+0xa8|整个effect mode|切换表和update判断并不采用相同掩码|
|+0xb8|blend/flip/文字字号word|文字图片不可按 bottom transparency插值|
|+0xcc|原始目标强度/数值，int32|不能替代double角度target|
|+0xd0|remaining，int32|0再减为-1与负数的行为不同|
|+0xd4|wave phase，int32|每次update+8|
|+0xd8..+0x100|sheet、frame、delay、finish、start/end|逐帧状态|
|+0x10c|地图/战斗生命周期、screen tone/flash/shake flags|保存与绘制共享|
|+0x114..+0x117|dirty、已准备状态|缓存行为，不需要按内存布局原样移植|

### 7.2 Show / Move 的字段与状态切换

Show 立即替换 current/target 坐标、缩放、透明度、色调；重置 remaining、wave phase、frame timer。普通 rotate/wave 保留原始强度，并令 angle0。固定角：

```text
q = double(resolvedNumerator)/(resolvedDenominator==0?1:resolvedDenominator)
a = fmod(q,360)
if q<0: a += 360
```

注意判负的是 q，不是 remainder；负整倍数可以得到360。Show将 a 写double +58/+70，原始 numerator另存int +cc。

Move 使用旧/新低2bit的16项切换表，但某些分支保存整个新mode。更新又按完整mode判断0/1/2/3，不能丢弃高位或把未知mode强行归normal。

|旧→新|Normal|Rotate|Wave|Angle|
|---|---|---|---|---|
|Normal|不改|写mode/current power/raw target|同前|设置double角度target|
|Rotate|只把mode设0|只改raw target|写mode/current power/raw target|设置double角度target|
|Wave|只把raw target设0，保留Wave|重建强度状态|只改raw target|设置double角度target|
|Angle|只把double target设0，保留Angle|重建强度状态|重建强度状态|设置double角度target|

KeepEffect跳过整段。Angle target分支在 abs(new-current)>180 时，实际比较 `currentAngle < difference`，据此给current加/减360，再写new target。这不是通常的最短方向判断，必须保留有确定结果的原版规则。

当前 [Show/Move解析](../../../src/game_interpreter.cpp#L3429) 和 [3007](../../../src/game_interpreter.cpp#L6273) 仍将 effect_power 的整数除法用于固定角；原版有分开的double target和raw integer。`1/2` degree会在这里先丢失。只把字段改成double还不够，还要修改Move状态切换、逐帧update及保存恢复。

### 7.3 逐帧更新的顺序

`06d8c0` 先处理到期sheet frame，再处理remaining和普通插值，然后处理特效，最后 frame timer++。

若 remaining=n>0：

```text
w = 1.0/n
current += (target-current)*w
remaining = n-1
```

普通插值包含X/Y、两轴缩放、透明度和四个色调。n=0不做普通插值，仍减到-1。n<0不减普通remaining；特效系数使用1。

|mode|update|
|---|---|
|Rotate1|power向raw target插值，然后angle += power|
|Wave2|phase+=8，power向raw target插值|
|Angle3|angle向double target插值；不是先把power向raw integer插值再赋给angle|
|未知mode|不套三种已知effect update|

**停止旋转的Normal余波需要独立保留。** 本轮汇编核对 `06db27..06db5b` 的写入确实是 +0x58，而不是 +0x70：在其门槛满足时，把 `oldAngle-trunc(oldAngle/256)*256+oldPower` 写回power；后面用oldAngle和remaining判断是否清零angle。当前代码通过改current_rotation计算余波，不是相同状态过程。这是伪代码/既有直觉解释容易掩盖的差异，见读取清单中 `06d8c0` assembly。

文字图片的 +b8 是word，原版update没有对其执行底部透明度插值。当前 [Picture::Update](../../../src/game_pictures.cpp#L694) 对 current_bot_trans 的一般处理不能用于文字类型；要按kind解释字段。

### 7.4 sheet、原点、生命周期

sheet列/行直接决定 `frameW=sourceW/cols`、`frameH=sourceH/rows`。静态frame可以无效；绘制prepare用 `uint32(frame)>=uint32(frameCount)` 拒绝绘制，不把frame钳到最后一格。auto范围不会主动排序成正向，结束模式1会erase，其余回到start。

原点表 `214de8` 的16字节为 `[0,5,7,13,15,1,3,4,12,0,0,0,0,0,0,0]`。正偏移为trunc(size/2)，负偏移为 `-trunc(size/2)-(size&1)`；奇数尺寸不能只用对称±half。特定模式先用signed32百分比乘除改变尺寸，再计算原点。

跟随地图滚动只看+a0低byte非0，滚动发生后base/current/target三套坐标都减requested delta*0.0625。erase-on-map-transfer只在地图ID改变时生效，同地图转移保留。layer按整个整数匹配，在每个pass中按picture slot顺序绘制。

当前 [Sprite_Picture](../../../src/sprite_picture.cpp#L93) 对sheet Y再次取 `% rows`、把double X/Y直接转int、对非正zoom直接不绘制，都需要与原版有效输入路径逐项对照。尤其负缩放在原版正常draw入口有明确处理，不能直接等同“不显示”。

### 7.5 prepare：缓存、颜色与透明度

`06c230` 在 transparency恰为100或frame无效时返回false。否则把当前tone及条件启用的screen tone/flash写到image对象，并在dirty、flip或frame变化时重建处理后像素。整图走 `0e30e0`，sheet走 `0e40c0`。

image opacity：`nearbyint(255-transparency*255*0.01)` 后整数转换。该helper `1d87e0 → 1ebe00` 使用原版浮点舍入模式；通常模式是ties-to-even，而不是floor或C++round的ties-away。没有在入口把所有值钳到0..100。位置也用nearbyint，后面某些缩放边界又用另一种round；不能共用一个“取整”概念。

同文件且未dirty时原版缓存路径只处理flip变化与blend更新，不重新复制chromakey。改变透明标志但沿用缓存图片可以与一次全新加载不同。需要保留这个可观察cache规则，而无需照搬引用计数和缓存容器实现。

### 7.6 颜色算法的准确公式

源像素按uint32 `AARRGGBB`解释，在little-endian缓冲中为BGRA。透明源像素的tone处理受chromakey开关控制。输出保留源alpha；图片合成另行消费透明开关和全局opacity。

`1a4d90` 首先对各tone分量做 `T=clamp(picT+screenT-100,0,200)`，然后处理饱和，再处理RGB分量。

饱和 s<100：

```text
L = trunc(3*(R+G+B)/10)
C' = trunc((C*s + L*(100-s))/100)
```

s>100：

```text
L = trunc((B+3*(G+R))/10)
C' = clamp(trunc(((3*C-2*L)*(s-100)+C*(200-s))/100),0,255)
```

s=100保持。注意低饱和与高饱和的L公式并不相同，低饱和也不是常见的0.299/0.587/0.114亮度。

每个RGB tone t：t<100时 `C=trunc(t*C/100)`；t>100时 `C=C+trunc((255-C)*(t-100)/100)`；t=100保持。

随后是picture flash，再是screen flash。令 K=flashAmount*8，F=flashChannel*8；只在K>0时执行 `C=trunc(((256-K)*C+K*F)/256)`。最后按对应低8bit重组，不可以把整个流程提前clamp为另一个颜色算法。

当前 [Bitmap::ToneBlit](../../../src/bitmap.cpp#L895) 用Y'CbCr亮度与128/1024尺度的hard-light表，Sprite_Picture又先把原版100尺度映射到128尺度。**这不是仅有最后1个像素的疑似差异，而是不同的公式和量化阶段。** 要求一致时，Maniac图片需要使用自己的精确tone路径；普通EasyRPG图片和字体接口继续用现有逻辑。

### 7.7 flip：完整nibble分派与尺寸边界

原版 whole flip nibble 1/2/3分别走H/V/HV；其他值走默认copy/tone，不能把它们按两个bool的bit叠加。

wholeimage V路径还有明确尺寸门槛：height<3走默认copy/tone，所以height2并不执行预期的V翻转。HV在某一轴退化时会进入相应单轴分支。sheet入口有独立的尺寸分支，应按其自身代码核对，不能把wholeimage规则直接套过去。

这是固定制品的确定行为；如果最高行为一致性包括小尺寸图片，就要保留，而不是调用一个通用FlipBlit后认为1×2也“更正确”。

### 7.8 合成：原版的16种组合

普通blit `013400` 将 `blend低2bit<<2 | (opacity<255?2:0) | chromakey` 作为16路选择。未知blend的消费者只取低2bit；flip消费者却比较完整nibble，二者不能统一。

给定源RGB S、目的RGB D，逐通道的完整opacity分支：

|blend|函数 F(S,D)|
|---|---|
|Normal|S|
|Multiply|trunc(S*D/255)|
|Add|min(S+D,255)|
|Overlay|D<128：trunc(2*S*D/255)；否则 `2*(D-trunc(S*D/255)+S)-255`|

启用chromakey时源alpha==0跳过，非0则接受源RGB；该图片路径不是按源alpha0..255进行现代premultiplied混合。没有opacity混合的Normal可以整像素复制，其余RGB路径有自己的alpha输出，不能把中间alpha一律按pixman重新计算。

当picture opacity p<255时，原版采用：

```text
channel = trunc((F(S,D)*(p+1)+D*(256-p))/256)
```

Normal的机器路径按位移与mask形成各通道；Multiply/Add/Overlay先计算其F再执行该权重。**权重之和是257，且p==255走单独路径。** 不能擅自改成255-p或除255。Overlay高通道公式先截断乘积，再补色，与常见overlay公式的截断位置不同。

现有 [GetOperator](../../../src/bitmap.cpp#L1303) 把Normal/Multiply/Add/Overlay交给PIXMAN_OP，无法仅凭操作名称保证这些规则。补完应提供Maniac图片的逐像素合成合同，必要时只替换该路径，继续复用Bitmap表面和呈现。

### 7.9 缩放、旋转与波形取样

**普通/缩放分派 `06dba0`：** 先用nearbyint取中心X/Y，再按flag加screen shake。宽高当前百分比转int等于100时可能进入native-size fast path，即100.x并不总是精确浮点缩放。一般缩放的源/目标rect同时裁剪，整数乘除会调整比例，接近0的剩余取样范围在部分分支保持1。不能用一次浮点matrix裁剪替代所有步骤。

`017400` 建立水平源索引表；每列累加原始宽度因子再做整数除法，负目标宽高另有反转。垂直同样以整数累计选择行，没有双线性滤波。右/下边界和screen上下分区flag也在该路径参与裁剪。

**旋转 `018d10`：** rotate1传 `-angle*1.40625` degree；fixed angle/保留angle的其他分支传 `-angle`。边界以原版舍入后的scaled宽高、sin/cos、奇偶尺寸和正负scale构造，并有`+1`包围边界。逆映射起点对两轴减0.1；逐列/逐行用double累加，`CVTSD2SI`按当前舍入模式取得源坐标，经无符号界限检查后绘制。这与pixman 16.16矩阵、默认像素中心和普通floor最近邻不是已证明相同的算法。

本轮新增的 [旋转分支汇编](rotation-branch-assembly.txt) 覆盖原版16个组合，确认nearest坐标转换、源界限、透明门槛和混合分支。截图相似不能替代这些坐标与像素检查。

**Wave `01aa70`：** draw入口把power*4以nearbyint量化，phase由update每次+8。每行取低8bit的相位索引，使用原版256项double表 `260bb0`，乘振幅后trunc为横向位移。行索引与缩放、裁剪范围有关，不能直接把相位套到当前屏幕Y；x取样则沿预先生成的源索引表。位移后再次裁剪左右范围。16种混合分支见 [波形分支汇编](wave-branch-assembly.txt)。

当前 [WaverBlit](../../../src/bitmap.cpp#L781) 和 [RotateZoomOpacityBlit](../../../src/bitmap.cpp#L1240) 是EasyRPG通用实现。应分别核对native-size、zoom、rotate、wave四种算法，不能只修fixed-angle除法就宣布绘制一致。

表的来源也已追到 `183420`：对k=0..255，以原版sin helper计算 `double(k)*3.14159265358979*0.0078125`，逐项写到260bb0；另用cos helper生成2603b0表。这是启动期生成的运行态表，不应直接读取PE中的初始零区当成最终数据。常数和乘法顺序应保留；实际bit值与跨平台sin内核的一致性仍属于5.3的边界。

### 7.10 图片像素操作与图片输出

3025/3026/3028与图片显示共用源/处理后像素、sheet和dirty状态。读取图片信息、导出变量数组、改变像素、重建文字图片和保存修改图片必须明确读取哪一层。

`06c5f0/06cc10` 的像素→numeric range处理会对裁剪外部分填0，输出宽高和银行范围以原始请求参与计算，不是只输出截过的Rect。`06d470` 的数组→image操作更换源/backing、kind与dirty。不能修改显示用bitmap后仍沿旧文件名恢复，也不能让色调后的临时pixels变成新源后重复套tone。

保存kind2图像使用原版压缩BGRA源字段；外部PNG编码器、文件路径和平台选文件行为可保留EasyRPG，但图像内容、目标银行、结果值及写入顺序属于游戏语义。图片修改与输出的全操作mode仍需按既有契约和消费者关闭，不能由本轮读取的两个共享helper代替全部mode验收。

## 8. 文字图片：从游标到存档的独立流程

### 8.1 3007 Text必须按游标消费

原版流程：整正文引用替换 → 正文解析 → 在返回游标读system配置 → 继续读font配置 → 写canonical描述 → 绘制。

首UTF-16 unit2直接取A22 string，3先取numeric A22再取string并传播SELF符号，构造 `1 + 完整引用正文 + 原Text[1:]` 后重新扫描。插入的正文控制字符能改变system/font边界。

正文跳过初始unit；0/1/2是终止分隔，unit3不是统一分隔符。反斜杠及参数列表有自己的推进方式：括号参数可以越过unit1/2寻找闭括号。因此**先Tokenize成四段再分别处理，从根本上失去原版信息**。

配置 `034c60` 的mode2/3 ID按UTF-16 unit中的低5bit累计：`id=(id<<5)|(unit&31)`；0x40说明ID组，0x20表示继续。mode3再经numeric解析string ID。其他mode按literal读取到unit<3。system、font的引用ID在Text内，不是另一对Args。

当前 [3007解析](../../../src/game_interpreter.cpp#L6312) 的Tokenize、仅正文GetWithMode、system/font当literal、`.empty()`回退，是本轮确认的直接差异。应使用局部UTF-16 cursor，保留raw参数和当前SELF上下文，复用已有numeric/string访问接口。

### 8.2 转义与插入是分阶段的

|输入|正文行为|
|---|---|
|V/v|解析numeric reference并输出十进制，可受D格式宽度影响|
|T/t|直接插入string value，LF分行；插入内容不重新进行顶层分隔/转义派发|
|N/n|actor名，0选队伍首人，按原版actor查找错误规则|
|SELF forms|由共享SELF转义解析器处理，不能把普通S泛化为ON/OFF|
|D/d|数字宽度限制1..10，影响后继数字，不产生字形|
|C/c|两值颜色状态，读完额外参数；canonical保留规范化控制码|
|$A..Z/$a..z|图标0..51，源格12×12|
|$$|一个字面$|
|其他$形式|按两值图标参数处理，缺值使用原版默认值|
|未知反斜杠序列|丢反斜杠、保留随后unit；双反斜杠及末尾反斜杠有独立规范形式|

创建时的whole-body引用会重新解析；正文中的T插入却不递归解析；读档canonical又使用重建模式。这三者不能通过同一次DefaultCommandInserter调用统一。

### 8.3 字体保留，布局与文字合成按原版合同

字体由EasyRPG选择及生成字形，继续使用当前Font的回退和平台逻辑。原版请求字体名可以作为状态保留，不需要强制选择RPG2000/RPG2000G、照搬LOGFONT或加入Windows专用字形路径。

其余可移植规则：

- F=(A13>>8)&0xffff，低nibble0/未知无背景，1拉伸system左上32×32，2平铺；bit4禁边框，bit5禁渐变，bit6禁(+1,+1)阴影，bit8禁普通横8/纵10margin。
- 自动W取最大行advance+iconCount*fontSize+字间距，再加两侧margin。自动H为两侧margin+rows*(fontSize+lineSpacing)-lineSpacing；零行另有 `2*verticalMargin+fontSize`。无margin且有shadow的非空分支加1尺寸。显式非0尺寸覆盖自动尺寸。
- icon为fontSize正方形，用12×12原版源格和整数最近邻索引。颜色用system的16格坐标和原版取样点；越界颜色保留原状态。
- glyph/icon包括shadow在内的整个rectangle超界时整块拒绘，不是通用Bitmap只裁去越界部分。
- 原版glyph `035a30` 把GDI生成的像素非0作为掩码；它不是直接把GDI RGB当最终字色。渐变与flat分支从system图选择最终颜色；shadow先写偏移像素、主体随后覆盖。EasyRPG掩码仍由现有字体生成，其色彩、shadow和clip后续处理按这一合同组织。

必须把“保留EasyRPG字形生成”作为允许差异的源头写清楚。字体相同名字未必相同文件，跨平台advance不同也不应被误报为Maniac解析bug；parser改变颜色、图标、间距或创建后变量重新展开，则不是字体差异。

### 8.4 创建快照、恢复模式与原版字段

`035360` 将canonical写picture+98：`1 + 处理后正文 + 1 + 已解析system + 1 + 已解析font`。原版writer `06ba60` 的chunk1正是此字段。

|图片chunk|保存内容|对Player的要求|
|---|---|---|
|1|名字或canonical text描述|文字kind按已解析描述恢复|
|6|+a0合并word，高byte含kind|不能仅保留fixed_to_map bool|
|9|+a4完整word|透明、文字间距仍有含义|
|0x10 /16|+58的double|fixed angle target须保留fraction|
|0x12 /18|+78的8字节宽高对|文字kind可用作有效尺寸，不能误读成普通double参数|
|0x1c /28|kind2压缩BGRA源|修改图片可新实例恢复|
|0x23 /35|+b8完整int32|文字style/字号/blend/flip，不能按底部透明度重写|
|0x2e /46|+cc原始int32|与double angle target独立|
|0x33 /51|positive remaining|默认恢复与运行0/-1状态要按原版loader理解|
|0x34 /52|+70的double angle|过渡中当前位置须保留|
|0x35 /53|wave phase|恢复后不能重新从0开始|

原版loader `06b370` 读kind1后调用 `0351d0`；该入口按已存字符串建立上下文，调用body parser的重建模式，然后读两个已解析配置，使用存下的style生成图像。它不调用带A22的创建入口，不重新读取已改变的正文变量。

当前 [Game_Windows::SetSaveData](../../../src/game_windows.cpp#L49) 从EasyRPG私有window记录恢复，[Refresh](../../../src/game_windows.cpp#L225) 经PendingMessage再次处理文本。原版只有kind1图片描述而没有私有window记录时，也必须能重建。恢复不应把canonical送回创建期转义程序。

因此需要两个明确入口：创建期解析为有效状态；从保存的有效状态重建。可继续使用当前Game_Windows管理显示对象，新增的状态保持局部且有实际持久化用途，不要把原始Text和canonical再次混用。

## 9. 存档及平台适配

### 9.1 SELF 序列化会主动丢弃一些运行态信息

原版root serializer `081c90` 的SELF root0x74由 `17cfd0` 输出common/map银行；内层 `17c430` 输出switch的布尔run、去掉尾0的numeric及string序列。物理slot0不进入这些字段。

原版运行时switch可以保留raw byte，但SELF保存run只区分0/非0，因此reload后可能变为0/1。不能将“raw byte运行时不变”误当成“原版SELF存档必须逐byte保留”。EasyRPG扩展可以保留额外运行信息，但读取原版字段的路径必须符合原版。

字符串save/load同样有NULL run与显式长度协议。原版写出的allocated empty不一定经loader保持为空串对象；本项目 [ManiacOriginalSave](../../../src/maniac_original_save.h#L119) 已明确记录这类差异。原版固有的有损过程应按实际loader解释，不能“修正”成更漂亮的往返。

### 9.2 恢复要覆盖三类状态

|类|例子|恢复合同|
|---|---|---|
|游戏持久状态|numeric/switch/string、SELF、图片current/target、有效描述|从原版字段或EasyRPG扩展恢复，包含kind解释|
|可重建缓存|bitmap、frame缓存、Font对象、dirty缓存|由有效状态重建，不重新执行创建指令|
|进行中的执行状态|frame身份、cursor、return receiver、wait、hook上下文、dynamic command|恢复后继续正确后缀；不可假定全部来源是当前活动事件|

应以“新进程读档后执行下一条命令”的可观察结果关闭，而不仅比较存档字段或同进程对象。原版没有保存的cache不要凭空要求持久化；但若EasyRPG采用不同内部调度，就需要其自己的字段来恢复等价状态。

### 9.3 保留的平台边界

|原版实现细节|Player的处理|
|---|---|
|LOGFONT、字体名查找、GDI rasterizer|保留EasyRPG Font；只复用语义状态与布局合同|
|GetAsyncKeyState、鼠标定位、Win32焦点与消息循环|保留EasyRPG Input与文本接收；不模拟Windows设备状态机|
|DirectSound ring/MIDI backend内部位置|以现有音频后端提供的值/能力为依据；命令级单位需要明确，不移植buffer布局|
|CreateThreadpoolWork、原版像素并行策略|不影响像素结果时保留EasyRPG执行策略|
|Win32 fullscreen/媒体窗口/确认框|保留EasyRPG已有用户流程；脚本scene/wait/result的可观察部分单独核对|
|文件名编码、native path、打开/保存UI|保留EasyRPG文件系统与平台流程；payload、写回与状态顺序仍一致|
|原版CRT/allocator的内存故障|不复刻内存损坏；合法操作的分配副作用与错误语义另行保留|

不要为弥补一处图片分歧要求Web和Android复制Win32，也不要因为保留输入行为而放弃脚本参数、SELF接收地址、等待和结果写回。

## 10. 当前项目差异与参考补丁的取舍

### 10.1 确认的差异与要保护的既有能力

|ID|结论|当前位置|优先级|
|---|---|---|---|
|D01|3007预分段与原版cursor协议不同；system/font引用没有同等解析|game_interpreter.cpp:6312|P0|
|D02|创建期缺少按阶段保存的有效文本与资源快照；恢复链不能仅依赖私有window|game_windows.cpp:49、225|P0|
|D03|固定角在指令入口做整数除法，update混用raw target与double current|game_interpreter.cpp:3434、3584、6278；game_pictures.cpp:729|P0|
|D04|图片颜色算法与原版不同，128尺度量化和Y'CbCr引入系统性差异|sprite_picture.cpp:155；bitmap.cpp:895|P0|
|D05|PIXMAN_OP名称不能保证原版16路RGB、alpha与opacity公式|bitmap.cpp:1303|P0|
|D06|double坐标直转int、floor zoom、通用rotation/wave与原版各自舍入/取样不同|sprite_picture.cpp:107；bitmap.cpp:781、1240、1286|P0|
|D07|文字类型的word字段进入普通bottom transparency插值/重置|game_pictures.cpp:694、SetNonEffectParams|P1|
|D08|Normal停止旋转余波写回字段与原版机器指令不同|game_pictures.cpp:700；原版06db5b|P1|
|D09|whole flip nibble、未知值及height2 vertical分支不能由两个bool完整表示|game_interpreter.cpp的flip解析与Sprite_Picture|P1|
|D10|原版地图事件与变量随机共用LCG；Player的通用Rand与Maniac seed分流|main_data.cpp:68；rand.cpp:59；game_event.cpp|P0|
|D11|MovePicture 的 relative 对位置、宽高缩放和透明度做目标增量；RGB 由独立 keep-RGBs 位控制。5c64d306c 误将非位置目标全部保留，dcdc3c3ca 已修正|game_pictures.cpp:370、SetNonEffectParams；原版03c6f0 的 ADD 写回|P0|
|D12|原点表已保留，但 X/Y 位组译反；右上、左下及边缘中点的锚点偏移错误|game_pictures.cpp:649；原版03c6f0与214de8|P0|
|D13|10860 的高字节动态事件寻址没有进入现有 GetCharacter；变量编号被当作物理事件编号|game_interpreter.cpp:2936；原版02b640|P0|
|D14|MovePicture 普通非负时长至少 1 帧，keep-transition 也至少 1；5c64d306c 中的 0 帧目标不被普通插值消费者更新，dcdc3c3ca 已修正|game_pictures.cpp:402、806；原版03c6f0 与06d8c0|P0|
|A01|实时bounds、整数位模式、LCG、MAX尾项、表达式broadcast等已有大量对应实现|control_variables、main_data、maniac_patch|保留并复核消费者|
|A02|NULL/present、SELF存储、原版存档已有基础|maniac_string_storage、game_maniac_self_variables、maniac_original_save|保留并统一调用|
|R01|CmdSetup补齐会改变存在性判断，但影响须逐消费者判定|game_interpreter_shared.h:211|定向审计|
|R02|数学公式接近原版不证明libm整数边界和跨平台bit等价|control_variables.cpp|补完前核验|
|R03|命令可分派不证明战斗/消息/保存等待链已等价|battle/map/message消费者|补完前核验|

P0/P1表示补完顺序，不是对当前项目的通用安全漏洞评级。D04/D05/D06等确认算法不同；具体游戏中哪一帧表现如何，未在本轮运行观察。D11–D14 是交叉核验后新增或更正的共享入口差异：它们不是新的游戏特例，而是会被使用相同图片/事件参数的游戏复用。D12/D13 已进入有限修复提交，其存在不代表其余合同已关闭。

### 10.1.1 交叉核验及更正：最小共享合同

上一轮实现检查了图片状态、像素路径和运行模式的若干下游合同，但没有把三个直接消费者的字段转换完整闭合。将它们放回“原始参数 → 引用解析 → 目标状态 → 几何转换 → 绘制/事件消费者”链后，缺口可以精确收敛为以下规则：

1. **MovePicture 的 relative 对位置、两轴缩放和透明度做目标增量，RGB 独立更新。** `0x02` 设置后，这些增量加到既有目标，而不是 current；`scale(0)` 不改变现有缩放是因为加零，`scale(-20)` 会把 `120` 改成 `100`。RGB/饱和度只有设置 `0x04` keep-RGBs 才保留，否则仍写入本次求值目标。`0x40` keep-transition、特效与文字 style word 是其他独立规则。原版 `03ceaa/03cf97/03d095` 分别对两轴缩放及透明度 ADD 写回；此前“只改位置”的判断错误，依据与游戏路径见 [消费链复核 §3–4](consumer-contract-followup.md#3-原版-movepicture-的真实合同)。
2. **原点表的表值和轴向是两件事。** `214de8 = [0,5,7,13,15,1,3,4,12,...]` 的低两位描述 Y，高两位描述 X；每轴再使用正半尺寸或带奇数像素的负半尺寸。把表复制正确但将两组位送入相反轴，仍会产生完整帧宽/高的双倍偏移。应使用 64×48 这类非正方形图片，并覆盖 origin 1–8，才能避免对称锚点掩盖错误。
3. **10860 的参数一同时包含坐标模式和事件接收者模式。** 低字节选择常量/变量/间接变量坐标，高字节的低四位独立选择事件 ID 的求值方式。`[454,257,257,258]` 的 `0x0101` 不是物理事件 454，而是 `V[454]` 指定的事件，坐标为 `V[257], V[258]`。事件接收者必须先解析，再进入 `GetCharacter`；坐标和方向仍按低字节模式解释。只把高位留在参数中而继续直接使用 A0，等价于没有实现该字段。

4. **MovePicture 的时长入口必须满足更新器前提。** 原版普通非负时长转换后至少 1 帧，keep-transition 小于 1 时也归为 1；普通更新器在 remaining=0 时不插值。当前入口产生 0 帧而仍沿用该更新器，使没有指定 `.time` 的移动只改 target，current 不变。

这些合同说明了先前报告为什么没有阻止现象出现：D03/D06 已覆盖图片状态和几何下游，A01/A02 已覆盖变量银行，但 11120 的 relative 字段分支及 duration 前提、原点表的轴向解释和 10860 的具体消费者仍未形成逐字段闭环。完整兼容的审计单位应包含入口参数的存在性、每个模式位的求值、目标字段写入/增量/保留和下游消费者；“handler 可分派”“字段可保存”或“通用变量读取可用”都不能替代它。

### 10.2 Player 未提交更改可复用的部分

参考 Player工作区（外部审计制品） 当前9个未提交文件约658新增、109删除。它们作为参考保留，未被本轮修改或移入Kai。

可作为补完起点的思路包括：UTF-16游标而不是预切段，配置5-bit引用，创建时effective text，原版kind字段解释、独立fixed-angle double target、文字style保护、原版文字图片重建入口。这些分别对应D01/D02/D03/D07。

需要重新筛选的部分：

- font.cpp中的原版字体/GDI趋同，不符合这次保留EasyRPG字体选择与字形逻辑的边界。
- 仅把flip转成bit bool，尚不能覆盖完整nibble和小尺寸分派。
- 仅修文字图片parser和角度，不会自动修复tone、opacity、zoom、rotation、wave像素链。
- 带ManiacVersion的条件属于参考树旧实现残留，不能再引入Kai；继续用现有Maniac启用条件。
- 字段和loader模板改动需按原版kind/default/canonical恢复合同审查；不能靠“存下更多字段”证明行为一致。

该补丁集不是应整体cherry-pick的完整答案。它揭示了共同原因，并提供部分有用实现，仍需按本报告的原版合同整理。

## 11. 面向完整补完的实施顺序

### 11.1 用现有结构完成四组共享改动

无需重写EasyRPG，也无需新增可选择的Maniac版本系统。推荐以下顺序，每组交付一个可审查的行为结果：

|阶段|修改面|交付条件|
|---|---|---|
|S1 原始语义入口|逐消费者的raw count、signed ID、NULL/present、UTF-16 cursor|短/显式0、SELF及配置读取可区分，未知位/尾项不丢失|
|S2 图片与文字状态|Show/Move/3007、Game_Pictures、Game_Windows、必要liblcf字段|double/current/target/raw分开；relative 的位置/缩放/透明度增量及独立 RGB 保留、duration 前提、origin 轴向和创建 snapshot/restore 入口闭合；文字word不走普通透明度路径|
|S3 图片精确像素|Maniac专用tone、flash、4×4blend与native/zoom/rotate/wave取样；复用Bitmap表面|给定相同源像素、字形掩码和状态，输出像素按合同一致；普通EasyRPG路径保持现有行为|
|S4 其余消费者与持久化|随机消费者统一、表达式/数组/strings入口、消息/事件/battle、save/load；10860 的接收者求值|共享helper被所有需要的消费者使用，进行中状态恢复后继续执行一致|

不建议把所有指令改成新的“大兼容框架”。很多核心规则已有实现，具体需修的入口、状态和像素函数才是变更面。

字体和输入在各阶段始终通过现有EasyRPG API。若要精确像素路径，可以在现有Bitmap上写经过计算的像素，再继续使用EasyRPG显示，避免将compatibility逻辑耦合到某个图形驱动。

### 11.2 验收以原版分支导出的有限基组组织

以下是后续实施的验收设计，**本轮没有新增项目测试或运行这些用例**。实施时先复用既有非UI检查；如需永久测试代码或人工UI验收，应按用户约束取得授权。

|组|有限分支基组|观察量|
|---|---|---|
|参数|缺项/显式0/原生writer短长度/3019生成长度|执行分支、raw count、保留payload|
|银行|global/self/caller、direct/indirect、slot0、缺失/空串、同银行别名|目标银行、值、逻辑长度、存在性|
|数值|实时bounds、signed极值、除0、shift低5bit、round/trunc边界、有限/非有限转换|int32位模式、FP中间值和转换位置|
|表达式|pair、list、range、scalar、short-circuit、额外子项和alias|primary/secondary、cursor、写入序列、PRNG状态|
|绘制|4blend×2chromakey×2opacity分支，含255与254；native/zoom/rotate/wave|每像素RGB/alpha、采样坐标、clip范围|
|几何|奇偶宽高、负scale、0/1/100.x、半像素、sheet端点、flip完整nibble|边界和逐帧状态，不仅最终截图|
|文字|whole-body引用、T非递归插入、system/font引用、嵌NUL、颜色/图标/shadow/margin|parser cursor、有效描述、字形外的像素|
|调度|busy前后、hook栈切换、3019重复生成、save结果快照后写入|frame/cursor/wait/consumer suffix|
|恢复|普通图/文字图/修改图、angle过渡、SELF、caller返回值|同进程恢复与新进程恢复后下一步输出|
|平台|同一份源码的Windows/Web/Android|核心整数/像素/恢复合同；字体/输入允许差异单列|

这是从原版分支导出的合同基组，不依赖寻找足够多的游戏。相等颜色、对称图案会遮掉错误，应使用不对称源像素、不同RGB通道、不同key伴随值；只测0/100透明度不会触及254等量化分支。

允许用变换关系减少重复验证，例如相同输入的显示像素与导出像素关系、保存后同一状态的恢复、同银行alias变化后的后续读取。但变换关系本身也必须由原版合同支持，不能拿理想数学关系替代原版怪异行为。

### 11.3 168条指令的关闭方式

每个固定opcode记录以下信息即可：原始字段/存在性 → bank/context → 数值与Text解析 → 状态写入顺序 → 调度结果 → 下游消费者 → 保存/恢复 → 保留的平台边界。

共享机制已经证明的部分引用同一证据，不为168条指令各重建一套测试。覆盖率按消费链闭合，不按“函数存在”“handler返回true”或测试断言总数计算。见 [168条指令路由与继续审计表](command-audit-matrix.csv)。矩阵保留原始路由证据，并明确本轮没有逐条将全部语义标记完成。

完成条件是这套定义下的游戏行为一致和平台保留边界明确；成功构建、已有某次本地产物、单个游戏能通关都不能替代它。真实游戏适合做最后的集成验证，核心机理的正确性应先由上述有限合同建立。

## 12. 明确的未关闭项与最短下一步

|项|当前已经知道|仍需要的证据/工作|
|---|---|---|
|数学内核|调用链、double、bounds、转换、原版pow CPU分支已定位|目标平台libm与原版的整数输出边界差分；必要的小范围内核一致实现|
|旋转/wave后续分支|原版32路代码及跳表字节已补出；取样、合成和wave256表初始化公式已核对|将精确采样循环规格实现并做像素对照；表的bit结果纳入原版数学内核差分|
|图片像素edit/export|共享像素访问、kind、dirty与银行路径已定位|各操作mode的完整消费者、文件像素及新实例恢复验证|
|字符串/regex|UTF-16、NULL、SELF及文字创建/恢复链已核对|3020各变体与原版库语义的定向对照|
|战斗/消息/移动/JS|原版共享调度合同及既有实现已有依据|完整状态消费者与等待/继续链关闭；本轮未执行旧暂停检查|
|原版全序列化|图片writer/loader、SELF root和初始seed来源已核对|剩余执行状态、随机state是否由原版持久化的对应确认；不能臆造原版字段|
|原版间接/virtual边界|固定Evidence保留了未解析点|只针对影响上述合同的调用继续收集，不重新扫全部函数|
|字体与输入|本报告明确保留EasyRPG|后续实现不可为填coverage扩成GDI/Win32移植|

最短的下一批应是 **S1/S2：整理3007游标与有效状态，闭合 MovePicture 的 relative 目标字段、原点轴向和 10860 动态接收者，再补固定角的 double/current/target 合同及原版文字图片恢复入口**。这直接清除交叉核验暴露的共享缺陷，改动面清晰。随后必须做S3；如果仍沿用当前tone/pixman通用路径，即便文字和角度修好，图片细节问题仍会在不同游戏中出现。

本报告提供补完依据、完整机理分解和关闭路线，没有将未调查完的所有168条指令或所有数学输入域标成“已完全一致”。本轮源码保持验证见 [工作区保护检查](source-integrity.json)。

## 13. 证据索引与可复查性

本轮校验并读取 49 个原版函数的 53 份规范facet记录；其中包括完整读取、分段读取及仅摘要读取，精确范围逐项记录，数量不替代语义覆盖。

[原版Evidence索引](evidence-index.md)连接规范记录的文本摘出与完整ID；规范记录摘要和实际读取范围也保留。完整live证据包留在本地审计目录，未纳入仓库。旋转/wave跳转表及Capstone分支解码附带字节摘要清单。

command-audit-matrix.csv的prior status只引用当前仓库已有索引，未作为本轮验收结论；audit_objects为继续审核的导航分类，未证明每条指令都访问列出的所有对象。source-integrity.json校验两个工作区开始时每个已登记文件的摘要及Git状态，保留Player既有9个未提交文件。
