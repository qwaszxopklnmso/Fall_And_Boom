// ============================================================
//  Falling Images  -  Win32 + GDI+ + WIC   (单窗口合成版)
//  编译: 双击 build.bat  (推荐)
//        或手动:
//        cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE main.cpp ^
//           /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib ole32.lib ^
//                 windowscodecs.lib dwmapi.lib
//        注意: /utf-8 必须加, 否则中文字符串在 GBK 代码页下会编译报错;
//              user32/gdi32/gdiplus/windowscodecs/dwmapi 也必须显式写出,
//              裸 cl 不会自动链接.
//  退出: Ctrl+Alt+Q
// ------------------------------------------------------------
//  Copyright (c) 2026 qwaszxopklnm
//  SPDX-License-Identifier: MIT        (完整协议见 LICENSE)
//  本文件代码由 AI 生成, 作者负责提需求、测试与验收.
// ============================================================

#define NOMINMAX

#include <windows.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <gdiplus.h>

#include <vector>
#include <memory>
#include <string>
#include <random>
#include <cmath>
#include <algorithm>
#include <cwctype>
#include <cstring>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

// ------------------------------------------------------------
// 可调参数
// ------------------------------------------------------------
static const wchar_t* kOverlayClass = L"FallingImgOverlay";
static const wchar_t* kMasterClass  = L"FallingImgMaster";

static const int   MAX_IMAGES      = 10;
static const int   MAX_FALLING     = 20;
static const int   MAX_TOTAL       = 220;
static const int   DEBRIS_COLS     = 4;
static const int   DEBRIS_ROWS     = 4;
static const float FALL_SPEED_MIN  = 405.0f;
static const float FALL_SPEED_MAX  = 600.0f;
static const int   SPRITE_SIZE     = 48;
static const float WINDOW_SCAN_SEC = 0.08f;   // 窗口扫描间隔
static const float FOLLOW_SANITY_VX = 20000.0f; // 跟随窗口时的荒谬值上限(挡矩形抖动)
static const float SWEEP_MIN_VX    = 250.0f;  // 认定为"创飞"的窗口速度阈值

// ---- 生成方式开关 ----
// 两个都开 = 平时自动随机下落, 想手动补一张就按 "."。
// 关掉 AUTO  = 屏幕上一直干干净净, 只有按 "." 才会出现图片。
// 关掉 MANUAL= 只能等自动随机下落, 连键盘钩子都不会装。
// 两个都关   = 什么都不会生成(退出热键仍然有效)。
static const bool  AUTO_SPAWN_ENABLED   = true;   // ★ 自动随机下落生成
static const bool  MANUAL_SPAWN_ENABLED = true;   // ★ 按 "." 在鼠标位置生成

// ---- 手动生成 ----
// 按这个键, 在鼠标当前位置生成一张随机图片。
// 用低级键盘钩子而不是 RegisterHotKey: 注册成热键会把这个键从所有程序那里
// 抢走(打字、输入小数点就全废了), 钩子只是旁听, 按键照样传给别的程序。
static const UINT  SPAWN_KEY_VK    = VK_OEM_PERIOD;   // 主键盘区的 "."

static const float EXPLOSION_RADIUS    = 320.0f;  // 爆炸冲击波半径(像素)
static const float EXPLOSION_POWER_MIN = 500.0f;  // 冲击波初速(范围边缘)
static const float EXPLOSION_POWER_MAX = 900.0f;  // 冲击波初速(爆心附近)
static const float EXPLOSION_GRAVITY   = 1400.0f; // 被炸飞之后的重力

// ---- 碰撞反馈: 闪一下再恢复(不再按剩余寿命长时间发灰) ----
static const float HIT_FLASH_SEC = 0.40f;   // "闪一下"的总时长(秒)
static const float HIT_DIM       = 0.50f;   // 闪到最浅时的 alpha

// ---- 临爆闪烁 ----
static const float EXPLODE_WARN_SEC     = 3.0f;   // ★ 爆炸前多少秒开始闪烁, 改这里
static const float EXPLODE_BLINK_PERIOD = 0.6f;   // ★ 闪烁周期(秒)
static const float EXPLODE_BLINK_MIN    = 0.45f;  // 闪烁时最浅的 alpha

// ---- 寿命 / 高速撞击伤害 ----
static const float SPRITE_LIFE_SEC    = 5.0f;   // ★ 统一爆炸时间(秒, 所有图片一样)
static const float FAST_HIT_SPEED     = 1650.0f; // ★ 相对速度超过它算"特别快的撞击"(像素/秒)
                                                //   定在 800 是因为自由落体上限是 600,
                                                //   所以普通下落互撞永远不会触发, 只有被撞飞/炸飞后才够
static const float FAST_HIT_LIFE_LOSS = 2.5f;   // ★ 高速撞击扣掉的寿命(秒)

// ------------------------------------------------------------
// 全局
// ------------------------------------------------------------
static HINSTANCE   g_hInst        = nullptr;
static ULONG_PTR   g_gdiplusToken = 0;
static std::mt19937 g_rng{ std::random_device{}() };

static int  g_workX = 0, g_workY = 0;
static int  g_screenW = 0, g_screenH = 0;
static HDC  g_screenDC = nullptr;

static HWND      g_overlayHwnd = nullptr;
static HDC       g_composeDC   = nullptr;
static HBITMAP   g_composeBm   = nullptr;
static HGDIOBJ   g_composeOld  = nullptr;
static void*     g_composeBits = nullptr;

static IWICImagingFactory* g_wicFactory = nullptr;

struct Sprite;
static std::vector<std::unique_ptr<Sprite>> g_sprites;

static float g_spawnTimer = 0.0f;
static float g_scanTimer  = 0.0f;
static float g_scanAccum  = 0.0f;    // 距上次窗口扫描累计的真实时间
static float g_scanDt     = 0.1f;    // 上次扫描的真实间隔(用于换算窗口移动速度)
static float g_displayTimer = 0.0f;  // 分辨率/工作区变化检查节流

// ------------------------------------------------------------
// 小工具
// ------------------------------------------------------------
static float RandF(float a, float b)
{
    return std::uniform_real_distribution<float>(a, b)(g_rng);
}
static int RandI(int a, int b)
{
    if (b < a) b = a;
    return std::uniform_int_distribution<int>(a, b)(g_rng);
}

static std::wstring FileNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

// ------------------------------------------------------------
// 顶层窗口扫描（带 Z 序遮挡过滤）
// ------------------------------------------------------------
struct WindowInfo {
    HWND hwnd = nullptr;
    RECT rect{};
    RECT prevRect{};
    bool hasPrev = false;
    // 能否当作"平台"。以下两种窗口不能:
    //   1) 全屏 / 无边框全屏 —— 铺满显示器, 没有有意义的顶边
    //   2) 顶边在屏幕上方 —— 例如最大化窗口(GetWindowRect.Top 通常是 -7),
    //      图片停上去会被摆到 y<0, 直接看不见
    // 但它们**都必须继续参与遮挡判定**: 一旦丢掉, 它们背后被完全遮盖的窗口
    // 就会被误判成可见平台, 出现穿模的物理碰撞。
    bool platform = true;
    // Z 序更高的窗口压在本窗口上的矩形(交集)。用来判断"顶边哪一段真的看得见"
    std::vector<RECT> occluders;
};

static std::vector<WindowInfo> g_windowList;
// 本次扫描时的前台窗口。有两个用途:
//   · z-band 窗口去重(见下);
//   · 遮挡判定里"用户正在操作的那个窗口"要特殊对待。
static HWND g_foregroundHwnd = nullptr;
// 本次扫描里由 GetForegroundWindow 补进来的 z-band 窗口(EnumWindows 看不到的),
// 仅用于去重: 万一将来某个系统版本又开始把它枚举出来, 不至于变成两条记录。
static HWND g_bandHwnd = nullptr;

// 把一个窗口按统一规则判定, 通过则追加到 list 末尾。
// 遮挡判定假设 "list 里已有的都是 Z 序更高的窗口", 所以调用顺序必须自上而下。

// 这个窗口算不算"遮挡"。完全按 Z 序算是不行的:
// Win11 的壳层浮层(开始菜单、通知中心、贴靠布局预览、输入法候选窗…)都是
// **顶层合成层**, 用的是 WS_EX_NOREDIRECTIONBITMAP(有时再加 WS_EX_LAYERED)。
// 它们会浮在正在被拖动的窗口上方, 一旦当成遮挡, 被拖窗口的**上缘就整段作废**:
// 现象就是"拖动时上缘接不住、也推不动, 松开鼠标立刻恢复"(浮层消失了)。
// 侧缘不走遮挡判定, 所以一直正常 —— 用户实测正是如此。
// 真正的"另一个实心窗口压在上面"(浏览器/编辑器/资源管理器)不受影响, 照旧遮挡。
static bool BlocksAsOccluder(HWND hwnd)
{
    if (hwnd == g_foregroundHwnd) return true;        // 前台浮层(开始菜单…)照旧遮挡
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_NOREDIRECTIONBITMAP) return false; // 合成浮层
    if (ex & WS_EX_LAYERED)             return false; // 半透明层: 看得见后面, 不算压住
    return true;
}

