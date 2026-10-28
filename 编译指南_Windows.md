# EasyRPG Player Windows 编译指南

本文档记录了在当前 Windows 设备上编译 EasyRPG Player 的完整步骤。

## 当前设备的固定入口

获得本次构建授权后，使用仓库包装脚本和现有 ASCII 构建目录：

```powershell
& ./builds/cmake/build-windows.ps1 -BuildDirectory C:/buildscripts/mr14/player-build -Targets @('EasyRPG_Player_exe', 'test_runner_player') -ConfigureArguments @('-DPLAYER_BUILD_LIBLCF=ON') -Parallel 4
```

内置 `lib/liblcf` 必须应用本仓库的 `builds/patches/liblcf-maniac-250921.patch`；否则会缺少当前 Player 所需的 Maniac 字段。2026-10-29 已按用户要求在本机 checkout 应用该配套补丁。先检查并保留它的本地改动，再检查补丁是否已存在：

```powershell
git -C lib/liblcf status --short
git -C lib/liblcf apply --reverse --check ../../builds/patches/liblcf-maniac-250921.patch
```

反向检查通过表示已经应用，不要重复打补丁。若尚未应用，先确认正向检查通过再执行：

```powershell
git -C lib/liblcf apply --check ../../builds/patches/liblcf-maniac-250921.patch
git -C lib/liblcf apply ../../builds/patches/liblcf-maniac-250921.patch
```

保持 `PLAYER_BUILD_LIBLCF=ON`，让 Player 和内置的匹配依赖一起构建。该选项为 ON 时，已有 `lib/liblcf` 优先于预编译依赖，单独设置 `liblcf_DIR` 不会改用安装版本。包装脚本会在调用 CMake 前拦住已知的旧头文件组合；先确认配置输出中的依赖选择，再编译。不 reset 旧 checkout、不修改全局 PATH，也不原样重试缺字段的失败配置。

包装脚本从缓存复用源码与 CMake 路径，并将配置、编译的工作目录都切到 ASCII 构建目录。仅把 `-S`、`-B` 写成 ASCII 路径但仍从中文目录直接调用 CMake，不足以规避本机路径问题。下文其他预设供独立环境配置参考，当前目录构建优先使用上述固定入口。

## 环境信息

- **操作系统**: Windows
- **编译器**: Visual Studio 2022 Community
- **CMake 版本**: Visual Studio 2022 附带的 CMake 3.31.6
- **依赖库来源**: buildscripts 预编译库用于 Release，本地 vcpkg 用于 Debug

## 目录结构

```
C:\Users\旻\Documents\GitHub\
├── Player\              # EasyRPG Player 源码
├── buildscripts\        # EasyRPG 官方依赖构建脚本和预编译库
└── vcpkg\              # 本地 vcpkg

C:\Player               # 指向 Player 的 junction，用于规避 CMake/VS 的中文路径问题
C:\buildscripts         # 指向 buildscripts 的 junction，用于 Release 单文件构建
C:\vcpkg                # 指向本地 vcpkg 的 junction
```

## 前提条件

以下软件和依赖库已经安装配置完成：

1. **Visual Studio 2022 Community**
   - 包含 C++ 桌面开发工作负载
   - 已安装路径：`C:\Program Files\Microsoft Visual Studio\2022\Community`

