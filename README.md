<p align="center">
  <img src="src/redefine.ico" width="96" height="96" alt="Redefine 应用图标" />
</p>

<h1 align="center">Redefine</h1>

<p align="center">为 Windows 文件资源管理器中的文件和文件夹添加可追踪的 Markdown 备注。</p>

<p align="center">
  <a href="CMakeLists.txt">
    <img src="https://img.shields.io/badge/version-0.7.1-2563EB?style=flat-square" alt="版本 0.7.1" />
  </a>
  <img src="https://img.shields.io/badge/Kimi-Code-111111?style=flat-square" alt="Kimi Code" />
</p>

## 功能特性

- **原生轻量**：基于 C++20、Win32、Direct2D 和 DirectWrite，不依赖 Qt、.NET 或 WebView2，不需要管理员权限，也不注入 Explorer。
- **Markdown 备注**：支持源码编辑、基础语法预览和自动保存；备注框可左右吸附、收起及切换编辑或预览。
- **对象身份追踪**：使用卷序列号与 File ID 绑定文件系统对象，同卷移动后仍可找到原备注。
- **无主备注管理**：删除、重命名或主动解绑后保留备注，支持普通文本和 RE2 正则实时搜索。
- **显式重新绑定**：将单个文件或文件夹拖到目标无主卡片，拖动经过时显示高亮反馈，验证通过后完成绑定。
- **本地存储**：数据保存在本地 SQLite 数据库中，不上传备注或文件路径。

## 使用方式

1. 运行 `redefine.exe`，系统托盘会显示 ℝ 图标。
2. 在文件资源管理器中选中一个文件或文件夹；没有选中项时，编辑当前文件夹的备注。
3. 在悬浮备注框中输入 Markdown，内容会自动保存。鼠标进入备注框后可使用预览、换侧、收起、无主备注管理和解除绑定按钮。
4. 右键单击托盘图标，可执行相同的主要操作，包括打开无主备注管理器和解除当前备注绑定。
5. 在无主备注管理器中搜索目标备注，再将一个同类型、尚未绑定备注的对象拖到对应卡片上。

解除绑定不会删除备注，只会将其转为无主状态。重新绑定文件夹备注时，不会自动级联恢复后代备注。

## 文件追踪规则

| 操作 | 备注行为 |
| --- | --- |
| 同卷移动 | 根据 File ID 保留绑定 |
| 同卷重命名 | 原备注转为无主，重命名后的对象不继承备注 |
| 同卷或跨卷复制 | 副本不继承原备注 |
| 跨卷移动 | 源对象删除后，原备注转为无主 |
| 删除或移入回收站 | 备注保留并转为无主，不自动绑定还原对象 |
| 主动解除绑定 | 当前对象恢复未绑定，原备注转为无主 |
| 删除后创建同名对象 | 新对象不会继承原备注 |

## 构建

### 环境要求

- Windows 10/11 x64
- Visual Studio 2022，包含“使用 C++ 的桌面开发”工作负载和 Windows SDK
- CMake 3.30 或更高版本
- Git 和可用的网络连接，用于首次获取第三方依赖

### 编译 Release

```bash
cmake --preset windows-x64
cmake --build out/build/windows-x64 --config Release
```

生成的程序位于：

```text
out/build/windows-x64/Release/redefine.exe
```

第三方依赖通过 CMake FetchContent 获取并静态链接，包括 md4c、RE2、SQLite、Abseil 和 WIL。

## 数据存储

备注数据库位于：

```text
%LOCALAPPDATA%\Redefine\notes.db
```

`last_path` 仅用于展示和辅助校验，不作为对象身份失效后的自动匹配依据。详细设计见 [DESIGN.md](DESIGN.md)。

## 许可证

本项目基于 [MIT License](LICENSE) 发布。