static bool AcceptWindow(HWND hwnd, std::vector<WindowInfo>* list)
{
    if (!IsWindowVisible(hwnd)) return false;
    if (IsIconic(hwnd))         return false;

    // ---- DWM 隐身(cloaked)窗口必须当成不可见 ----
    // 对这类窗口 IsWindowVisible 仍然返回 TRUE, 但它根本没被渲染。
    // 典型: 其它虚拟桌面上的窗口、被 shell 挂起的 UWP 表面。
    // 实测本机: "Windows 输入体验"(Windows.UI.Core.CoreWindow) 是全屏(0,0,2560,1440)
    // 且 cloaked=2 —— 不排除它, 它就会把后面所有窗口判成"被完全遮盖",
    // 于是那些窗口全都当不成平台。这比 TOOLWINDOW 那条过滤器影响大得多。
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0)
        return false;

    // 注意: 这里以前有一句 `if (extStyle & WS_EX_TOOLWINDOW) return TRUE;`,
    // 会把整个窗口丢掉。但 shell 的浮层几乎全是 WS_EX_TOOLWINDOW ——
    // 开始菜单、搜索、任务视图(Win+Tab)、音量/通知中心、任务栏、桌面……
    // 丢掉它们的后果是: 既不能遮挡, 背后被它们盖住的窗口又会被误判成可见平台。
    // 这个样式只表示"不进 Alt+Tab / 任务栏", 并不代表窗口不可见, 所以不能拿来排除。

    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    if (wcscmp(cls, kOverlayClass) == 0) return false;
    if (wcscmp(cls, kMasterClass)  == 0) return false;
    if (wcscmp(cls, L"Progman")    == 0) return false;
    if (wcscmp(cls, L"WorkerW")    == 0) return false;
    if (wcscmp(cls, L"Shell_TrayWnd") == 0) return false;

    RECT r;
    if (!GetWindowRect(hwnd, &r)) return false;
    if (r.right <= r.left || r.bottom <= r.top) return false;
    // 只有"完全落在工作区上下方之外"的窗口才能直接丢掉。
    // 注意不能写成 r.top < 0: 最大化窗口的 GetWindowRect 会把不可见的
    // DWM 调整边框算进去(Top 常为 -7), 那样会把正在铺满屏幕的窗口丢掉,
    // 导致它背后被完全遮盖的窗口重新获得物理碰撞。
    if (r.top >= g_screenH || r.bottom <= 0) return false;

    // ---- 全屏 / 无边框窗口化全屏 判定 ----
    // 这类窗口铺满整个显示器。它仍然要参与下面的"遮挡过滤"，
    // 否则它背后的窗口会失去遮挡依据、被误判成可见平台；
    // 但它自己绝不能当平台，否则图片会全部堆在屏幕最顶端。
    bool fullscreen = false;
    {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (mon && GetMonitorInfoW(mon, &mi)) {
            const RECT& m = mi.rcMonitor;
            const int tol = 2;
            fullscreen = (r.left  <= m.left  + tol && r.top    <= m.top    + tol &&
                          r.right >= m.right - tol && r.bottom >= m.bottom - tol);
        }
    }

    // ---- 收集被 Z 序更高的窗口压住的区域 ----
    // 注意：完全被盖住的窗口直接丢弃；只被盖住一部分的窗口要保留，
    //       但要记住被盖住的是哪一块，否则图片会停在看不见的顶边上（悬空）。
    std::vector<RECT> occ;
    for (const auto& wi : *list) {
        if (!BlocksAsOccluder(wi.hwnd)) continue;
        RECT inter;
        if (!IntersectRect(&inter, &wi.rect, &r)) continue;
        if (inter.left   <= r.left  && inter.right  >= r.right &&
            inter.top    <= r.top   && inter.bottom >= r.bottom) {
            return false;                      // 完全被覆盖, 直接跳过
        }
        occ.push_back(inter);
    }

    WindowInfo wi;
    wi.hwnd      = hwnd;
    wi.rect      = r;
    wi.prevRect  = r;
    wi.hasPrev   = false;
    // ---- 顶边能不能当平台 ----
    // 这里只用"顶边上面放得下整张图片"(r.top >= SPRITE_SIZE)做一个粗判,
    // 主要给"创飞"用。落点/停靠的真正判定是 TopEdgeFits(), 它按**精灵自己的
    // 高度**算: 48px 的图片需要 top >= 48, 12px 的碎片只要 top >= 12。
    //
    // 曾经试过把门槛放宽到"顶边贴着屏幕上沿也算", 结果是被拖到屏幕上沿的窗口
    // (以及拖动时系统临时冒出来的全宽浮层)全变成 y=0 的架子, 新生成的图片
    // 一出来就卡在屏幕最上方不下落。所以这里不能放宽 —— 上面放不下就是放不下,
    // 图片会从这种窗口前面穿过去(和原来的行为一致), 但绝不会被瞬移到别处。
    //
    // 仍然排除: 全屏窗口 / 最大化窗口(顶边在 -7 左右) / 太小的窗口
    // (屏幕顶端几像素高的触控条、1x1 辅助窗口)。
    const bool maximized = (IsZoomed(hwnd) != FALSE);
    const bool bigEnough = (r.right - r.left) >= SPRITE_SIZE &&
                           (r.bottom - r.top)  >= SPRITE_SIZE;
    wi.platform  = (!fullscreen && !maximized && bigEnough && r.top >= SPRITE_SIZE);
    wi.occluders = std::move(occ);
    list->push_back(std::move(wi));
    return true;
}

// EnumWindows 按 Z 序从顶层到底层枚举
static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    if (hwnd == g_bandHwnd) return TRUE;      // 已经在 ScanWindows 里补过了
    AcceptWindow(hwnd, reinterpret_cast<std::vector<WindowInfo>*>(lParam));
    return TRUE;
}

// EnumWindows 只能看见默认 z-band 上的窗口。
// 检查 target 是否能被 EnumWindows 枚举到(命中即提前结束)。
struct ExistsCtx { HWND target; bool found; };
static BOOL CALLBACK ExistsProc(HWND hwnd, LPARAM lParam)
{
    auto* c = reinterpret_cast<ExistsCtx*>(lParam);
    if (hwnd == c->target) { c->found = true; return FALSE; }
    return TRUE;
}

// 判断窗口顶边上横坐标 cx 这一点是否真的可见。
// 被更高 Z 序窗口压住的部分不能当平台，否则图片会停在看不见的边上，看起来像悬空。
static bool TopEdgeVisibleAt(const WindowInfo& wv, float cx)
{
    if (wv.occluders.empty()) return true;
    // 用户正在操作的那个窗口(拖动中的那个), 上缘永远作废不得 ——
    // 拖动时壳层浮层随时可能飘在它上方, 一旦作废就是"拖着拖着上缘就接不住东西了"。
    if (wv.hwnd == g_foregroundHwnd) return true;
    const LONG px = (LONG)std::lround(cx);
    const LONG py = wv.rect.top + 1;
    for (const RECT& o : wv.occluders) {
        if (px >= o.left && px < o.right && py >= o.top && py < o.bottom)
            return false;
    }
    return true;
}

// 精灵的底边这一帧有没有"越过"窗口顶边(越过就该落在它上面)。
//
// 不能直接用 prevBottom <= wr.top: 窗口矩形每 WINDOW_SCAN_SEC 才采样一次,
// 用户快速往上拖窗口时一帧能跳几十像素, 顶边变小了之后
// "prevBottom <= 顶边" 反而永远不成立(底边早就在新顶边下面了) ——
// 于是窗口直接从图片身上穿过去, 这就是"窗口向上推会穿"的根因。
//
// 所以参照边取 max(上一拍的顶边, 这一拍的顶边):
//   窗口静止 / 往下走 -> 参照边 == 当前顶边, 判据和原来一模一样;
//   窗口往上走     -> 参照边 == 上一拍的顶边, "原来在顶边之上、这一拍被
//                    吞进来" 就能被判出来, 图片会被顶到新的顶边上。
static bool SweptDownPastTop(const WindowInfo& wv, float prevBottom, float newBottom)
{
    const float curTop = (float)wv.rect.top;
    const float refTop = (wv.hasPrev && (float)wv.prevRect.top > curTop)
                       ? (float)wv.prevRect.top
                       : curTop;
    return (newBottom > curTop && prevBottom <= refTop);
}

// 窗口顶边"往上顶"精灵: 窗口在上升, 顶边这一拍从精灵身体里扫过去。
//
// SweptDownPastTop 的前提是"精灵底边上一拍还在顶边之上"。可精灵常常是**停在
// 屏幕底部(或别的平台上)不动的**: 窗口顶边从下面升上来时, 第一拍就可能已经越过
// 精灵底边(窗口上一拍还在工作区外 / 刚进列表, 没有记录), 之后 prevBottom 永远
// 大于参照顶边 —— 判据一直不成立, 顶边直接从精灵身上升过去。
// 用户看到的就是"接不住也推不动": 顶边扫过图片, 图片纹丝不动。
//
// 所以按扫掠本身判: 顶边这一拍扫过的竖直线段 [curTop, prevTop] 与精灵身体
// [prevBottom - h, prevBottom] 相交, 并且精灵底边现在还在新顶边之下, 就把它抬到
// 新顶边上。顶边本来就远在精灵上方的(图片整个在窗口肚子里)不算 —— 那是瞬移。
static bool RisingTopLifts(const WindowInfo& wv, int h, float prevBottom, float newBottom)
{
    if (!wv.hasPrev) return false;
    const float curTop  = (float)wv.rect.top;
    const float prevTop = (float)wv.prevRect.top;
    if (curTop >= prevTop) return false;            // 没在上升
    if (newBottom <= curTop) return false;          // 精灵底边还在顶边之上
    return (prevBottom - (float)h) <= prevTop;      // 顶边这一拍确实扫进了精灵身体
}

// 这条顶边能不能给"高 h 的精灵"当落脚点 —— 顶边上面得放得下它。
//
// 刻意按**精灵自己的高度**算, 而不是用全局的 wv.platform:
//   · 48px 的图片需要 top >= 48; 12px 的碎片只要 top >= 12。
//   · 顶边贴着屏幕上沿(top < h)的窗口放不下, 就老老实实让它穿过去,
//     绝不能把精灵摆到 y = top - h < 0 再钳到 0 —— 那就是"图片卡在屏幕最上方
//     不下落"。全屏/最大化窗口(top = -7)也因此自动出局, 不必再单独判。
static bool TopEdgeFits(const WindowInfo& wv, int h)
{
    return wv.rect.top >= h;
}

