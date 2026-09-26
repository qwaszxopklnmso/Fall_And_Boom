// ============================================================
//  Falling Images  -  Win32 + GDI+ + WIC   (单窗口合成版)
//  编译: 双击 build.bat  (推荐)
//        或手动:
//        cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE main.cpp ^
//           /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib ole32.lib windowscodecs.lib
//        注意: /utf-8 必须加, 否则中文字符串在 GBK 代码页下会编译报错;
//              user32/gdi32/gdiplus/windowscodecs 也必须显式写出, cl 不会自动链接.
//  退出: Ctrl+Alt+Q
// ============================================================

#define NOMINMAX

#include <windows.h>
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

using namespace Gdiplus;

// ------------------------------------------------------------
// 可调参数
// ------------------------------------------------------------
static const wchar_t* kOverlayClass = L"FallingImgOverlay";
static const wchar_t* kMasterClass  = L"FallingImgMaster";

static const int   MAX_IMAGES      = 6;
static const int   MAX_FALLING     = 12;
static const int   MAX_TOTAL       = 220;
static const int   DEBRIS_COLS     = 3;
static const int   DEBRIS_ROWS     = 3;
static const float FALL_SPEED_MIN  = 60.0f;
static const float FALL_SPEED_MAX  = 480.0f;
static const int   SPRITE_SIZE     = 32;
static const float WINDOW_SCAN_SEC = 0.10f;   // 窗口扫描间隔
static const float FOLLOW_MAX_VX   = 250.0f;  // 超过这个速度就不再"拖着走", 改为创飞
static const float SWEEP_MIN_VX    = 250.0f;  // 认定为"创飞"的窗口速度阈值

static const float EXPLOSION_RADIUS    = 150.0f;  // 爆炸冲击波半径(像素)
static const float EXPLOSION_POWER_MIN = 260.0f;  // 冲击波初速(范围边缘)
static const float EXPLOSION_POWER_MAX = 640.0f;  // 冲击波初速(爆心附近)
static const float EXPLOSION_GRAVITY   = 1300.0f; // 被炸飞之后的重力

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

// EnumWindows 按 Z 序从顶层到底层枚举
static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto* list = reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (IsIconic(hwnd))         return TRUE;

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    if (wcscmp(cls, kOverlayClass) == 0) return TRUE;
    if (wcscmp(cls, kMasterClass)  == 0) return TRUE;
    if (wcscmp(cls, L"Progman")    == 0) return TRUE;
    if (wcscmp(cls, L"WorkerW")    == 0) return TRUE;
    if (wcscmp(cls, L"Shell_TrayWnd") == 0) return TRUE;

    RECT r;
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if (r.right <= r.left || r.bottom <= r.top) return TRUE;
    // 只有"完全落在工作区上下方之外"的窗口才能直接丢掉。
    // 注意不能写成 r.top < 0: 最大化窗口的 GetWindowRect 会把不可见的
    // DWM 调整边框算进去(Top 常为 -7), 那样会把正在铺满屏幕的窗口丢掉,
    // 导致它背后被完全遮盖的窗口重新获得物理碰撞。
    if (r.top >= g_screenH || r.bottom <= 0) return TRUE;

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
        RECT inter;
        if (!IntersectRect(&inter, &wi.rect, &r)) continue;
        if (inter.left   <= r.left  && inter.right  >= r.right &&
            inter.top    <= r.top   && inter.bottom >= r.bottom) {
            return TRUE;                       // 完全被覆盖, 直接跳过
        }
        occ.push_back(inter);
    }

    WindowInfo wi;
    wi.hwnd      = hwnd;
    wi.rect      = r;
    wi.prevRect  = r;
    wi.hasPrev   = false;
    wi.platform  = (!fullscreen && r.top >= 0);
    wi.occluders = std::move(occ);
    list->push_back(std::move(wi));
    return TRUE;
}