2. **Visual Studio 附带的 CMake**
   - 推荐使用：
     `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
   - 系统 PATH 中的 CMake 4.2.0-rc1 在当前中文用户名路径下可能异常退出，不建议用于本仓库

3. **Git**
   - 用于克隆 liblcf 库

4. **buildscripts 预编译依赖库**
   - 实际位置：`C:\Users\旻\Documents\GitHub\buildscripts`
   - 推荐 junction：`C:\buildscripts`
   - Release preset 使用 `C:\buildscripts\windows\vcpkg`
   - 当前发布 triplet：`x64-windows-static`

5. **本地 vcpkg 依赖库**
   - 实际位置：`C:\Users\旻\Documents\GitHub\vcpkg`
   - 推荐 junction：`C:\vcpkg`
   - 当前使用 triplet：`x64-windows`
   - Debug preset 会显式使用动态 CRT，以匹配 `x64-windows` 依赖库

## 编译步骤

### 1. 配置编译环境

打开 PowerShell，导航到 ASCII 路径的 Player junction：

```cmd
cd C:\Player
```

如果 junction 不存在，先创建：

```cmd
cmd /c mklink /J C:\Player "C:\Users\旻\Documents\GitHub\Player"
cmd /c mklink /J C:\buildscripts "C:\Users\旻\Documents\GitHub\buildscripts"
cmd /c mklink /J C:\vcpkg "C:\Users\旻\Documents\GitHub\vcpkg"
```

如果 `C:\buildscripts\windows\vcpkg` 不存在，进入 `C:\buildscripts\windows` 后运行 `download_prebuilt.cmd`，或手动下载 `https://ci.easyrpg.org/downloads/windows/toolchain-windows.zip` 并解压到该目录。

### 2. 运行 CMake 配置

使用本地调试预设配置项目：

```cmd
"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --fresh --preset windows-x64-vs2022-debug-local
```

**说明**：
- 此命令会使用 `C:\vcpkg` 中的本地依赖库
- 会使用 `x64-windows` 和动态 CRT，避免与本地 vcpkg 库链接冲突
- 会自动克隆并配置 liblcf 库（如果 `lib/liblcf` 目录不存在）
- 配置文件会生成到 `build/windows-x64-vs2022-debug-local` 目录

**可用的其他预设**：
- `windows-x64-vs2022-debug-local` - 当前设备推荐的本地调试版本，动态链接，不能单文件分发
- `windows-x64-vs2022-release-local` - 当前设备推荐的本地发布版本，静态链接，产物是单文件 `Player.exe`
- `windows-x64-vs2022-debug` - 官方调试版本，默认要求 `x64-windows-static`
- `windows-x64-vs2022-relwithdebinfo` - 官方带调试信息的发布版本，默认要求 `x64-windows-static`
- `windows-x64-vs2022-release` - 官方发布版本，默认要求 `x64-windows-static`

### 3. 编译项目

配置完成后，运行编译命令：

```cmd
"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build --preset windows-x64-vs2022-debug-local
```

**说明**：
- 编译过程会先编译 liblcf 库，然后编译 Player
- 编译时间取决于电脑性能，通常需要几分钟

### 4. 查找可执行文件

编译成功后，可执行文件位于：

```
C:\Player\build\windows-x64-vs2022-debug-local\Debug\Player.exe
```

发布版可执行文件位于：

```
C:\Player\build\windows-x64-vs2022-release-local\Release\Player.exe
```

发布版使用 `x64-windows-static`，可以直接把单个 `Player.exe` 复制到游戏目录运行。

```cmd
copy C:\Player\build\windows-x64-vs2022-release-local\Release\Player.exe D:\もしもコレクション7\
```

## 快速重新编译

如果只是修改了源代码，不需要重新配置，直接运行编译命令即可：

```cmd
cd C:\Player
"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build --preset windows-x64-vs2022-debug-local
```

## 清理构建

如果需要完全重新编译，删除 build 目录：

```cmd
cd C:\Player
rmdir /S /Q build\windows-x64-vs2022-debug-local
```

然后重新执行配置和编译步骤。

## 配置文件说明

项目使用了自定义的 CMake 预设配置，配置文件位于：

```
builds/cmake/CMakePresetsUser.json
CMakeUserPresets.json
```

**重要配置项**：

1. **vcpkg toolchain 路径**（第30行）：
   ```json
   "toolchainFile": "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
   ```

2. **自动编译 liblcf**（第32行）：
   ```json
   "PLAYER_BUILD_LIBLCF": "ON"
   ```