// 同上, 再加上"这一帧确实是从上方越过这条顶边进来的"。
// SweptDownPastTop 则只管穿越本身, 用来避免把图片甩到窗口下方/侧方:
// 就算这窗口放不下(顶边贴着屏幕上沿、太小、最大化…), 也绝不能因为它
// 而把图片瞬移到别处 —— 宁可让它按物理落出去。
// 两条判据取或: 精灵掉下来接住, 或者窗口顶边升上来把精灵顶起来。
static bool CrossesTopEdge(const WindowInfo& wv, int h, float prevBottom, float newBottom)
{
    if (!TopEdgeFits(wv, h)) return false;
    return SweptDownPastTop(wv, prevBottom, newBottom) ||
           RisingTopLifts(wv, h, prevBottom, newBottom);
}

// 在所有够格的窗口里挑出"这一帧该落在它顶边上"的那一个(没有则 nullptr)。
// x/w/h 是精灵的横向范围与高度, prevBottom/newBottom 是底边在本帧位移前后的位置。
//
// 多个候选时取**顶边最高**的那个。理由:
//   1) 正常下落时, 先碰到的当然是最高的那块平台;
//   2) 窗口上拖把精灵吞进来时, 唯一不留下穿模的解也是最高的那条顶边 ——
//      如果退而落到原来那块(更低的)平台上, 精灵会和已经升上来的窗口重叠,
//      下一帧又被抢一次, 来回抽搐。
// exclude 用来排除精灵当前正踩着的那块平台: 它由上层的"跟随"逻辑处理
// (跟随还要带上水平位移), 不能在这里被当成"抢走"。
static const WindowInfo* FindLandingTop(float x, float w, int h, float prevBottom,
                                        float newBottom, HWND exclude = nullptr)
{
    const WindowInfo* best = nullptr;
    for (auto& wv : g_windowList) {
        if (exclude && wv.hwnd == exclude) continue;
        const RECT& wr = wv.rect;
        if (x + w <= wr.left || x >= wr.right) continue;
        if (!CrossesTopEdge(wv, h, prevBottom, newBottom)) continue;
        if (!TopEdgeVisibleAt(wv, x + w * 0.5f)) continue;   // 顶边被压住, 不是平台
        if (!best || wr.top < best->rect.top) best = &wv;
    }
    return best;
}

static void ScanWindows()
{
    // 完整重建列表(EnumWindows 天然按 Z 序), 再回填上一次的 rect 用于算速度
    std::vector<WindowInfo> oldList;
    oldList.swap(g_windowList);
    g_windowList.clear();

    // ---- 先补上 EnumWindows 看不见的 "z-band 窗口" ----
    // Win11 的开始菜单 / 搜索 / 通知中心 等 shell 浮层是用 CreateWindowInBand
    // 建在比默认 z-band 更高的 band 上的。它们是货真价实的顶层窗口
    // (WS_POPUP|WS_VISIBLE, 父窗口是桌面, 没被 DWM cloaked, 矩形也正常),
    // 但 EnumWindows / EnumChildWindows / FindWindow / GetWindow 链
    // **一个都枚举不到它们**, 只有 GetForegroundWindow 能拿到。
    // 因为它们在所有窗口之上, 必须先入列 —— 这样后面的窗口才会把被它们
    // 压住的那部分顶边算成"看不见", 不会出现悬空的停靠点。
    HWND fg = GetForegroundWindow();
    g_foregroundHwnd = fg;
    g_bandHwnd = nullptr;
    if (fg) {
        ExistsCtx ec = { fg, false };
        EnumWindows(ExistsProc, reinterpret_cast<LPARAM>(&ec));
        if (!ec.found) {
            // 枚举不到的可见窗口 => band 窗口。此时 g_windowList 为空,
            // AcceptWindow 内部的遮挡收集自然是空的(它就是最上层)。
            AcceptWindow(fg, &g_windowList);
            g_bandHwnd = fg;
        }
    }

    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&g_windowList));

    for (auto& wi : g_windowList) {
        for (const auto& old : oldList) {
            if (old.hwnd == wi.hwnd) {
                wi.prevRect = old.rect;
                wi.hasPrev  = true;
                break;
            }
        }
    }
}

// ------------------------------------------------------------
// Sprite
// ------------------------------------------------------------
struct Sprite
{
    Bitmap* src = nullptr;
    int srcOffX = 0, srcOffY = 0;
    int srcW = 0, srcH = 0;
    int w = 0, h = 0;

    float x = 0.0f, y = 0.0f;
    float vx = 0.0f, vy = 0.0f;
    float gravity = 0.0f;
    float baseFallSpeed = 0.0f;

    float life = 0.0f, maxLife = 1.0f;
    float alpha = 1.0f;
    // 只在"爆炸碎屑"上用: 碎屑按剩余寿命淡出。
    // 碰撞不再设置它了(那是老的长达数秒的发灰效果), 碰撞改为 hitFlash。
    float fadeStartLife = 0.0f;
    // "被撞到了"的短暂闪光倒计时(秒)。>0 表示正在闪, 走三角形包络再恢复。
    float hitFlash = 0.0f;

    bool isFalling = false;
    bool isDebris  = false;
    bool dead = false;

    // 停在哪一个窗口的顶边上(没有则 nullptr)。
    // 有了它才能跟着窗口做"垂直"移动 —— 只靠"离顶边多少像素"是判断不出来的。
    HWND restingOn = nullptr;
};

static std::unique_ptr<Sprite> MakeSprite(Bitmap* src, int rx, int ry, int rw, int rh)
{
    if (!src || rw <= 0 || rh <= 0) return nullptr;
    auto s = std::make_unique<Sprite>();
    s->src = src;
    s->srcOffX = rx; s->srcOffY = ry;
    s->srcW = rw;    s->srcH = rh;
    s->w = rw;       s->h = rh;
    return s;
}

// ------------------------------------------------------------
// 图片资源（支持多帧 GIF 动画）
// ------------------------------------------------------------
struct ImageAsset
{
    Bitmap* bmp = nullptr;    // 缩放后的 SPRITE_SIZE 位图, 所有精灵都引用它
    Bitmap* raw = nullptr;    // 原始解码结果(多帧 GIF 靠它换帧)
    UINT    frameCount = 1;
    UINT    frame      = 0;
    std::vector<UINT> delayMs;   // 每帧延时(毫秒)
    float   timer = 0.0f;        // 当前帧已显示的时间
    float   life  = 3.0f;
    int     dx = 0, dy = 0, dw = 0, dh = 0;   // 居中缩放后的位置(相对 32x32)
};

static std::vector<std::unique_ptr<ImageAsset>> g_assets;
static std::vector<std::wstring>                g_failedFiles;

// 用 WIC 解码 —— GDI+ 不认识的格式(WebP / JPEG XL 等)走这里。
// 只要系统装了对应解码器就能读(WebP 解码器 Win10/11 通常自带)。
static Bitmap* LoadViaWIC(const std::wstring& path)
{
    if (!g_wicFactory) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    __uuidof(IWICImagingFactory),
                                    reinterpret_cast<void**>(&g_wicFactory))))
            return nullptr;
    }

    IWICBitmapDecoder*     decoder = nullptr;
    IWICBitmapFrameDecode* frame   = nullptr;
    IWICFormatConverter*   conv    = nullptr;
    Bitmap* result = nullptr;

    do {
        if (FAILED(g_wicFactory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &decoder)))
            break;

        if (FAILED(decoder->GetFrame(0, &frame)))
            break;

        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0)
            break;

        if (FAILED(g_wicFactory->CreateFormatConverter(&conv)))
            break;

        if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom)))
            break;

        // 32bppPBGRA 是预乘 alpha, 正好等价于 GDI+ 的 PixelFormat32bppPARGB
        const UINT stride = w * 4;
        std::vector<BYTE> buf((size_t)stride * h);
        if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)buf.size(), buf.data())))
            break;

        // 注意: GDI+ 的 Bitmap(w,h,stride,fmt,scan0) 只是"引用"这段内存,
        //       所以必须 Clone 一份自己拥有的副本, buf 才能安全释放。
        Bitmap tmp((INT)w, (INT)h, (INT)stride, PixelFormat32bppPARGB, buf.data());
        if (tmp.GetLastStatus() != Ok)
            break;

        result = tmp.Clone(0, 0, (INT)w, (INT)h, PixelFormat32bppPARGB);
        if (result && result->GetLastStatus() != Ok) { delete result; result = nullptr; }
    } while (false);

    if (conv)    conv->Release();
    if (frame)   frame->Release();
    if (decoder) decoder->Release();
    return result;
}

static void ShutdownWIC()
{
    if (g_wicFactory) { g_wicFactory->Release(); g_wicFactory = nullptr; }
}

// 把 raw 的当前帧按居中缩放布局重绘进 32x32 的 bmp
static void RenderAssetFrame(ImageAsset& a)
{
    if (!a.bmp || !a.raw) return;
    const int w = (int)a.raw->GetWidth();
    const int h = (int)a.raw->GetHeight();
    if (w <= 0 || h <= 0) return;

    Graphics g(a.bmp);
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.Clear(Color(0, 0, 0, 0));                       // 清掉上一帧的残留
    g.DrawImage(a.raw, Rect(a.dx, a.dy, a.dw, a.dh), 0, 0, w, h, UnitPixel);
    g.Flush();
}

static void ComputeAssetLayout(ImageAsset& a)
{
    const int w = (int)a.raw->GetWidth();
    const int h = (int)a.raw->GetHeight();
    if (w <= 0 || h <= 0) { a.dx = a.dy = 0; a.dw = a.dh = SPRITE_SIZE; return; }

    const float scale = std::min((float)SPRITE_SIZE / w, (float)SPRITE_SIZE / h);
    a.dw = std::max(1, (int)std::lround(w * scale));
    a.dh = std::max(1, (int)std::lround(h * scale));
    a.dx = (SPRITE_SIZE - a.dw) / 2;
    a.dy = (SPRITE_SIZE - a.dh) / 2;
}

