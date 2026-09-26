# Fall_And_Boom

Windows 桌面小玩具：图片从屏幕顶端掉落，落到窗口顶边会停住并跟着窗口移动，
被快速拖动的窗口"创飞"，存活 5 秒后炸成碎片，爆炸还会把附近的图片一起炸飞。
被以特别快的速度撞击时，引信会缩短 2.5 秒。

## 特性

- Win32 + GDI+ 单窗口合成，整屏 layered window 一次提交
- 图片格式：`png` / `jpg` / `jpeg` / `bmp` / `gif` / `webp` / `tif` / `ico`
  - 优先用 GDI+ 解码；遇到 WebP、JPEG XL 等它不认识的格式自动回退到 WIC
  - 支持多帧 GIF 动画
- 把其它程序的窗口顶边当作平台；**全屏 / 无边框全屏窗口会被忽略**
- **被其它窗口遮住的那段顶边不会被当成平台**，不会出现"悬空停住"
- 停住的图片会跟随窗口做**水平和垂直**移动（窗口往上拖也会被顶上去）
- 被撞飞的图片会与**窗口侧面**正常碰撞推挤，不会穿模
- 爆炸产生冲击波，把半径内的其它图片一起炸飞（被炸飞不变浅）
- 爆炸前 3 秒开始闪烁，0.6 秒一个周期
- 光标完全穿透，不抢焦点

## 编译

双击 `build.bat`，或在命令行：

```
build.bat          # 编译
build.bat run      # 编译并直接运行
build.bat clean    # 清理编译产物
```

需要 Visual Studio 的「使用 C++ 的桌面开发」工作负载。
脚本会依次通过 `vswhere` → 常见安装路径 → 扫盘来定位 `vcvars64.bat`。

手动编译：

```
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE main.cpp ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib ole32.lib windowscodecs.lib
```

> `/utf-8` **必须加**：源码是 UTF-8，不加的话中文字符串会在 GBK 代码页下报
> `C2001: 字符串字面量中的换行符`。
> `user32/gdi32/gdiplus/windowscodecs` 也必须显式写出，裸 `cl` 不会自动链接。

## 图片

把图片放进 exe 同目录的 `images\` 文件夹（按文件名排序，最多取前 10 个）。

每张图片统一存活 **5 秒**，然后爆炸成 4×4 的碎片。
被以特别快的速度撞击（相对速度 ≥ 800 像素/秒）会**提前 2.5 秒**爆炸。

爆炸前 3 秒开始闪烁。所以满血时只在生命最后 3 秒闪；
而挨过一次重击后剩余寿命变成 2.5 秒，会立刻开始闪。

> WebP 依赖系统安装的解码器（Win10/11 通常自带）。
> 若某张图片解不了，程序不会崩溃，只会在全部图片都失败时弹窗列出文件名。

## 可调参数

都在 `main.cpp` 顶部的「可调参数」区，改完重新编译即可：

| 常量 | 默认 | 说明 |
|---|---|---|
| `SPRITE_LIFE_SEC` | 5.0 | 统一爆炸时间（秒） |
| `FAST_HIT_SPEED` | 800.0 | 判定"特别快的撞击"的相对速度阈值（像素/秒） |
| `FAST_HIT_LIFE_LOSS` | 2.5 | 高速撞击扣掉的寿命（秒） |
| `EXPLODE_WARN_SEC` | 3.0 | 爆炸前多少秒开始闪烁 |
| `EXPLODE_BLINK_PERIOD` | 0.6 | 闪烁周期（秒） |
| `HIT_FLASH_SEC` / `HIT_DIM` | 0.40 / 0.50 | 被撞后闪光的时长 / 最浅 alpha |
| `SPRITE_SIZE` | 48 | 图片缩放到的边长（像素） |
| `MAX_IMAGES` | 10 | 最多加载几张图片 |
| `MAX_FALLING` / `MAX_TOTAL` | 20 / 220 | 同屏下落中 / 精灵总数上限 |
| `EXPLOSION_RADIUS` | 150.0 | 爆炸冲击波半径（像素） |
| `FALL_SPEED_MIN` / `_MAX` | 260 / 600 | 下落速度范围（像素/秒） |
| `WINDOW_SCAN_SEC` | 0.08 | 窗口位置采样间隔（秒） |

## 退出

`Ctrl + Alt + Q`。

若该组合被其它程序占用，会自动依次改用 `Ctrl+Alt+W`、`Ctrl+Alt+X`、
`Ctrl+Alt+F12`、`Ctrl+Shift+Q`；全部失败时会弹窗提示改用任务管理器结束。
