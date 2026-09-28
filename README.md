# Fall_And_Boom

Windows 桌面小玩具：图片从屏幕顶端掉落，落到其它窗口的顶边上会停住并跟着窗口一起动；
被快速拖动的窗口"创飞"，引信到点炸成碎片，爆炸再把附近的图片一起掀飞。

**不用安装任何东西** —— 下载、解压、双击 `FallingImages.exe` 就能跑。

## 快速开始

1. 到 [Releases](https://github.com/qwaszxopklnmso/Fall_And_Boom/releases/latest) 下载 `FallingImages_vX.Y.Z.zip`
2. 解压到**任意可写目录**（别放 `Program Files`，那里写不进配置），双击 `FallingImages.exe`
3. 图片自己会开始掉。想用你自己的图，就把图片丢进 `images\` 的对应子文件夹
4. 退出：**`Ctrl + Alt + Q`**

> 图片格式：`png` / `jpg` / `bmp` / `gif` 到哪都能解。
> **别用 `webp`** —— Windows 不自带 WebP 解码器，干净系统上会解不出来（图片变少，全挂了会弹窗）。

## 三类图片

图片放在 exe 同目录的 `images\` 下，**按子文件夹分类，只扫一层**：

| 文件夹 | 类别 | 行为 |
|---|---|---|
| `images\blocks\` | **方块** | 不会自己爆炸；停稳一会儿淡出消失。被爆炸波及时炸成碎片 |
| `images\explosives\` | **爆炸物** | 有引信，5 秒后自己爆炸，把半径内的东西炸飞 |
| `images\buildings\` | **建筑** | **地形**：不掉、不被推、不被掀飞，可以当地板 / 墙壁 / 天花板。**只能手动放**，挨满 3 次爆炸后碎掉 |

`images\` 根目录下直接放的图片按**爆炸物**处理（兼容老版本）。
`buildings\` 可以整个不要 —— 那就按 `/` 没反应，其余照常。

## 操作

| 操作 | 默认键 | 说明 |
|---|---|---|
| 放一个方块 | `,` | 在**鼠标位置**生成，不用等自动掉落 |
| 放一个爆炸物 | `.` | 同上 |
| 放一块建筑 | `/` | 同上，建筑只能这样放 |
| 退出 | `Ctrl + Alt + Q` | 被占用时自动依次改用 `Ctrl+Alt+W` / `X` / `F12`、`Ctrl+Shift+Q` |

按键全部可以在 `config.ini` 里改；三个手动键还能**各关各的**，
全关掉时程序连键盘钩子都不装。

## 配置（`config.ini`）

**所有可调参数都在 exe 同目录的 `config.ini` 里，改完重启程序生效，不用重新编译。**

- 文件不存在时，程序会按内置默认值自动生成一份，每个参数都带中文注释
- **已有的文件永远不会被覆盖**，只会被读取
- 分四段：通用 / 方块 / 爆炸物 / 建筑
- 删掉某一行 = 该参数用内置默认值；写错的值只影响它自己，启动时弹一次窗列出问题

常用几个：

| 键 | 默认 | 说明 |
|---|---|---|
| `DROPTIME_MIN` / `DROPTIME_MAX` | 0.2 / 1.3 | 两次自动生成的间隔（秒），掉落快慢主要看它 |
| `MAX_FALLING` | 120 | 同屏下落中的图片上限（方块 + 爆炸物合计，建筑不占） |
| `BLOCK_SIZE` / `EXPLOSIVE_SIZE` / `BUILDING_SIZE` | 48 | 三类图片各自的显示边长（像素） |
| `EXPLOSIVE_LIFE_SEC` | 5 | 引信长度，几秒后爆炸 |
| `EXPLOSION_RADIUS` | 310 | 爆炸冲击波半径（像素） |
| `AUTO_SPAWN_ENABLED` / `MANUAL_SPAWN_ENABLED` | `true` / `true` | 自动掉落 / 手动按键的总开关 |

**全部参数一览表**、写法和边界规则都在 [NOTES.md](NOTES.md) 的「配置」一节。

### 预设（换手感）

`presets\` 里带了两个调好的配置，想换个感觉不用自己一项项改：

| 预设 | 一句话 |
|---|---|
| `presets\config_fragile.ini` **易碎品** | 满屏连锁炸，谁也留不住 |
| `presets\config_fast.ini` **快速/高配置** | 小颗粒满天掉，地上很快铺满碎渣 |

用法：复制到上一级目录（exe 旁边），改名成 `config.ini` 覆盖现有的，重启程序。
不想下整个压缩包的话，也可以单独下载：

- [config_fragile.ini（易碎品）](https://github.com/qwaszxopklnmso/Fall_And_Boom/releases/latest/download/config_fragile.ini)
- [config_fast.ini（快速/高配置）](https://github.com/qwaszxopklnmso/Fall_And_Boom/releases/latest/download/config_fast.ini)

## 主要特性

- 图片会**落在其它程序的窗口顶边上**，并跟着窗口水平 + 垂直移动（拖多快都跟得上）
- 被快速拖动的窗口"创飞"，抛物线飞出去，撞到窗口侧面会正常弹开，不穿模
- 爆炸物引信到点爆炸，冲击波把半径内的图片一起炸飞；爆炸物自己和被炸到的方块变成碎片
- **方块不连锁**：被炸掉的方块只是碎掉，不会再放冲击波
- 全屏 / 无边框全屏窗口自动忽略；被遮住的顶边不算平台（不会"悬空停住"）
- 开始菜单 / 搜索 / 通知中心这类 shell 浮层也能碰撞
- 光标完全穿透，不抢焦点，不影响你正常干活

实现细节 —— 碰撞解算、z-band 窗口、遮挡判定、踩过的坑 —— 全在 [NOTES.md](NOTES.md)。

## 常见问题

| 现象 | 原因 / 怎么办 |
|---|---|
| 双击没反应，或弹出"没有找到可用图片！" | `images\` 和三个子文件夹没解压出来，或者图片格式解不了（多半是 `webp`） |
| 图片卡在屏幕最上方不往下掉 | 没有，这是设计：顶边贴着屏幕上沿的窗口接不住图片，会直接放它过去 |
| 杀毒软件提示键盘钩子 | 三个手动键用的是 `WH_KEYBOARD_LL` 旁听（原样放行）。不联网、不写注册表、不改系统 |
| 配置改了没用 | 要**重启程序**；另外确认改的是 exe 旁边那份 `config.ini` |
| 在 `Program Files` 里跑，配置存不下来 | 只读目录写不进去，程序不报错，直接用内置默认值。换个目录解压即可 |
| 系统要求 | Windows 10 / 11 **x64**。静态链接（`/MT`），**不需要装 VC++ 运行库**，不需要管理员权限 |

## 编译

需要 Visual Studio 的「使用 C++ 的桌面开发」工作负载。

```
build.bat          # 编译
build.bat run      # 编译并直接运行
build.bat clean    # 清理编译产物
```

手动编译：

```
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE main.cpp ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib ole32.lib ^
         windowscodecs.lib dwmapi.lib
```

> `/utf-8` 必须加：源码是 UTF-8，不加会在 GBK 代码页下报 `C2001`。
> **不要用 `/MTd`**：那是调试版运行库，依赖 `ucrtbased.dll`，反而跑不起来。

## 版本号约定

目前是 `1.x.y`：**一个 `y` 版本号 = 一次已推送的修复或小功能**，
每个版本都能在[提交记录](https://github.com/qwaszxopklnmso/Fall_And_Boom/commits/main)和 tag 里查到。

## AI 生成声明

本项目的代码与文字 —— `main.cpp`、`build.bat`、README —— **均由 AI 生成**；
qwaszxopklnm 负责给图片资源、提需求、测试与验收。代码因此可能带一些典型痕迹
（注释偏多、防御性分支偏多、命名风格不统一），欢迎提 issue 指出。

引用本项目时以 [LICENSE](LICENSE) 为准；AI 生成这一事实不改变授权方式。

## 许可证

**MIT License** — Copyright (c) 2026 **qwaszxopklnm**

完整文本见 [LICENSE](LICENSE)。简单说：随便用、随便改、随便发，保留版权声明即可，作者不承担任何责任。