// 判断窗口顶边上横坐标 cx 这一点是否真的可见。
// 被更高 Z 序窗口压住的部分不能当平台，否则图片会停在看不见的边上，看起来像悬空。
static bool TopEdgeVisibleAt(const WindowInfo& wv, float cx)
{
    if (wv.occluders.empty()) return true;
    const LONG px = (LONG)std::lround(cx);
    const LONG py = wv.rect.top + 1;
    for (const RECT& o : wv.occluders) {
        if (px >= o.left && px < o.right && py >= o.top && py < o.bottom)
            return false;
    }
    return true;
}

static void ScanWindows()
{
    // 完整重建列表(EnumWindows 天然按 Z 序), 再回填上一次的 rect 用于算速度
    std::vector<WindowInfo> oldList;
    oldList.swap(g_windowList);
    g_windowList.clear();

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
    float fadeStartLife = 0.0f;

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
static void SpawnFalling()
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
    s->x = RandF(0.0f, (float)std::max(1, g_screenW - SPRITE_SIZE));
    s->y = -(float)SPRITE_SIZE;
    s->baseFallSpeed = RandF(FALL_SPEED_MIN, FALL_SPEED_MAX);
    s->vy = s->baseFallSpeed;
    s->vx = 0.0f;
    s->gravity = 0.0f;
    s->maxLife = s->life = a.life;
    s->alpha = 1.0f;
    s->fadeStartLife = 0.0f;

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
            d->fadeStartLife = d->maxLife;

            out.push_back(std::move(d));
        }
    }
}

