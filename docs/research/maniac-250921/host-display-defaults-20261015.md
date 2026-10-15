# Maniac 显示设置的宿主回退

2026-10-15，依据固定行动准则及用户确认的显示策略修正。沿用既有250921基准，不新增选版或显示配置开关。

|平台|启动策略|游戏主动显示指令|
|---|---|---|
|Web|保留嵌入页面的布局和EasyRPG显示配置，不导入或回写`ManiacVideoSettings.bin`|保留原有指令；尺寸与定位使用canvas所在窗口的视口，退出全屏后恢复原嵌入样式或先前的显式窗口倍率|
|Windows|仅存在可读取、类型为`REG_BINARY`且长度为16字节的原有`VideoSettings`记录时导入；否则保留EasyRPG配置|保留现有处理；没有导入记录时不在退出阶段创建合成的原版配置|
|Android|仅存在可读取的`ManiacVideoSettings.bin`记录时导入；缺少或短记录保留EasyRPG配置|保留现有SDL3适配；没有导入记录时不在退出阶段创建合成配置|

这里的显式记录指当前代码原有的注册表值或对应的持久化文件，不引入新的`RPG_RT.ini`键。已有有效记录的读取、保存和全屏CLI覆盖继续有效。

## 成因与修正

原先`LoadManiacVideoDefaults`在未读到记录时也合成2倍窗口、普通窗口模式、插值与FPS默认值，并调用各平台setter。Windows的EasyRPG倍率会在窗口创建后被覆盖；Android的`maniac_window_zoom`及请求标志会激活独立视口分支，导致应用自己的缩放设置失效。现在仅成功导入实际记录才设置`maniac_video_defaults_loaded`并应用这些值；未导入时保存阶段也直接返回。Web始终采用宿主默认行为，不访问这份文件。

Web原先使用`window.parent.innerWidth/innerHeight`计算iframe内`position:fixed`的canvas坐标，混用了两个视口。现在显式显示请求按canvas所在的`window`计算；分辨率、iframe自身resize及全屏状态变化时重新应用布局，未设置窗口倍率时恢复启动前的inline布局值。父页面的焦点、外部触控和全屏状态连接继续保留。

## 验证范围

- Node在隔离的视口/样式对象中执行产品布局闭包：18项通过，覆盖默认自适应、嵌入原样式、显式倍率/fit、父子视口差异、iframe resize、全屏整数/等比/拉伸、返回及容器布局恢复。显式倍率未请求fit时，退出全屏也会清除全屏遗留的位置样式。
- 临时C++入口编译产品的`LoadManiacVideoDefaults`、`SaveManiacVideoDefaults`方法，分别选择Windows、Android、Web预处理分支，使用隔离的注册表/文件后端：72项断言通过。覆盖记录缺失、空/短记录、Windows类型/长度错误、有效记录、幂等、CLI覆盖、已有记录的保存，以及缺省情况下不写出新记录。没有读写用户注册表或配置。
- 504项既有非UI检查、3,677,971断言通过；正式测试和入口未修改。任务内检查源码在完成后移除。
- Windows Release x64和Web构建、打包通过；Web包内布局脚本与最终源码一致，必需文件和归档完整性检查通过。

构建及产物记录位于本地`build/maniac-display-defaults-20261015/`。Android的SDL3显示分支已核对，但本次没有真机/UI验收或Android APK构建；Node对象检查不作为浏览器验收，源码检查和构建不作为实际显示验收。