static void ReadFrameDelays(ImageAsset& a)
{
    a.delayMs.assign(a.frameCount, 100);
    if (a.frameCount <= 1) return;

    const PROPID id = PropertyTagFrameDelay;
    const UINT size = a.raw->GetPropertyItemSize(id);
    if (size < sizeof(ULONG) * a.frameCount) return;

    std::vector<BYTE> buf(size);
    auto* item = reinterpret_cast<PropertyItem*>(buf.data());
    if (a.raw->GetPropertyItem(id, size, item) != Ok) return;

    const auto* d = reinterpret_cast<const ULONG*>(item->value);
    if (!d) return;
    for (UINT i = 0; i < a.frameCount; ++i)
        a.delayMs[i] = (d[i] == 0) ? 100 : d[i] * 10;   // GIF 单位是 1/100 秒
}

static void UpdateAnimations(float dt)
{
    for (auto& up : g_assets) {
        ImageAsset& a = *up;
        if (a.frameCount <= 1 || !a.raw) continue;

        a.timer += dt;
        int guard = 4;                       // 掉帧时最多补 4 帧, 防止卡死
        for (;;) {
            UINT d = (a.delayMs.size() == a.frameCount) ? a.delayMs[a.frame] : 100;
            if (d < 20) d = 100;
            const float need = d / 1000.0f;
            if (a.timer < need || guard-- <= 0) break;

            a.timer -= need;
            a.frame = (a.frame + 1) % a.frameCount;
            a.raw->SelectActiveFrame(&FrameDimensionTime, a.frame);
            RenderAssetFrame(a);
        }
    }
}

// ------------------------------------------------------------
// 全屏合成缓冲
// ------------------------------------------------------------
static bool InitComposeBuffer(int w, int h)
{
    // 屏幕 DC 只取一次, 重复取会泄漏
    if (!g_screenDC) g_screenDC = GetDC(nullptr);
    if (!g_screenDC) return false;

    g_composeDC = CreateCompatibleDC(g_screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    g_composeBm = CreateDIBSection(g_screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_composeBm || !bits) return false;

    g_composeBits = bits;
    g_composeOld = SelectObject(g_composeDC, g_composeBm);
    memset(bits, 0, (size_t)w * h * 4);
    return true;
}

static void FreeComposeBuffer()
{
    if (g_composeDC) {
        if (g_composeOld) SelectObject(g_composeDC, g_composeOld);
        DeleteDC(g_composeDC);
        g_composeDC  = nullptr;
        g_composeOld = nullptr;
    }
    if (g_composeBm) { DeleteObject(g_composeBm); g_composeBm = nullptr; }
    g_composeBits = nullptr;
}

// ------------------------------------------------------------
// 生成下落图片
// ------------------------------------------------------------
// usePos=true 时以 (px,py) 为中心生成(鼠标位置), 并夹进工作区,
// 免得生成到屏幕外面直接看不见; 否则照旧在屏幕顶端随机横坐标生成。
static void SpawnFalling(bool usePos = false, float px = 0.0f, float py = 0.0f)
{
    if (g_assets.empty()) return;
    if ((int)g_sprites.size() >= MAX_TOTAL) return;

    int falling = 0;
    for (auto& sp : g_sprites)
        if (sp->isFalling && !sp->dead) ++falling;
    if (falling >= MAX_FALLING) return;

    const int idx = RandI(0, (int)g_assets.size() - 1);
    ImageAsset& a = *g_assets[idx];
    if (!a.bmp) return;

    auto s = MakeSprite(a.bmp, 0, 0, SPRITE_SIZE, SPRITE_SIZE);
    if (!s) return;

    s->isFalling = true;
    s->isDebris  = false;
    if (usePos) {
        s->x = px - SPRITE_SIZE * 0.5f;
        s->y = py - SPRITE_SIZE * 0.5f;
        const float maxX = (float)std::max(0, g_screenW - SPRITE_SIZE);
        const float maxY = (float)std::max(0, g_screenH - SPRITE_SIZE);
        if (s->x < 0.0f)      s->x = 0.0f;    else if (s->x > maxX) s->x = maxX;
        if (s->y < 0.0f)      s->y = 0.0f;    else if (s->y > maxY) s->y = maxY;
    } else {
        s->x = RandF(0.0f, (float)std::max(1, g_screenW - SPRITE_SIZE));
        s->y = -(float)SPRITE_SIZE;
    }
    s->baseFallSpeed = RandF(FALL_SPEED_MIN, FALL_SPEED_MAX);
    s->vy = s->baseFallSpeed;
    s->vx = 0.0f;
    s->gravity = 0.0f;
    s->maxLife = s->life = a.life;
    s->alpha = 1.0f;
    s->fadeStartLife = 0.0f;
    s->hitFlash = 0.0f;

    g_sprites.push_back(std::move(s));
}

// ------------------------------------------------------------
// 爆炸：切碎片
// ------------------------------------------------------------
static void SpawnDebris(Sprite* s, std::vector<std::unique_ptr<Sprite>>& out)
{
    if (!s || !s->src || s->w <= 0 || s->h <= 0) return;
    if (g_sprites.size() + out.size() >= (size_t)MAX_TOTAL) return;

    const float cw = (float)s->w / DEBRIS_COLS;
    const float ch = (float)s->h / DEBRIS_ROWS;
    const float cx = s->x + s->w * 0.5f;
    const float cy = s->y + s->h * 0.5f;

    for (int r = 0; r < DEBRIS_ROWS; ++r) {
        for (int c = 0; c < DEBRIS_COLS; ++c) {

            if (g_sprites.size() + out.size() >= (size_t)MAX_TOTAL) return;

            int rx = (int)std::lround(c * cw);
            int ry = (int)std::lround(r * ch);
            int rw = (int)std::lround((c + 1) * cw) - rx;
            int rh = (int)std::lround((r + 1) * ch) - ry;
            if (rw <= 0 || rh <= 0) continue;

            auto d = MakeSprite(s->src, s->srcOffX + rx, s->srcOffY + ry, rw, rh);
            if (!d) continue;

            d->isFalling = false;
            d->isDebris  = true;
            d->x = s->x + rx;
            d->y = s->y + ry;

            float dx = (d->x + rw * 0.5f) - cx;
            float dy = (d->y + rh * 0.5f) - cy;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0f) len = 1.0f;

            float spd = RandF(140.0f, 430.0f);
            d->vx = dx / len * spd + RandF(-50.0f, 50.0f);
            d->vy = dy / len * spd + RandF(-200.0f, -60.0f);
            d->gravity = 1200.0f;

            d->maxLife = RandF(0.85f, 1.45f);
            d->life    = d->maxLife;
            d->alpha   = 1.0f;
            d->fadeStartLife = d->maxLife;   // 碎屑淡出(碰撞不走这条路)
            d->hitFlash = 0.0f;

            out.push_back(std::move(d));
        }
    }
}

// ------------------------------------------------------------
// 爆炸冲击波: 把半径内的其它精灵一起炸飞
// 注意: 这里刻意不碰 alpha 相关字段(fadeStartLife / hitFlash) ——
//       被爆炸冲击波掀飞不应该让图片变浅, 这一点和窗口"创飞"不同。
// ------------------------------------------------------------
static void ApplyExplosionShockwave(float cx, float cy)
{
    for (auto& sp : g_sprites) {
        Sprite* s = sp.get();
        if (s->dead || !s->src) continue;

        float dx = (s->x + s->w * 0.5f) - cx;
        float dy = (s->y + s->h * 0.5f) - cy;
        float d  = std::sqrt(dx * dx + dy * dy);
        if (d > EXPLOSION_RADIUS) continue;

        if (d < 1.0f) {
            // 正好压在爆心上, 给一个随机朝上的方向
            dx = RandF(-1.0f, 1.0f);
            dy = -1.0f;
            d  = std::sqrt(dx * dx + dy * dy);
            if (d < 0.001f) { dx = 0.0f; dy = -1.0f; d = 1.0f; }
        }

        // 越靠近爆心越猛
        const float t = 1.0f - d / EXPLOSION_RADIUS;
        const float power = EXPLOSION_POWER_MIN +
                            (EXPLOSION_POWER_MAX - EXPLOSION_POWER_MIN) * t;

        s->vx = dx / d * power + RandF(-40.0f, 40.0f);
        s->vy = dy / d * power - RandF(40.0f, 140.0f);   // 整体略微上扬
        s->gravity   = EXPLOSION_GRAVITY;
        s->restingOn = nullptr;      // 从平台上掀下来

        // 下落中的图片: gravity > 0 会把它切到抛物线分支;
        // 碎片本来就吃 gravity/vx/vy, 直接生效。
    }
}