3. **本地调试 preset**：
   ```json
   "name": "windows-x64-vs2022-debug-local"
   ```

4. **本地发布 preset**：
   ```json
   "name": "windows-x64-vs2022-release-local"
   ```

5. **发布版 static triplet**：
   ```json
   "VCPKG_TARGET_TRIPLET": "x64-windows-static"
   ```

6. **调试版本地 vcpkg 与 CRT**：
   ```json
   "VCPKG_TARGET_TRIPLET": "x64-windows",
   "VCPKG_CRT_LINKAGE": "dynamic"
   ```

这些配置已经设置好，通常不需要修改。

## 常见问题

### 问题1：找不到 vcpkg toolchain 文件

**错误信息**：
```
Could not find toolchain file: "/windows/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

**解决方案**：
检查 `builds/cmake/CMakePresetsUser.json` 文件中的 `toolchainFile` 路径是否正确。

### 问题1.1：CMake 在配置依赖时无错误信息退出

**现象**：
配置过程停在查找 Git、PNG、ZLIB 或其他依赖附近，退出码类似 `-1073740791`，但没有普通 CMake 错误。

**解决方案**：
从 `C:\Player` 运行配置和编译，不要直接从 `C:\Users\旻\Documents\GitHub\Player` 运行。当前工具链在中文用户名路径下不稳定。

### 问题2：找不到 liblcf

**错误信息**：
```
Could not find a package configuration file provided by "liblcf"
```

**解决方案**：
确保 `CMakePresetsUser.json` 中的 `PLAYER_BUILD_LIBLCF` 设置为 `"ON"`。

### 问题3：编译错误

如果遇到编译错误，尝试：
1. 清理 build 目录后重新编译
2. 确保 Visual Studio 2022 已正确安装
3. 检查是否有未提交的代码修改导致编译失败

### 问题4：链接时报 `__imp_fgets`、`__imp_fdopen` 等无法解析

**原因**：
工程使用了静态 CRT，但本地 vcpkg 依赖来自 `x64-windows`，使用动态 CRT。

**解决方案**：
使用 `windows-x64-vs2022-debug-local` preset。该 preset 已设置：

```json
"VCPKG_TARGET_TRIPLET": "x64-windows",
"VCPKG_CRT_LINKAGE": "dynamic"
```

### 问题5：复制到游戏目录后启动提示缺少 DLL

**原因**：
使用了 Debug 动态构建，或误用了 `x64-windows` 动态 triplet。动态构建的 `Player.exe` 依赖输出目录里的 SDL2、libpng、zlib、fmt 等 DLL。

**解决方案**：
发布给游戏目录使用 `windows-x64-vs2022-release-local`。该 preset 使用 `x64-windows-static`，产物是单文件 `Player.exe`。Debug 动态构建只用于本地调试。

## 更新依赖库

如果需要更新当前本地 vcpkg 依赖库：

```cmd
cd C:\vcpkg
.\vcpkg.exe upgrade --no-dry-run
```

**注意**：Debug local preset 使用 `x64-windows`。Release local preset 使用 `C:\buildscripts\windows\vcpkg` 中的 `x64-windows-static`。

## 使用 Visual Studio IDE 编译

如果更喜欢使用 Visual Studio IDE：

1. 配置项目（同上）
2. 打开解决方案文件：
   ```
   build\windows-x64-vs2022-debug-local\EasyRPG_Player.sln
   ```
3. 在 Visual Studio 中选择 Debug 配置
4. 点击"生成" → "生成解决方案"

## 参考文档

- [官方编译文档](docs/BUILDING.md)
- [buildscripts Windows 说明](../buildscripts/windows/README.md)
- [EasyRPG 官网](https://easyrpg.org/)

---

**最后更新**: 2026-05-18
**编译器版本**: MSVC 19.44.35222.0 (Visual Studio 2022)