// ------------------------------------------------------------
// 爆炸冲击波: 把半径内的其它精灵一起炸飞
// 注意: 这里刻意不碰 fadeStartLife —— 被炸飞不应该让图片变浅。
//       (窗口"创飞"是会变浅的, 冲击波不会, 两者行为不同)
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
// ------------------------------------------------------------
static void CollideWithWindows(Sprite* s)
{
    for (auto& wv : g_windowList) {
        if (!wv.platform) continue;           // 全屏 / 顶边在屏幕外 -> 不当作平台
        const RECT& wr = wv.rect;
        if (s->x + s->w <= wr.left || s->x >= wr.right) continue;
        if (s->y + s->h <= wr.top  || s->y >= wr.bottom) continue;

        // 四个方向的穿透深度
        float pTop    = (s->y + s->h) - wr.top;
        float pBottom = wr.bottom - s->y;
        float pLeft   = (s->x + s->w) - wr.left;
        float pRight  = wr.right - s->x;

        float minP = pTop;
        int axis = 0; // 0=上方 1=下方 2=左侧 3=右侧
        if (pBottom < minP) { minP = pBottom; axis = 1; }
        if (pLeft   < minP) { minP = pLeft;   axis = 2; }
        if (pRight  < minP) { minP = pRight;  axis = 3; }

        if (axis == 0) {
            // 顶边被更高 Z 序窗口压住的那一段不是平台, 不要在这一格停下
            if (!TopEdgeVisibleAt(wv, s->x + s->w * 0.5f)) continue;

            // 从上方落到窗口顶部
            s->y = (float)wr.top - s->h;
            if (s->vy > 0) {
                s->vy = -s->vy * 0.35f;
                if (fabsf(s->vy) < 50.0f) s->vy = 0.0f;
            }
            s->vx *= 0.85f;
            if (fabsf(s->vx) < 20.0f) s->vx = 0.0f;
        } else if (axis == 1) {
            // 从下方撞到窗口底部
            s->y = (float)wr.bottom;
            if (s->vy < 0) s->vy = -s->vy * 0.4f;
        } else if (axis == 2) {
            // 从左侧撞到窗口右边缘
            s->x = (float)wr.right;
            if (s->vx < 0) s->vx = -s->vx * 0.5f;
        } else {
            // 从右侧撞到窗口左边缘
            s->x = (float)wr.left - s->w;
            if (s->vx > 0) s->vx = -s->vx * 0.5f;
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
                    if (s->fadeStartLife <= 0.0f)
                        s->fadeStartLife = s->life;
                    break;
                }
            }

            if (s->gravity > 0.0f) {
                // ---- 抛物线（被弹飞/被创飞） ----
                s->vy += s->gravity * dt;
                s->x  += s->vx * dt;
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
                CollideWithWindows(s);

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
                        if (wv.hwnd == s->restingOn && wv.platform) { host = &wv; break; }
                    }
                    if (host) {
                        const bool overlapX = !(s->x + s->w <= host->rect.left ||
                                                s->x >= host->rect.right);
                        const float ny = (float)host->rect.top - (float)s->h;
                        // 顶边被压住 / 被拖出屏幕上方 -> 放掉, 重新下落
                        if (overlapX && ny >= 0.0f &&
                            TopEdgeVisibleAt(*host, s->x + s->w * 0.5f))
                        {
                            s->y = ny;                       // 垂直跟随(上移会把它顶上去)
                            if (host->hasPrev) {
                                const float winVx =
                                    (float)(host->rect.left - host->prevRect.left) / g_scanDt;
                                if (fabsf(winVx) >= 1.0f && fabsf(winVx) < FOLLOW_MAX_VX)
                                    s->x += winVx * dt;      // 水平跟随
                            }
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

                    // 落到某个窗口"看得见"的顶边上
                    const float cx = s->x + s->w * 0.5f;
                    for (auto& wv : g_windowList) {
                        if (!wv.platform) continue;
                        const RECT& wr = wv.rect;
                        if (s->x + s->w <= wr.left || s->x >= wr.right) continue;
                        if (!TopEdgeVisibleAt(wv, cx)) continue;   // 顶边被压住, 不是平台
                        const float wTop = (float)wr.top;
                        if (s->y + s->h > wTop && prevBottom <= wTop) {
                            s->y = wTop - s->h;
                            s->vy = 0.0f;
                            s->restingOn = wv.hwnd;
                            break;
                        }
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
            s->y  += s->vy * dt;
            s->life -= dt;

            if (s->x < 0) { s->x = 0; s->vx = -s->vx * 0.7f; }
            if (s->x + s->w > g_screenW) { s->x = (float)(g_screenW - s->w); s->vx = -s->vx * 0.7f; }
            if (s->y < 0) { s->y = 0; s->vy = -s->vy * 0.7f; }
            if (s->y + s->h > g_screenH) {
                s->y = (float)(g_screenH - s->h);
                if (s->vy > 0) s->vy = -s->vy * 0.35f;
                if (fabsf(s->vy) < 40.0f) s->vy = 0.0f;
                s->vx *= 0.85f;
                if (fabsf(s->vx) < 15.0f) s->vx = 0.0f;
            }
        }

        // 透明度更新
        if (s->fadeStartLife > 0.0f) {
            float na = s->life / s->fadeStartLife;
            if (na < 0.0f) na = 0.0f;
            if (fabsf(na - s->alpha) > 0.01f) {
                s->alpha = na;
            }
        }
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

            if (a->fadeStartLife <= 0.0f) a->fadeStartLife = a->life;
            if (b->fadeStartLife <= 0.0f) b->fadeStartLife = b->life;
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

    const float pool[3] = { 3.0f, 5.0f, 10.0f };

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
        asset->life  = pool[g_assets.size() % 3];

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
            L"放入 1~6 张 png / jpg / bmp / gif / webp 图片。\n\n"
            L"文件名按字母排序，第 1/4 张 = 3 秒，第 2/5 张 = 5 秒，第 3/6 张 = 10 秒后爆炸。";
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

        g_spawnTimer -= dt;
        if (g_spawnTimer <= 0.0f) {
            SpawnFalling();
            g_spawnTimer = RandF(0.9f, 2.0f);
        }

        UpdatePhysics(dt);
        PresentAll();

        Sleep(1);
    }

    g_sprites.clear();

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