// ------------------------------------------------------------
// 通用：与窗口进行 AABB 碰撞（供抛物线状态使用）
// prevX / prevBottom 是精灵本帧位移之前的位置 —— 光有"现在重叠了"是不够的,
// 必须知道它是从哪一面进来的。
// ------------------------------------------------------------
static void CollideWithWindows(Sprite* s, float prevX, float prevBottom)
{
    const float prevTop   = prevBottom - (float)s->h;      // 精灵上一拍的顶边
    const float prevRight = prevX + (float)s->w;           // 精灵上一拍的右边缘
    const float newBottom = s->y + (float)s->h;
    const float newRight  = s->x + (float)s->w;

    for (auto& wv : g_windowList) {
        const RECT& wr = wv.rect;
        if (newRight <= (float)wr.left || s->x >= (float)wr.right) continue;
        if (newBottom <= (float)wr.top  || s->y >= (float)wr.bottom) continue;

        // 四个方向的穿透深度
        const float pTop    = newBottom - (float)wr.top;
        const float pBottom = (float)wr.bottom - s->y;
        const float pLeft   = newRight - (float)wr.left;
        const float pRight  = (float)wr.right - s->x;

        // ---- 参照矩形: 这一拍和上一拍"并起来"的那一层 ----
        // 窗口矩形每 WINDOW_SCAN_SEC 才采样一次, 快速拖动时一帧能跳几十像素。
        // 只拿当前矩形判"上一拍在外面"是判不出来的(窗口一冲, 上一拍的位置早就
        // 不在当前矩形里了), 所以窗口动过的那条边要用上一拍的位置。
        const float refTop    = (wv.hasPrev && (float)wv.prevRect.top    > (float)wr.top)    ? (float)wv.prevRect.top    : (float)wr.top;
        const float refBottom = (wv.hasPrev && (float)wv.prevRect.bottom < (float)wr.bottom) ? (float)wv.prevRect.bottom : (float)wr.bottom;
        const float refLeft   = (wv.hasPrev && (float)wv.prevRect.left   < (float)wr.left)   ? (float)wv.prevRect.left   : (float)wr.left;
        const float refRight  = (wv.hasPrev && (float)wv.prevRect.right  > (float)wr.right)  ? (float)wv.prevRect.right  : (float)wr.right;

        // ---- 上一拍这个精灵在窗口的哪一面 ----
        const bool wasLeft  = (prevRight <= refLeft);
        const bool wasRight = (prevX     >= refRight);
        const bool wasAbove = (prevBottom <= refTop);
        const bool wasBelow = (prevTop   >= refBottom);
        const bool inX      = !wasLeft && !wasRight;   // 上一拍横向就和窗口重叠了

        // ---- 按"从哪一面进来"解算, 而不是按"离哪条边近" ----
        // 最小穿透深度只看距离、不看来向, 所以贴着左/右边缘的图片会被甩到侧面、
        // 贴着底边的会被甩到下方 —— 视觉上就是"直接穿过窗口", 跟快慢无关。
        //
        // 四个面是互斥的(上一拍只可能在窗口外面的一侧), 所以这里最多只有一个为真,
        // 不会出现"侧边撞的却按顶边解"。
        //
        // "从上面进来"必须额外要求 inX: 上一拍横向就已经和窗口重叠。
        // 少了这一条, 一个被窗口**侧边**撞到、位置恰好在窗口上半部分的图片,
        // 会因为"窗口这一拍正好也往上走了"而满足之前的宽松顶边判据,
        // 被瞬移到窗口顶上。
        const bool fromTop    = inX && wasAbove && (newBottom > (float)wr.top);
        const bool fromBottom = wasBelow && (s->y < (float)wr.bottom);
        const bool fromLeft   = wasLeft  && (newRight > (float)wr.left);
        const bool fromRight  = wasRight && (s->x < (float)wr.right);

        int axis = -1;              // 0=上方 1=下方 2=左侧 3=右侧
        if      (fromTop)    axis = 0;
        else if (fromBottom) axis = 1;
        else if (fromLeft)   axis = 2;
        else if (fromRight)  axis = 3;

        if (axis < 0) {
            // ---- 上一拍就已经在窗口肚子里了 -> 退回最小穿透, 但要带护栏 ----
            // 顶边: 上面放得下这个精灵才算(TopEdgeFits)。
            // 其余边: 必须在工作区内; 而且穿透深度不能超过一个身位, 否则
            //         "最小穿透"会把一个本来就待在窗口肚子里的精灵整个挪到
            //         另一条边上去 —— 这正是"瞬移到窗口下方/侧方"。
            //         深陷其中的干脆不解析, 让它按物理自己落出去。
            //         例: 通知中心 (2200,0)-(2560,1392) 的右边和底边都贴着工作区
            //         边界, 只有左边的竖边是"实墙"。
            // 窗口底边: 只认"精灵相对窗口在往上走"(从下面撞上来)。不加这一条,
            //         一个往下穿过窗口的精灵会在退出底边的瞬间被"啪"地按到
            //         wr.bottom 上, 看起来就是无端跳一下。
            const float relVy = s->vy -
                (wv.hasPrev ? (float)(wv.rect.top - wv.prevRect.top) / g_scanDt : 0.0f);
            const bool okTop    = TopEdgeFits(wv, s->h) && TopEdgeVisibleAt(wv, s->x + s->w * 0.5f);
            const bool okBottom = (wr.bottom < g_screenH) && (pBottom <= (float)s->h) &&
                                  (relVy < 0.0f);
            const bool okLeft   = (wr.left   > 0)         && (pLeft   <= (float)s->w);
            const bool okRight  = (wr.right  < g_screenW) && (pRight  <= (float)s->w);

            float minP = 0.0f;
            if (okTop)               { minP = pTop;    axis = 0; }
            if (okBottom && (axis < 0 || pBottom < minP)) { minP = pBottom; axis = 1; }
            if (okLeft   && (axis < 0 || pLeft   < minP)) { minP = pLeft;   axis = 2; }
            if (okRight  && (axis < 0 || pRight  < minP)) { minP = pRight;  axis = 3; }
            if (axis < 0) continue;            // 四条边都不算数 -> 当作背景
        }

        if (axis == 0) {
            // 从上方落到窗口顶部
            s->y = (float)wr.top - (float)s->h;
            if (s->vy > 0.0f) s->vy = -s->vy * 0.35f;
            if (fabsf(s->vy) < 50.0f) s->vy = 0.0f;
            s->vx *= 0.85f;
            if (fabsf(s->vx) < 20.0f) s->vx = 0.0f;
        } else if (axis == 1) {
            // 从下方撞到窗口底部
            s->y = (float)wr.bottom;
            if (s->vy < 0.0f) s->vy = -s->vy * 0.4f;
        } else if (axis == 2) {
            // 精灵是从左边进来的 -> 推回窗口左侧
            s->x = (float)wr.left - (float)s->w;
            if (s->vx > 0.0f) s->vx = -s->vx * 0.5f;
        } else {
            // 精灵是从右边进来的 -> 推回窗口右侧
            s->x = (float)wr.right;
            if (s->vx < 0.0f) s->vx = -s->vx * 0.5f;
        }
        break; // 一帧只处理一次窗口碰撞
    }
}

// ------------------------------------------------------------
// 物理更新
// ------------------------------------------------------------
static void UpdatePhysics(float dt)
{
    if (dt <= 0.0f) dt = 0.001f;

    g_scanTimer -= dt;
    g_scanAccum += dt;
    if (g_scanTimer <= 0.0f) {
        ScanWindows();
        g_scanDt    = (g_scanAccum > 0.001f) ? g_scanAccum : 0.001f;
        g_scanAccum = 0.0f;
        g_scanTimer = WINDOW_SCAN_SEC;
    }

    // ---- 逐个精灵更新 ----
    for (auto& sp : g_sprites) {
        Sprite* s = sp.get();
        if (s->dead) continue;

        if (s->isFalling) {
            // 被快速水平移动的窗口"创飞"
            if (s->gravity == 0.0f) {
                for (auto& wv : g_windowList) {
                    if (!wv.hasPrev || !wv.platform) continue;
                    // 窗口位置是每 0.1s 采样一次的, 必须用扫描间隔换算速度;
                    // 用每帧 dt 会把速度放大 5~6 倍, 导致图片被轻微移动的窗口"创飞"
                    float winVx = (float)(wv.rect.left - wv.prevRect.left) / g_scanDt;
                    if (fabsf(winVx) < SWEEP_MIN_VX) continue;
                    if (winVx >  3000.0f) winVx =  3000.0f;
                    if (winVx < -3000.0f) winVx = -3000.0f;

                    if (s->x + s->w <= wv.rect.left || s->x >= wv.rect.right) continue;
                    if (s->y + s->h <= wv.rect.top  || s->y >= wv.rect.bottom) continue;

                    s->vx = winVx * 0.8f;
                    s->vy = -320.0f;
                    s->gravity = 1500.0f;
                    s->restingOn = nullptr;
                    s->hitFlash  = HIT_FLASH_SEC;   // 闪一下, 不再长时间变浅
                    // 被窗口以特别快的速度撞飞 -> 引信缩短
                    if (fabsf(winVx) >= FAST_HIT_SPEED)
                        s->life -= FAST_HIT_LIFE_LOSS;
                    break;
                }
            }

            if (s->gravity > 0.0f) {
                // ---- 抛物线（被弹飞/被创飞） ----
                s->vy += s->gravity * dt;
                const float prevX = s->x;
                s->x  += s->vx * dt;

                const float prevBottom = s->y + s->h;
                s->y  += s->vy * dt;
                s->life -= dt;

                // 屏幕边界反弹
                if (s->x < 0) { s->x = 0; s->vx = -s->vx * 0.7f; }
                if (s->x + s->w > g_screenW) { s->x = (float)(g_screenW - s->w); s->vx = -s->vx * 0.7f; }
                if (s->y < 0) { s->y = 0; s->vy = -s->vy * 0.7f; }
                if (s->y + s->h > g_screenH) {
                    s->y = (float)(g_screenH - s->h);
                    s->vy = -s->vy * 0.4f;
                    if (fabsf(s->vy) < 30.0f) s->vy = 0.0f;
                }

                // 飞行中也要与窗口碰撞
                CollideWithWindows(s, prevX, prevBottom);

                // 兜底: 窗口矩形每 WINDOW_SCAN_SEC 才采样一次, 快速上拖时可能
                // 一拍就整个跳过精灵 —— 这时 CollideWithWindows 的重叠判定
                // 根本不成立(它要求两者仍然相交), 精灵会被留在窗口下面, 看起来
                // 就是窗口穿了过去。这里再用 FindLandingTop 补一次:
                // 底边这一帧确实越过了某条顶边就贴上去。
                // 正常的抛物线落顶边不会被重复处理 —— CollideWithWindows 已经
                // 把它摆到 y = top - h, newBottom > curTop 自然不成立。
                if (const WindowInfo* w =
                        FindLandingTop(s->x, (float)s->w, s->h, prevBottom, s->y + s->h))
                {
                    s->y = (float)w->rect.top - (float)s->h;
                    if (s->vy > 0.0f) s->vy = -s->vy * 0.35f;
                }

                // 窗口碰撞后可能又把自己推到屏幕外，再钳制一次
                if (s->x < 0) s->x = 0;
                if (s->x + s->w > g_screenW) s->x = (float)(g_screenW - s->w);
                if (s->y < 0) s->y = 0;
                if (s->y + s->h > g_screenH) {
                    s->y = (float)(g_screenH - s->h);
                    if (s->vy > 0) s->vy = 0.0f;
                }
            }
            else {
                // ---- 匀速下落 / 停在窗口顶边 ----
                s->life -= dt;

                bool resting = false;

                // 1) 上一帧停在某窗口顶边上 -> 先跟着这个窗口走(水平 + 垂直)
                //    只跟水平是不够的: 窗口往上/斜上的时候图片会被"穿过"或横着滑走。
                if (s->restingOn) {
                    const WindowInfo* host = nullptr;
                    for (auto& wv : g_windowList) {
                        // 只要这个窗口还在列表里就继续跟着它 —— 这里**不**再要求
                        // TopEdgeFits。窗口被往上拖到顶边上面放不下时, 图片会压在
                        // y=0 继续跟着走; 放掉它才是错的: 用户看到的是"图片莫名其妙
                        // 从窗口顶边上掉下去"。
                        // (新图片能不能"落"在这种窗口上是另一回事, 那条走
                        //  FindLandingTop/CrossesTopEdge, 仍然是严格的 TopEdgeFits。)
                        if (wv.hwnd == s->restingOn) { host = &wv; break; }
                    }
                    // 有"别的"窗口这一拍从下面顶上来 -> 直接改站到它上面。
                    // 只"放掉"是不行的: 原来那块平台的顶边还在原处, 下一帧又会把
                    // 图片接回去, 于是来回抽搐。不做这一步的话, 停靠在 A 上的图片
                    // 会被上升的 B 直接穿过去。
                    if (host) {
                        const float myBottom = s->y + s->h;
                        if (const WindowInfo* thief =
                                FindLandingTop(s->x, (float)s->w, s->h, myBottom, myBottom,
                                               s->restingOn))
                        {
                            s->y         = (float)thief->rect.top - (float)s->h;
                            s->restingOn = thief->hwnd;
                            s->vy        = 0.0f;
                            resting      = true;
                        }
                    }
                    if (host && !resting) {
                        const bool overlapX = !(s->x + s->w <= host->rect.left ||
                                                s->x >= host->rect.right);
                        const float ny = (float)host->rect.top - (float)s->h;
                        if (overlapX &&
                            TopEdgeVisibleAt(*host, s->x + s->w * 0.5f))
                        {
                            // 垂直: 顶边是参照点, 先精确对齐再按窗速平滑外推。
                            // 只写 s->y = ny 的话, 图片每 0.08s 才动一下(窗口矩形
                            // 是采样来的), 看起来就是一顿一顿地"跟不上"。
                            s->y = ny;
                            if (host->hasPrev) {
                                const float winVx =
                                    (float)(host->rect.left - host->prevRect.left) / g_scanDt;
                                const float winVy =
                                    (float)(host->rect.top  - host->prevRect.top ) / g_scanDt;

                                // 水平: 顶边上没有参照点, 只能按窗速积分。
                                //
                                // 这里**故意不设"游戏性"速度上限**。原来卡在
                                // FOLLOW_MAX_VX(250 px/s), 而拖动窗口轻松就超过它,
                                // 结果窗口直接从图片脚下被抽走 ——
                                // "图片跟不上 / 滑出去 / 掉下去"。
                                // 图片本来就压在窗口顶边上, 窗口一动它当然要跟着走;
                                // 创飞(横向扫进来撞飞)是另一条路径, 不受影响。
                                // 只留一个荒谬值上限挡窗口矩形抖动。
                                if (fabsf(winVx) < FOLLOW_SANITY_VX)
                                    s->x += winVx * dt;
                                // 同样按"距上次扫描过了多久"外推垂直位置
                                if (fabsf(winVy) < FOLLOW_SANITY_VX)
                                    s->y += winVy * g_scanAccum;
                            }
                            if (s->y < 0.0f) s->y = 0.0f;   // 顶边已到屏幕上沿, 压在 y=0
                            if (s->x < 0) s->x = 0;
                            if (s->x + s->w > g_screenW) s->x = (float)(g_screenW - s->w);
                            s->vy = 0.0f;
                            resting = true;
                        }
                    }
                    if (!resting) s->restingOn = nullptr;

                    if (resting) {
                        if (s->x < 0) s->x = 0;
                        if (s->x + s->w > g_screenW) s->x = (float)(g_screenW - s->w);
                    }
                }

                if (!resting) {
                    // 2) 自由下落
                    s->vy = s->baseFallSpeed;
                    const float prevBottom = s->y + s->h;
                    s->y += s->vy * dt;

                    // 落到某个窗口"看得见"的顶边上。
                    // 判定交给 FindLandingTop/CrossesTopEdge: 它们额外考虑了
                    // "窗口自己往上冲"的情况, 否则窗口上拖时图片会直接穿过窗口
                    // 而不是被顶上去。
                    if (const WindowInfo* w =
                            FindLandingTop(s->x, (float)s->w, s->h, prevBottom, s->y + s->h))
                    {
                        s->y         = (float)w->rect.top - (float)s->h;
                        s->vy        = 0.0f;
                        s->restingOn = w->hwnd;
                    }

                    // 3) 工作区底部
                    if (s->y + s->h >= g_screenH) {
                        s->y = (float)(g_screenH - s->h);
                        s->vy = 0.0f;
                        s->restingOn = nullptr;
                    }
                }
            }
        }
        else {
            // ---- 碎片 / 弹丸 ----
            s->vy += s->gravity * dt;
            s->x  += s->vx * dt;

            const float prevBottom = s->y + s->h;
            s->y  += s->vy * dt;
            s->life -= dt;

            if (s->x < 0) { s->x = 0; s->vx = -s->vx * 0.7f; }
            if (s->x + s->w > g_screenW) { s->x = (float)(g_screenW - s->w); s->vx = -s->vx * 0.7f; }

            // ---- 碎片也能落在窗口顶边上 ----
            // 和图片共用 FindLandingTop/CrossesTopEdge, 所以窗口快速上拖时碎片
            // 同样会被顶上去, 而不是穿过去。
            // 落地方式和"工作区底部"完全一致(弹一下、阻尼、然后停住),
            // 而且因为判据里含上一拍的顶边, 停在窗口上的碎片会自动跟着窗口上下走。
            // 刻意不设 restingOn: 碎片只活 1 秒左右, 不需要水平跟随。
            if (const WindowInfo* w =
                    FindLandingTop(s->x, (float)s->w, s->h, prevBottom, s->y + s->h))
            {
                s->y = (float)w->rect.top - (float)s->h;
                if (s->vy > 0.0f) s->vy = -s->vy * 0.35f;
                if (fabsf(s->vy) < 40.0f) s->vy = 0.0f;
                s->vx *= 0.85f;
                if (fabsf(s->vx) < 15.0f) s->vx = 0.0f;
            }

            if (s->y < 0) { s->y = 0; s->vy = -s->vy * 0.7f; }
            if (s->y + s->h > g_screenH) {
                s->y = (float)(g_screenH - s->h);
                if (s->vy > 0) s->vy = -s->vy * 0.35f;
                if (fabsf(s->vy) < 40.0f) s->vy = 0.0f;
                s->vx *= 0.85f;
                if (fabsf(s->vx) < 15.0f) s->vx = 0.0f;
            }
        }

        // ---- 透明度 ----
        float a = 1.0f;

        // (1) 爆炸碎屑自身的淡出。碰撞已经不走这条路了。
        if (s->fadeStartLife > 0.0f) {
            float d = s->life / s->fadeStartLife;
            if (d < 0.0f) d = 0.0f;
            if (d < a) a = d;
        }

        // (2) 被撞/被弹 -> 快速闪一下再恢复。
        //     三角形包络: 前半段压到 HIT_DIM, 后半段回到全不透明,
        //     不再按剩余寿命慢慢发灰。
        if (s->hitFlash > 0.0f) {
            float t = 1.0f - s->hitFlash / HIT_FLASH_SEC;   // 0 -> 1
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            const float k = (t < 0.5f) ? (t * 2.0f) : ((1.0f - t) * 2.0f);  // 0->1->0
            const float dim = 1.0f - (1.0f - HIT_DIM) * k;
            if (dim < a) a = dim;

            s->hitFlash -= dt;
            if (s->hitFlash < 0.0f) s->hitFlash = 0.0f;
        }

        // (3) 临爆闪烁: 爆炸前 EXPLODE_WARN_SEC 秒开始, 每 EXPLODE_BLINK_PERIOD 秒一闪。
        //     只对"会爆炸"的精灵生效(下落中的图片), 碎屑只是消失、不爆炸。
        if (s->isFalling && !s->isDebris &&
            s->life > 0.0f && s->life <= EXPLODE_WARN_SEC) {
            const float phase = fmodf(s->life, EXPLODE_BLINK_PERIOD) / EXPLODE_BLINK_PERIOD;
            if (phase < 0.5f && EXPLODE_BLINK_MIN < a) a = EXPLODE_BLINK_MIN;
        }

        s->alpha = a;
    }

    // ---- 精灵之间碰撞（下落 + 抛物线都参与） ----
    for (size_t i = 0; i < g_sprites.size(); ++i) {
        Sprite* a = g_sprites[i].get();
        if (a->dead || !a->isFalling) continue;
        for (size_t j = i + 1; j < g_sprites.size(); ++j) {
            Sprite* b = g_sprites[j].get();
            if (b->dead || !b->isFalling) continue;

            float ax1 = a->x, ay1 = a->y, ax2 = a->x + a->w, ay2 = a->y + a->h;
            float bx1 = b->x, by1 = b->y, bx2 = b->x + b->w, by2 = b->y + b->h;
            float overlapX = std::min(ax2, bx2) - std::max(ax1, bx1);
            float overlapY = std::min(ay2, by2) - std::max(ay1, by1);
            if (overlapX <= 0.0f || overlapY <= 0.0f) continue;

            // 碰撞前的相对速度。下面会改写速度, 所以必须在这里先算。
            const float relVx = a->vx - b->vx;
            const float relVy = a->vy - b->vy;
            const float impact = std::sqrt(relVx * relVx + relVy * relVy);

            float acx = a->x + a->w * 0.5f;
            float bcx = b->x + b->w * 0.5f;
            float acy = a->y + a->h * 0.5f;
            float bcy = b->y + b->h * 0.5f;

            if (overlapX < overlapY) {
                // 水平碰撞
                float dir = (acx < bcx) ? -1.0f : 1.0f;
                float push = overlapX * 0.5f + 1.0f;
                a->x += dir * push;
                b->x -= dir * push;

                float aVx = a->vx;
                a->vx = b->vx * 0.5f + dir * 90.0f;
                b->vx = aVx * 0.5f - dir * 90.0f;

                a->vy = -80.0f;
                b->vy = -80.0f;
                a->gravity = 900.0f;
                b->gravity = 900.0f;
            }
            else {
                // 垂直碰撞
                float dir = (acy < bcy) ? -1.0f : 1.0f;
                float push = overlapY * 0.5f + 1.0f;
                a->y += dir * push;
                b->y -= dir * push;

                if (dir < 0) {
                    float av = fabsf(a->vy);
                    float bv = fabsf(b->vy);
                    a->vy = -(av * 0.4f + 120.0f);
                    b->vy =  (bv * 0.3f + 40.0f);
                } else {
                    float av = fabsf(a->vy);
                    float bv = fabsf(b->vy);
                    b->vy = -(bv * 0.4f + 120.0f);
                    a->vy =  (av * 0.3f + 40.0f);
                }

                a->vx += RandF(-40.0f, 40.0f);
                b->vx += RandF(-40.0f, 40.0f);
                a->gravity = 900.0f;
                b->gravity = 900.0f;
            }

            a->restingOn = nullptr;
            b->restingOn = nullptr;

            if (a->x < 0) a->x = 0;
            if (a->x + a->w > g_screenW) a->x = (float)(g_screenW - a->w);
            if (b->x < 0) b->x = 0;
            if (b->x + b->w > g_screenW) b->x = (float)(g_screenW - b->w);

            a->hitFlash = HIT_FLASH_SEC;
            b->hitFlash = HIT_FLASH_SEC;

            // 特别快的互撞 -> 双方引信都缩短
            if (impact >= FAST_HIT_SPEED) {
                a->life -= FAST_HIT_LIFE_LOSS;
                b->life -= FAST_HIT_LIFE_LOSS;
            }
        }
    }

    // ---- 检查死亡 / 爆炸 ----
    std::vector<Sprite*> toExplode;
    for (auto& sp : g_sprites) {
        if (sp->dead) continue;
        if (sp->life > 0.0f) continue;

        if (sp->isFalling && !sp->isDebris) {
            sp->dead = true;
            toExplode.push_back(sp.get());
        } else {
            sp->dead = true;
        }
    }

    // ---- 爆炸冲击波: 把范围内的其它精灵炸飞(不变浅), 再生成自己的碎片 ----
    for (Sprite* s : toExplode)
        ApplyExplosionShockwave(s->x + s->w * 0.5f, s->y + s->h * 0.5f);

    std::vector<std::unique_ptr<Sprite>> pending;
    for (Sprite* s : toExplode)
        SpawnDebris(s, pending);

    for (auto it = g_sprites.begin(); it != g_sprites.end(); ) {
        if ((*it)->dead) it = g_sprites.erase(it);
        else             ++it;
    }
    for (auto& p : pending)
        g_sprites.push_back(std::move(p));
}

// ------------------------------------------------------------
// 渲染并提交
// ------------------------------------------------------------
static void PresentAll()
{
    if (!g_overlayHwnd || !g_composeBits) return;

    memset(g_composeBits, 0, (size_t)g_screenW * g_screenH * 4);

    Bitmap compose(g_screenW, g_screenH, g_screenW * 4,
                   PixelFormat32bppPARGB, (BYTE*)g_composeBits);
    Graphics g(&compose);
    g.SetCompositingMode(CompositingModeSourceOver);
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    for (auto& sp : g_sprites) {
        Sprite* s = sp.get();
        if (s->dead) continue;

        Rect dst((INT)std::lround(s->x), (INT)std::lround(s->y), s->w, s->h);

        if (s->alpha >= 0.999f) {
            g.DrawImage(s->src, dst,
                        s->srcOffX, s->srcOffY, s->srcW, s->srcH,
                        UnitPixel);
        } else {
            REAL a = s->alpha;
            ColorMatrix cm = {
                a, 0, 0, 0, 0,
                0, a, 0, 0, 0,
                0, 0, a, 0, 0,
                0, 0, 0, a, 0,
                0, 0, 0, 0, 1
            };
            ImageAttributes ia;
            ia.SetColorMatrix(&cm);
            g.DrawImage(s->src, dst,
                        s->srcOffX, s->srcOffY, s->srcW, s->srcH,
                        UnitPixel, &ia);
        }
    }

    // 必须先把 GDI+ 的绘制指令冲刷下去, 再拿这块 DIB 去 UpdateLayeredWindow,
    // 否则可能提交到还没画完的内容。
    g.Flush();

    POINT ptDst = { g_workX, g_workY };
    SIZE  sz    = { g_screenW, g_screenH };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    UpdateLayeredWindow(g_overlayHwnd, g_screenDC, &ptDst, &sz,
                        g_composeDC, &ptSrc, 0, &bf, ULW_ALPHA);
}

// ------------------------------------------------------------
// 加载图片
// ------------------------------------------------------------
static void LoadImages()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring dir(exePath);
    size_t p = dir.find_last_of(L"\\/");
    if (p != std::wstring::npos) dir = dir.substr(0, p);

    std::wstring imgDir = dir + L"\\images";

    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd = {};
    std::wstring pattern = imgDir + L"\\*.*";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            std::wstring name = fd.cFileName;
            std::wstring lower = name;
            for (auto& ch : lower) ch = (wchar_t)towlower(ch);

            size_t dot = lower.find_last_of(L'.');
            std::wstring ext = (dot == std::wstring::npos) ? L"" : lower.substr(dot);

            if (ext == L".png"  || ext == L".jpg"  || ext == L".jpeg" ||
                ext == L".bmp"  || ext == L".gif"  || ext == L".webp" ||
                ext == L".tif"  || ext == L".tiff" || ext == L".ico"  ||
                ext == L".jfif" || ext == L".jxl")
            {
                files.push_back(imgDir + L"\\" + name);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    std::sort(files.begin(), files.end());
    if ((int)files.size() > MAX_IMAGES) files.resize(MAX_IMAGES);

    for (auto& path : files) {
        auto asset = std::make_unique<ImageAsset>();

        // 1) 先交给 GDI+ —— 它自带 GIF 多帧合成, 换帧只要 SelectActiveFrame
        Bitmap* raw = Bitmap::FromFile(path.c_str());
        if (raw && raw->GetLastStatus() != Ok) { delete raw; raw = nullptr; }

        // 2) GDI+ 读不了的(WebP / JPEG XL ...)交给 WIC
        if (!raw) raw = LoadViaWIC(path);

        if (!raw) {
            g_failedFiles.push_back(FileNameOf(path));
            continue;
        }

        const int w = (int)raw->GetWidth();
        const int h = (int)raw->GetHeight();
        if (w <= 0 || h <= 0) {
            delete raw;
            g_failedFiles.push_back(FileNameOf(path));
            continue;
        }

        asset->raw = raw;
        asset->frameCount = raw->GetFrameCount(&FrameDimensionTime);
        if (asset->frameCount == 0) asset->frameCount = 1;
        asset->frame = 0;
        asset->life  = SPRITE_LIFE_SEC;   // 所有图片统一 5 秒

        ReadFrameDelays(*asset);
        ComputeAssetLayout(*asset);

        asset->bmp = new Bitmap(SPRITE_SIZE, SPRITE_SIZE, PixelFormat32bppPARGB);
        if (asset->bmp->GetLastStatus() != Ok) {
            delete asset->bmp;
            asset->bmp = nullptr;
            delete raw;
            g_failedFiles.push_back(FileNameOf(path));
            continue;
        }
        RenderAssetFrame(*asset);

        g_assets.push_back(std::move(asset));
    }
}

// ------------------------------------------------------------
// 显示/工作区变化(改分辨率、任务栏移动等) -> 重建 overlay 与合成缓冲
// ------------------------------------------------------------
static bool CheckDisplayChange()
{
    RECT wa = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) return true;

    const int nx = wa.left, ny = wa.top;
    const int nw = wa.right - wa.left;
    const int nh = wa.bottom - wa.top;
    if (nx == g_workX && ny == g_workY && nw == g_screenW && nh == g_screenH)
        return true;
    if (nw <= 0 || nh <= 0) return true;

    g_workX = nx; g_workY = ny; g_screenW = nw; g_screenH = nh;

    FreeComposeBuffer();

    if (g_overlayHwnd) {
        SetWindowPos(g_overlayHwnd, HWND_TOPMOST, g_workX, g_workY, g_screenW, g_screenH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    if (!InitComposeBuffer(g_screenW, g_screenH))
        return false;

    // 窗口矩形全部作废, 强制重扫; 并把精灵夹回可见区域
    g_scanTimer = 0.0f;
    g_scanAccum = 0.0f;
    for (auto& sp : g_sprites) {
        sp->restingOn = nullptr;
        if (sp->x < 0) sp->x = 0;
        if (sp->y < 0) sp->y = 0;
        if (sp->x + sp->w > g_screenW) sp->x = (float)std::max(0, g_screenW - sp->w);
        if (sp->y + sp->h > g_screenH) sp->y = (float)std::max(0, g_screenH - sp->h);
    }
    return true;
}

// ------------------------------------------------------------
// 入口
// ------------------------------------------------------------
struct ComGuard
{
    bool ok = false;
    ComGuard()  { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComGuard() { if (ok) CoUninitialize(); }
};

// ------------------------------------------------------------
// 低级键盘钩子: 按 "." 在鼠标位置生成图片
// ------------------------------------------------------------
// overlay 窗口带 WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, 永远拿不到焦点,
// 所以它收不到 WM_KEYDOWN; 而 RegisterHotKey 会把这个键从所有程序手里抢走。
// WH_KEYBOARD_LL 钩子只是"旁听": 我们照旧 CallNextHookEx 把按键原样放行,
// 别的程序完全不受影响。
// 低级钩子由安装它的线程(主线程)在自己的消息循环里回调, 所以下面这几个
// 变量不存在跨线程竞争 —— 钩子里只记一下位置, 真正的生成放在主循环里做。
static HHOOK g_kbHook       = nullptr;
static DWORD g_lastDotTick  = 0;       // 上次触发的时刻(去抖, 兼作滤掉长按自动重复)
static bool  g_spawnAtMouse = false;   // 有待处理的生成请求
static POINT g_spawnPoint   = {};

// 两次触发的最小间隔(毫秒)。长按的自动重复间隔只有 ~30ms, 会被这条挡掉;
// 而且它是"比时间戳"而不是"记按键状态", 所以就算漏掉一次 KEYUP 也不会卡死。
static const DWORD SPAWN_KEY_DEBOUNCE_MS = 250;

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && lParam) {
        const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (kb->vkCode == SPAWN_KEY_VK &&
            (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            const DWORD now = GetTickCount();
            if (now - g_lastDotTick >= SPAWN_KEY_DEBOUNCE_MS) {
                g_lastDotTick = now;
                POINT pt;
                if (GetCursorPos(&pt)) {
                    g_spawnPoint   = pt;
                    g_spawnAtMouse = true;
                }
            }
        }
    }
    // 原样放行: 别的程序该怎么收到 "." 还是怎么收到
    return CallNextHookEx(g_kbHook, code, wParam, lParam);
}

// 退出热键候选: 默认 Ctrl+Alt+Q 被占用时自动退到下一个
struct HotkeyDef { UINT mods; UINT vk; const wchar_t* name; };
static const HotkeyDef kHotkeys[] = {
    { MOD_CONTROL | MOD_ALT, 'Q',      L"Ctrl+Alt+Q"    },
    { MOD_CONTROL | MOD_ALT, 'W',      L"Ctrl+Alt+W"    },
    { MOD_CONTROL | MOD_ALT, 'X',      L"Ctrl+Alt+X"    },
    { MOD_CONTROL | MOD_ALT, VK_F12,   L"Ctrl+Alt+F12"  },
    { MOD_CONTROL | MOD_SHIFT, 'Q',    L"Ctrl+Shift+Q"  },
};

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    g_hInst = hInstance;
    ComGuard comGuard;                       // WIC 需要 COM

    GdiplusStartupInput gsi;
    if (GdiplusStartup(&g_gdiplusToken, &gsi, nullptr) != Ok)
        return 1;

    RECT wa = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_workX   = wa.left;
    g_workY   = wa.top;
    g_screenW = wa.right - wa.left;
    g_screenH = wa.bottom - wa.top;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kOverlayClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW mc = {};
    mc.cbSize        = sizeof(mc);
    mc.lpfnWndProc   = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_HOTKEY && w == 1) { PostQuitMessage(0); return 0; }
        if (m == WM_DESTROY)          { PostQuitMessage(0); return 0; }
        return DefWindowProcW(h, m, w, l);
        };
    mc.hInstance     = hInstance;
    mc.lpszClassName = kMasterClass;
    RegisterClassExW(&mc);

    HWND master = CreateWindowExW(0, kMasterClass, L"", WS_POPUP,
                                  0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);

    // ---- 注册退出热键: 逐个候选尝试, 全失败也要能告警而不是变成"退不掉" ----
    const wchar_t* hotkeyName = nullptr;
    if (master) {
        for (const auto& hk : kHotkeys) {
            if (RegisterHotKey(master, 1, hk.mods | MOD_NOREPEAT, hk.vk)) {
                hotkeyName = hk.name;
                break;
            }
        }
    }
    if (!hotkeyName) {
        MessageBoxW(nullptr,
            L"注册退出热键失败（组合键可能已被其它程序占用）。\n\n"
            L"程序仍会正常运行，但请用任务管理器\n"
            L"结束 FallingImages.exe 来退出。",
            L"Falling Images", MB_OK | MB_ICONWARNING);
    }

    g_overlayHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kOverlayClass, L"", WS_POPUP,
        g_workX, g_workY, g_screenW, g_screenH,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_overlayHwnd) {
        MessageBoxW(nullptr, L"无法创建 overlay 窗口", L"错误", MB_OK);
        if (hotkeyName) UnregisterHotKey(master, 1);
        if (master) DestroyWindow(master);
        ShutdownWIC();
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    if (!InitComposeBuffer(g_screenW, g_screenH)) {
        MessageBoxW(nullptr, L"无法创建合成缓冲", L"错误", MB_OK);
        DestroyWindow(g_overlayHwnd);
        if (hotkeyName) UnregisterHotKey(master, 1);
        if (master) DestroyWindow(master);
        ShutdownWIC();
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    ShowWindow(g_overlayHwnd, SW_SHOWNA);

    LoadImages();

    if (g_assets.empty()) {
        std::wstring msg =
            L"没有找到可用图片！\n\n"
            L"请在 exe 同目录下建一个 images 文件夹，\n"
            L"放入 1~" + std::to_wstring(MAX_IMAGES) +
            L" 张 png / jpg / bmp / gif / webp 图片。\n\n"
            L"每张图片存活 " + std::to_wstring((int)SPRITE_LIFE_SEC) +
            L" 秒后爆炸；\n被以特别快的速度撞击会提前 " +
            std::to_wstring((int)FAST_HIT_LIFE_LOSS) + L" 秒爆炸。";
        if (!g_failedFiles.empty()) {
            msg += L"\n\n以下文件无法解码：\n";
            for (size_t i = 0; i < g_failedFiles.size() && i < 8; ++i)
                msg += L"  · " + g_failedFiles[i] + L"\n";
        }
        MessageBoxW(nullptr, msg.c_str(), L"Falling Images", MB_OK | MB_ICONINFORMATION);

        FreeComposeBuffer();
        DestroyWindow(g_overlayHwnd);
        if (hotkeyName) UnregisterHotKey(master, 1);
        if (master) DestroyWindow(master);
        ShutdownWIC();
        ReleaseDC(nullptr, g_screenDC);
        GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    g_spawnTimer = 0.8f;

    // ---- 装上低级键盘钩子(按 "." 在鼠标位置生成) ----
    // 放在这里是为了让上面几条出错退出的分支不用管它。
    // 关掉手动生成就连钩子都不装 —— 免得白白往系统里挂一个全局键盘钩子。
    // 失败也不弹窗: 只是少一个手动生成的功能, 程序照常跑。
    if (MANUAL_SPAWN_ENABLED)
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);

    bool running = true;
    MSG msg = {};
    while (running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)   { running = false; break; }
            if (msg.message == WM_HOTKEY) { running = false; break; }  // 兜底
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart);
        prev = now;
        if (dt > 0.1f) dt = 0.1f;

        // ---- 分辨率 / 工作区变化检查 ----
        g_displayTimer -= dt;
        if (g_displayTimer <= 0.0f) {
            g_displayTimer = 1.0f;
            if (!CheckDisplayChange()) { running = false; break; }
        }

        UpdateAnimations(dt);

        // ---- 按 "." 在鼠标位置生成一张随机图片 ----
        // 钩子回调里只记了位置, 真正干活放在这里。
        // 鼠标坐标是屏幕坐标, 减去 overlay 原点换成合成缓冲坐标。
        if (MANUAL_SPAWN_ENABLED && g_spawnAtMouse) {
            g_spawnAtMouse = false;
            SpawnFalling(true,
                         (float)(g_spawnPoint.x - g_workX),
                         (float)(g_spawnPoint.y - g_workY));
        }

        // ---- 自动随机下落生成 ----
        if (AUTO_SPAWN_ENABLED) {
            g_spawnTimer -= dt;
            if (g_spawnTimer <= 0.0f) {
                SpawnFalling();
                g_spawnTimer = RandF(0.9f, 2.0f);
            }
        }

        UpdatePhysics(dt);
        PresentAll();

        Sleep(1);
    }

    g_sprites.clear();

    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }

    for (auto& a : g_assets) {
        delete a->bmp;
        delete a->raw;
    }
    g_assets.clear();

    FreeComposeBuffer();
    if (g_overlayHwnd) DestroyWindow(g_overlayHwnd);

    if (master) {
        if (hotkeyName) UnregisterHotKey(master, 1);
        DestroyWindow(master);
    }

    ShutdownWIC();
    ReleaseDC(nullptr, g_screenDC);
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}
