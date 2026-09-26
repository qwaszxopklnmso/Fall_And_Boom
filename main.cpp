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
#include <cwchar>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

// ------------------------------------------------------------
// 可调参数(默认值)
// ------------------------------------------------------------
// ★ 这些参数的实际取值来自 exe 同目录的 config.ini —— 改完重启程序即可生效,
//   不需要重新编译。config.ini 不存在时程序会自动按下面这些默认值生成一份,
//   里面带中文说明; 已有的文件永远不会被覆盖。
//   配置文件按 "通用 / 方块 blocks / 爆炸物 explosives" 分了三段。
static const wchar_t* kOverlayClass = L"FallingImgOverlay";
static const wchar_t* kMasterClass  = L"FallingImgMaster";

// ---- 通用 ----
static float SPAWN_FIRST_DELAY = 0.8f;// 启动后第一张图片出现前的等待(秒)
static float DROPTIME_MIN    = 0.2f;  // ★ 两次自动生成的最小间隔(秒)
static float DROPTIME_MAX    = 1.3f;  // ★ 两次自动生成的最大间隔(秒)
static float WINDOW_SCAN_SEC = 0.09f; // 窗口扫描间隔
static float FOLLOW_SANITY_VX = 20000.0f; // 跟随窗口时的荒谬值上限(挡矩形抖动)
static float SWEEP_MIN_VX    = 260.0f;// 认定为"创飞"的窗口速度阈值
static int   MAX_FALLING     = 20;    // 同屏"正在下落"的数量上限(方块 + 爆炸物)
static int   MAX_TOTAL       = 220;   // 精灵总数上限(含爆炸碎片)
static int   DEBRIS_COLS     = 4;     // 爆炸碎片列数
static int   DEBRIS_ROWS     = 4;     // 爆炸碎片行数

// 生成方式开关:
// 两个都开 = 平时自动随机下落, 想手动补一张就按 "."。
// 关掉 AUTO  = 屏幕上一直干干净净, 只有按 "." 才会出现图片。
// 关掉 MANUAL= 只能等自动随机下落, 连键盘钩子都不会装。
// 两个都关   = 什么都不会生成(退出热键仍然有效)。
static bool  AUTO_SPAWN_ENABLED   = true;   // ★ 自动随机下落生成
static bool  MANUAL_SPAWN_ENABLED = true;   // ★ 按后面两节里配的键手动生成

// ---- 手动生成 ----
// 两个键各管一类: 一个只出方块, 一个只出爆炸物。
// 用低级键盘钩子而不是 RegisterHotKey: 注册成热键会把这个键从所有程序那里
// 抢走(打字、输入符号就全废了), 钩子只是旁听, 按键照样传给别的程序。
static UINT  BLOCK_SPAWN_KEY_VK     = VK_OEM_COMMA;    // 主键盘区的 ","
static UINT  EXPLOSIVE_SPAWN_KEY_VK = VK_OEM_PERIOD;   // 主键盘区的 "."

// 手动生成请求的类别(SPAWN_RANDOM 只用于自动生成)
enum SpawnKind { SPAWN_RANDOM = 0, SPAWN_BLOCK, SPAWN_EXPLOSIVE, SPAWN_KIND_COUNT };

// 两次触发的最小间隔(毫秒)。长按的自动重复间隔只有 ~30ms, 会被这条挡掉;
// 而且它是"比时间戳"而不是"记按键状态", 所以就算漏掉一次 KEYUP 也不会卡死。
static DWORD SPAWN_KEY_DEBOUNCE_MS = 250;

// ---- 方块 blocks (images\blocks) ----
// 方块**没有引信**: 不会自己爆炸, 也不会闪。
// 它只会被爆炸波及 —— 爆炸范围内的方块会被炸成碎片(见 BLOCK_DESTROY_IN_BLAST),
// 但被炸掉的方块自己**不会再炸**: 就这么一层, 不连锁。
// 停稳之后 BLOCK_STAY_SEC 秒消失(淡出, 不是爆炸), 免得屏幕越堆越满。
static bool  BLOCKS_ENABLED         = true;   // ★ 关掉就完全不生成方块
static int   BLOCK_MAX_IMAGES       = 8;      // ★ blocks 文件夹最多读几张
static int   BLOCK_SIZE             = 48;     // 方块显示边长(像素)
static float BLOCK_SPAWN_WEIGHT     = 1.0f;   // ★ 生成权重, 和爆炸物按比例随机
static float BLOCK_FALL_SPEED_MIN   = 450.0f; // 方块下落速度下限(像素/秒)
static float BLOCK_FALL_SPEED_MAX   = 700.0f; // 方块下落速度上限(像素/秒)
static float BLOCK_STAY_SEC         = 12.0f;  // 停稳后停留几秒消失(0 = 一直留着)
static float BLOCK_FADE_SEC         = 1.5f;   // 消失前的淡出时长(秒)
static bool  BLOCK_DESTROY_IN_BLAST = true;   // ★ 爆炸范围内的方块会被炸掉
// 碰撞反馈: 被撞/被创飞时"闪一下"再恢复(不再按剩余寿命长时间发灰)
static float BLOCK_HIT_FLASH_SEC    = 0.40f;  // 方块"闪一下"的总时长(秒, 0 = 不闪)
static float BLOCK_HIT_DIM          = 0.50f;  // 方块闪到最浅时的 alpha

// ---- 爆炸物 explosives (images\explosives) ----
static bool  EXPLOSIVES_ENABLED       = true; // ★ 关掉就只剩方块
static int   EXPLOSIVE_MAX_IMAGES     = 15;   // ★ explosives 文件夹最多读几张
static int   EXPLOSIVE_SIZE           = 48;   // 爆炸物显示边长(像素)
static float EXPLOSIVE_SPAWN_WEIGHT   = 1.0f; // ★ 生成权重, 和方块按比例随机
static float EXPLOSIVE_FALL_SPEED_MIN = 450.0f; // 爆炸物下落速度下限(像素/秒)
static float EXPLOSIVE_FALL_SPEED_MAX = 700.0f; // 爆炸物下落速度上限(像素/秒)
static float EXPLOSIVE_HIT_FLASH_SEC  = 0.40f;  // 爆炸物"闪一下"的总时长(秒, 0 = 不闪)
static float EXPLOSIVE_HIT_DIM        = 0.50f;  // 爆炸物闪到最浅时的 alpha

static float EXPLOSIVE_LIFE_SEC   = 5.0f;   // ★ 引信: 生成后几秒爆炸
static float EXPLODE_WARN_SEC     = 3.0f;   // ★ 爆炸前多少秒开始闪烁
static float EXPLODE_BLINK_PERIOD = 0.6f;   // ★ 闪烁周期(秒)
static float EXPLODE_BLINK_MIN    = 0.45f;  // 闪烁时最浅的 alpha

static float EXPLOSION_RADIUS    = 310.0f;  // 爆炸冲击波半径(像素)
static float EXPLOSION_POWER_MIN = 500.0f;  // 冲击波初速(范围边缘)
static float EXPLOSION_POWER_MAX = 900.0f;  // 冲击波初速(爆心附近)
static float EXPLOSION_GRAVITY   = 1350.0f; // 被炸飞之后的重力

static float FAST_HIT_SPEED     = 1650.0f; // ★ 相对速度超过它算"特别快的撞击"(像素/秒)
                                           //   定得比自由落体上限高, 所以普通下落互撞
                                           //   永远不会触发, 只有被撞飞/炸飞后才够
static float FAST_HIT_LIFE_LOSS = 2.5f;    // ★ 高速撞击扣掉的引信时间(秒)

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
// config.ini: 启动时读一次, 覆盖上面的默认值
// ------------------------------------------------------------
// 只认 "键 = 值" 这一种行:
//   · 键名忽略大小写、下划线、横线和空格(MAX_IMAGES / max-images / max images 等价)
//   · '#' 或 ';' 开头的整行是注释; 行中间只有前面是空白才算注释
//     (这样 BLOCK_SPAWN_KEY = ";" 这种值不会被误伤)
//   · 认不出来的键、解析不了的值会被跳过并提示一次 —— 写错一个字符不影响启动
//   · 删掉某一行 = 该参数用程序内置的默认值
// 设计上刻意做成"坏值只影响这一项": 任何异常都能退回默认值继续跑。
enum CfgKind { CFG_INT, CFG_FLOAT, CFG_BOOL, CFG_UINT, CFG_KEY };

struct CfgEntry {
    const wchar_t* key;      // 键名(写进 config.ini 的形式)
    CfgKind        kind;
    void*          ptr;
    const wchar_t* comment;  // 写进 config.ini 的说明
    int            group;    // 分组, 用来插小标题
};

struct CfgGroup {
    const wchar_t* title;   // 写进 config.ini 的分节标题
    const wchar_t* descr;   // 标题下面那句说明
};

// 三个分节: 通用 / 方块 / 爆炸物。
// config.ini 里用 "# --- 方块 blocks ---" 这样的注释行把三段分开。
static const CfgGroup kCfgGroups[] = {
    { L"通用 common",
      L"两边都适用的东西: 生成节奏、窗口扫描、碰撞反馈、按键" },
    { L"方块 blocks  (images\\blocks)",
      L"不会自己爆炸, 也不会闪; 只有被爆炸波及时才会炸成碎片" },
    { L"爆炸物 explosives  (images\\explosives)",
      L"有引信, 时间到了自己爆炸, 并把范围内的一切炸飞" },
};

static const CfgEntry kCfgTable[] = {
    // ---------------- 通用 ----------------
    { L"SPAWN_FIRST_DELAY",     CFG_FLOAT, &SPAWN_FIRST_DELAY,     L"启动后第一张图片出现前的等待, 秒", 0 },
    { L"DROPTIME_MIN",          CFG_FLOAT, &DROPTIME_MIN,          L"两次自动生成的最小间隔, 秒(掉落快慢主要看这两个)", 0 },
    { L"DROPTIME_MAX",          CFG_FLOAT, &DROPTIME_MAX,          L"两次自动生成的最大间隔, 秒", 0 },
    { L"WINDOW_SCAN_SEC",       CFG_FLOAT, &WINDOW_SCAN_SEC,       L"窗口位置扫描间隔, 秒(越小越跟手, 也越费 CPU)", 0 },
    { L"SWEEP_MIN_VX",          CFG_FLOAT, &SWEEP_MIN_VX,          L"窗口横向速度超过它就把图片\"创飞\", 像素/秒", 0 },
    { L"FOLLOW_SANITY_VX",      CFG_FLOAT, &FOLLOW_SANITY_VX,      L"跟随窗口时的荒谬速度上限, 挡窗口矩形抖动用", 0 },
    { L"MAX_FALLING",           CFG_INT,   &MAX_FALLING,           L"同屏\"正在下落\"的数量上限(方块 + 爆炸物)", 0 },
    { L"MAX_TOTAL",             CFG_INT,   &MAX_TOTAL,             L"精灵总数上限(含爆炸碎片)", 0 },
    { L"DEBRIS_COLS",           CFG_INT,   &DEBRIS_COLS,           L"爆炸碎片列数", 0 },
    { L"DEBRIS_ROWS",           CFG_INT,   &DEBRIS_ROWS,           L"爆炸碎片行数", 0 },
    { L"AUTO_SPAWN_ENABLED",    CFG_BOOL,  &AUTO_SPAWN_ENABLED,    L"自动随机下落 true / false", 0 },
    { L"MANUAL_SPAWN_ENABLED",  CFG_BOOL,  &MANUAL_SPAWN_ENABLED,  L"按下面两节里配的键手动生成 true / false", 0 },
    { L"SPAWN_KEY_DEBOUNCE_MS", CFG_UINT,  &SPAWN_KEY_DEBOUNCE_MS, L"同一个键两次触发的最小间隔, 毫秒(挡长按重复)", 0 },

    // ---------------- 方块 blocks ----------------
    { L"BLOCKS_ENABLED",         CFG_BOOL,  &BLOCKS_ENABLED,         L"false = 完全不生成方块(blocks 文件夹也不会读)", 1 },
    { L"BLOCK_MAX_IMAGES",       CFG_INT,   &BLOCK_MAX_IMAGES,       L"blocks 文件夹最多读几张(按文件名排序)", 1 },
    { L"BLOCK_SIZE",             CFG_INT,   &BLOCK_SIZE,             L"方块显示边长, 像素(会自动缩放)", 1 },
    { L"BLOCK_SPAWN_WEIGHT",     CFG_FLOAT, &BLOCK_SPAWN_WEIGHT,     L"每张方块图的生成权重, 0 = 不出方块", 1 },
    { L"BLOCK_SPAWN_KEY",        CFG_KEY,   &BLOCK_SPAWN_KEY_VK,     L"按这个键在鼠标位置生成一个方块(一个字符或虚拟键码数字)", 1 },
    { L"BLOCK_FALL_SPEED_MIN",   CFG_FLOAT, &BLOCK_FALL_SPEED_MIN,   L"方块下落速度下限, 像素/秒", 1 },
    { L"BLOCK_FALL_SPEED_MAX",   CFG_FLOAT, &BLOCK_FALL_SPEED_MAX,   L"方块下落速度上限, 像素/秒", 1 },
    { L"BLOCK_STAY_SEC",         CFG_FLOAT, &BLOCK_STAY_SEC,         L"方块停稳后停留几秒消失(淡出, 不爆炸); 0 = 一直留着", 1 },
    { L"BLOCK_FADE_SEC",         CFG_FLOAT, &BLOCK_FADE_SEC,         L"方块消失前的淡出时长, 秒(0 = 直接不见)", 1 },
    { L"BLOCK_DESTROY_IN_BLAST", CFG_BOOL,  &BLOCK_DESTROY_IN_BLAST, L"爆炸范围内的方块会被炸掉 true / false", 1 },
    { L"BLOCK_HIT_FLASH_SEC",    CFG_FLOAT, &BLOCK_HIT_FLASH_SEC,    L"方块被撞后\"闪一下\"的时长, 秒(0 = 不闪)", 1 },
    { L"BLOCK_HIT_DIM",          CFG_FLOAT, &BLOCK_HIT_DIM,          L"方块闪到最浅时的 alpha, 0~1(1 = 看不出闪)", 1 },

    // ---------------- 爆炸物 explosives ----------------
    { L"EXPLOSIVES_ENABLED",       CFG_BOOL,  &EXPLOSIVES_ENABLED,       L"false = 完全不生成爆炸物(explosives 文件夹也不会读)", 2 },
    { L"EXPLOSIVE_MAX_IMAGES",     CFG_INT,   &EXPLOSIVE_MAX_IMAGES,     L"explosives 文件夹最多读几张(按文件名排序)", 2 },
    { L"EXPLOSIVE_SIZE",           CFG_INT,   &EXPLOSIVE_SIZE,           L"爆炸物显示边长, 像素(会自动缩放)", 2 },
    { L"EXPLOSIVE_SPAWN_WEIGHT",   CFG_FLOAT, &EXPLOSIVE_SPAWN_WEIGHT,   L"每张爆炸物图的生成权重, 0 = 不出爆炸物(和方块权重比大小)", 2 },
    { L"EXPLOSIVE_SPAWN_KEY",      CFG_KEY,   &EXPLOSIVE_SPAWN_KEY_VK,   L"按这个键在鼠标位置生成一个爆炸物(一个字符或虚拟键码数字)", 2 },
    { L"EXPLOSIVE_FALL_SPEED_MIN", CFG_FLOAT, &EXPLOSIVE_FALL_SPEED_MIN, L"爆炸物下落速度下限, 像素/秒", 2 },
    { L"EXPLOSIVE_FALL_SPEED_MAX", CFG_FLOAT, &EXPLOSIVE_FALL_SPEED_MAX, L"爆炸物下落速度上限, 像素/秒", 2 },
    { L"EXPLOSIVE_HIT_FLASH_SEC",  CFG_FLOAT, &EXPLOSIVE_HIT_FLASH_SEC,  L"爆炸物被撞后\"闪一下\"的时长, 秒(0 = 不闪)", 2 },
    { L"EXPLOSIVE_HIT_DIM",        CFG_FLOAT, &EXPLOSIVE_HIT_DIM,        L"爆炸物闪到最浅时的 alpha, 0~1(1 = 看不出闪)", 2 },
    { L"EXPLOSIVE_LIFE_SEC",       CFG_FLOAT, &EXPLOSIVE_LIFE_SEC,       L"引信: 生成后几秒自己爆炸", 2 },
    { L"EXPLODE_WARN_SEC",         CFG_FLOAT, &EXPLODE_WARN_SEC,         L"爆炸前多少秒开始闪烁(比引信长也没关系, 会自动截断)", 2 },
    { L"EXPLODE_BLINK_PERIOD",     CFG_FLOAT, &EXPLODE_BLINK_PERIOD,     L"闪烁周期, 秒", 2 },
    { L"EXPLODE_BLINK_MIN",        CFG_FLOAT, &EXPLODE_BLINK_MIN,        L"闪烁时最浅的 alpha, 0~1", 2 },
    { L"EXPLOSION_RADIUS",         CFG_FLOAT, &EXPLOSION_RADIUS,         L"爆炸冲击波半径, 像素", 2 },
    { L"EXPLOSION_POWER_MIN",      CFG_FLOAT, &EXPLOSION_POWER_MIN,      L"冲击波初速(范围边缘), 像素/秒", 2 },
    { L"EXPLOSION_POWER_MAX",      CFG_FLOAT, &EXPLOSION_POWER_MAX,      L"冲击波初速(爆心附近), 像素/秒", 2 },
    { L"EXPLOSION_GRAVITY",        CFG_FLOAT, &EXPLOSION_GRAVITY,        L"被炸飞之后的重力", 2 },
    { L"FAST_HIT_SPEED",           CFG_FLOAT, &FAST_HIT_SPEED,           L"相对速度超过它算\"特别快的撞击\", 像素/秒", 2 },
    { L"FAST_HIT_LIFE_LOSS",       CFG_FLOAT, &FAST_HIT_LIFE_LOSS,       L"高速撞击扣掉的引信时间, 秒(只对爆炸物有效)", 2 },
};

// ---- 小工具 ----
static std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring d(buf, n);
    const size_t p = d.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? d : d.substr(0, p);
}

static std::wstring TrimW(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::wstring NormKey(const std::wstring& s)
{
    std::wstring r;
    for (wchar_t ch : s) {
        if (ch == L' ' || ch == L'\t' || ch == L'-' || ch == L'_') continue;
        r.push_back((wchar_t)towupper(ch));
    }
    return r;
}

static std::wstring LowerW(const std::wstring& s)
{
    std::wstring r;
    for (wchar_t ch : s) r.push_back((wchar_t)towlower(ch));
    return r;
}

static bool ParseCfgInt(const std::wstring& v, int* out)
{
    wchar_t* end = nullptr;
    const long long x = wcstoll(v.c_str(), &end, 0);   // 支持 0x 前缀
    if (end == v.c_str()) return false;
    while (*end && iswspace(*end)) ++end;
    if (*end) return false;
    *out = (int)x;
    return true;
}

static bool ParseCfgUInt(const std::wstring& v, unsigned long* out)
{
    wchar_t* end = nullptr;
    const unsigned long long x = wcstoull(v.c_str(), &end, 0);
    if (end == v.c_str()) return false;
    while (*end && iswspace(*end)) ++end;
    if (*end) return false;
    *out = (unsigned long)x;
    return true;
}

static bool ParseCfgFloat(const std::wstring& v, float* out)
{
    wchar_t* end = nullptr;
    const double x = wcstod(v.c_str(), &end);
    if (end == v.c_str()) return false;
    while (*end && iswspace(*end)) ++end;
    if (*end) return false;
    *out = (float)x;
    return true;
}

static bool ParseCfgBool(const std::wstring& v, bool* out)
{
    const std::wstring s = LowerW(v);
    if (s == L"1" || s == L"true" || s == L"yes" || s == L"on" || s == L"开") { *out = true;  return true; }
    if (s == L"0" || s == L"false" || s == L"no" || s == L"off" || s == L"关") { *out = false; return true; }
    return false;
}

// 按键: 优先当成"一个字符"(交给 VkKeyScanW 翻译), 否则当成虚拟键码数字
static bool ParseCfgKey(const std::wstring& v, UINT* out)
{
    std::wstring s = v;
    if (s.size() >= 2 &&
        ((s.front() == L'"' && s.back() == L'"') || (s.front() == L'\'' && s.back() == L'\'')))
        s = s.substr(1, s.size() - 2);

    if (s.size() == 1) {
        const SHORT r = VkKeyScanW(s[0]);
        if (r == -1) return false;
        *out = (UINT)(r & 0xFF);
        return true;
    }
    int n = 0;
    if (ParseCfgInt(s, &n) && n > 0 && n < 256) { *out = (UINT)n; return true; }
    return false;
}

static std::string WToUtf8(const std::wstring& s)
{
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string r((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n, nullptr, nullptr);
    return r;
}

static std::wstring DecodeText(const std::string& raw)
{
    size_t off = 0;
    if (raw.size() >= 3 &&
        (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
        off = 3;

    const char* p = raw.data() + off;
    const int   len = (int)(raw.size() - off);
    if (len <= 0) return L"";

    // 先按 UTF-8 解; 解不动(比如记事本存成 GBK)就退回系统代码页。
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, nullptr, 0);
    UINT cp = CP_UTF8;
    if (need <= 0) { cp = CP_ACP; need = MultiByteToWideChar(cp, 0, p, len, nullptr, 0); }
    if (need <= 0) return L"";

    std::wstring w((size_t)need, L'\0');
    MultiByteToWideChar(cp, 0, p, len, &w[0], need);
    return w;
}

static bool ReadWholeFile(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 4 * 1024 * 1024) { CloseHandle(h); return false; }

    out.assign((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    const bool ok = out.empty() ||
        (ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size());
    CloseHandle(h);
    return ok;
}

// 把当前值写回 config.ini 的文本形式
static std::wstring CfgValueText(const CfgEntry& e)
{
    wchar_t buf[64] = {};
    switch (e.kind) {
    case CFG_INT:   swprintf(buf, 64, L"%d", *(const int*)e.ptr);   return buf;
    case CFG_UINT:  swprintf(buf, 64, L"%lu", *(const DWORD*)e.ptr); return buf;
    case CFG_FLOAT: swprintf(buf, 64, L"%g", (double)*(const float*)e.ptr); return buf;
    case CFG_BOOL:  return *(const bool*)e.ptr ? L"true" : L"false";
    case CFG_KEY: {
        const UINT vk = *(const UINT*)e.ptr;
        static const wchar_t* kCand =
            L"abcdefghijklmnopqrstuvwxyz0123456789.,;'[]\\/-=`";
        for (const wchar_t* p = kCand; *p; ++p) {
            const SHORT r = VkKeyScanW(*p);
            if (r != -1 && (UINT)(r & 0xFF) == vk) {
                std::wstring s = L"\"";
                s.push_back(*p);
                s.push_back(L'"');
                return s;
            }
        }
        swprintf(buf, 64, L"%u", vk);
        return buf;
    }
    }
    return L"";
}

static bool WriteDefaultConfig(const std::wstring& path)
{
    std::wstring t;
    t += L"# ============================================================\r\n";
    t += L"#  Falling Images  配置文件\r\n";
    t += L"#  改完保存 -> 重启程序生效 (不用重新编译, 也不用重新下载)\r\n";
    t += L"#\r\n";
    t += L"#  格式:   键 = 值\r\n";
    t += L"#  '#' 或 ';' 开头的整行是注释; 行中间只有前面是空白才算注释\r\n";
    t += L"#  删掉某一行 = 该参数用程序内置的默认值\r\n";
    t += L"#  写错的键/值会被忽略并在启动时提示, 不影响程序运行\r\n";
    t += L"#\r\n";
    t += L"#  图片分两类, 各放一个文件夹:\r\n";
    t += L"#     images\\blocks      方块    —— 不会自己爆炸, 只会被炸掉\r\n";
    t += L"#     images\\explosives  爆炸物  —— 有引信, 到点自己爆炸\r\n";
    t += L"#     images 根目录下的图片按\"爆炸物\"处理(兼容老版本)\r\n";
    t += L"#\r\n";
    t += L"#  本文件由程序自动生成, 只在它不存在时创建, 永远不会覆盖你的修改。\r\n";
    t += L"# ============================================================\r\n";

    int lastGroup = -1;
    for (const auto& e : kCfgTable) {
        if (e.group != lastGroup) {
            lastGroup = e.group;
            t += L"\r\n# ------------------------------------------------------------\r\n";
            t += L"# --- ";
            t += kCfgGroups[e.group].title;
            t += L"\r\n#     ";
            t += kCfgGroups[e.group].descr;
            t += L"\r\n# ------------------------------------------------------------\r\n";
        }
        std::wstring line = e.key;
        line += L" = ";
        line += CfgValueText(e);
        while (line.size() < 34) line.push_back(L' ');
        line += L"# ";
        line += e.comment;
        line += L"\r\n";
        t += line;
    }

    std::string bytes;
    bytes += "\xEF\xBB\xBF";          // UTF-8 BOM, 让记事本认得出中文注释
    bytes += WToUtf8(t);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);   // CREATE_NEW: 绝不覆盖
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

static bool ApplyCfgValue(const CfgEntry& e, const std::wstring& v)
{
    switch (e.kind) {
    case CFG_INT:   { int x;                  if (!ParseCfgInt(v, &x))   return false; *(int*)e.ptr   = x; return true; }
    case CFG_UINT:  { unsigned long x;        if (!ParseCfgUInt(v, &x))  return false; *(DWORD*)e.ptr = (DWORD)x; return true; }
    case CFG_FLOAT: { float x;                if (!ParseCfgFloat(v, &x)) return false; *(float*)e.ptr = x; return true; }
    case CFG_BOOL:  { bool x;                 if (!ParseCfgBool(v, &x))  return false; *(bool*)e.ptr  = x; return true; }
    case CFG_KEY:   { UINT x;                 if (!ParseCfgKey(v, &x))   return false; *(UINT*)e.ptr  = x; return true; }
    }
    return false;
}

// 兜底: 把明显不合理的值夹回可用范围, 免得写错一个数就让程序崩掉/卡死
static void SanitizeConfig()
{
    auto clampI = [](int& v, int lo, int hi) { if (v < lo) v = lo; if (v > hi) v = hi; };
    auto clampF = [](float& v, float lo, float hi) {
        if (!(v == v)) v = lo;                       // NaN
        if (v < lo) v = lo;
        if (v > hi) v = hi;
    };

    clampI(MAX_FALLING, 1, 2000);
    clampI(MAX_TOTAL,   8, 20000);
    clampI(DEBRIS_COLS, 1, 16);
    clampI(DEBRIS_ROWS, 1, 16);

    clampF(SPAWN_FIRST_DELAY, 0.0f, 600.0f);
    clampF(DROPTIME_MIN, 0.02f, 600.0f);
    clampF(DROPTIME_MAX, 0.02f, 600.0f);
    if (DROPTIME_MAX < DROPTIME_MIN) std::swap(DROPTIME_MAX, DROPTIME_MIN);

    clampF(WINDOW_SCAN_SEC,  0.01f, 2.0f);
    clampF(FOLLOW_SANITY_VX, 100.0f, 10000000.0f);
    clampF(SWEEP_MIN_VX,     0.0f, 1000000.0f);

    // ---- 方块 ----
    clampI(BLOCK_MAX_IMAGES, 1, 512);
    clampI(BLOCK_SIZE,       8, 512);
    clampF(BLOCK_SPAWN_WEIGHT, 0.0f, 1000000.0f);
    clampF(BLOCK_FALL_SPEED_MIN, 0.0f, 20000.0f);
    clampF(BLOCK_FALL_SPEED_MAX, 0.0f, 20000.0f);
    if (BLOCK_FALL_SPEED_MAX < BLOCK_FALL_SPEED_MIN)
        std::swap(BLOCK_FALL_SPEED_MAX, BLOCK_FALL_SPEED_MIN);
    clampF(BLOCK_STAY_SEC, 0.0f, 36000.0f);
    clampF(BLOCK_FADE_SEC, 0.0f, 600.0f);
    clampF(BLOCK_HIT_FLASH_SEC, 0.0f, 60.0f);
    clampF(BLOCK_HIT_DIM,       0.0f, 1.0f);

    // ---- 爆炸物 ----
    clampI(EXPLOSIVE_MAX_IMAGES, 1, 512);
    clampI(EXPLOSIVE_SIZE,       8, 512);
    clampF(EXPLOSIVE_SPAWN_WEIGHT, 0.0f, 1000000.0f);
    clampF(EXPLOSIVE_FALL_SPEED_MIN, 0.0f, 20000.0f);
    clampF(EXPLOSIVE_FALL_SPEED_MAX, 0.0f, 20000.0f);
    if (EXPLOSIVE_FALL_SPEED_MAX < EXPLOSIVE_FALL_SPEED_MIN)
        std::swap(EXPLOSIVE_FALL_SPEED_MAX, EXPLOSIVE_FALL_SPEED_MIN);

    clampF(EXPLOSION_RADIUS,    0.0f, 20000.0f);
    clampF(EXPLOSION_POWER_MIN, 0.0f, 20000.0f);
    clampF(EXPLOSION_POWER_MAX, 0.0f, 20000.0f);
    if (EXPLOSION_POWER_MAX < EXPLOSION_POWER_MIN) std::swap(EXPLOSION_POWER_MAX, EXPLOSION_POWER_MIN);
    clampF(EXPLOSION_GRAVITY, -20000.0f, 20000.0f);

    clampF(EXPLOSIVE_HIT_FLASH_SEC, 0.0f, 60.0f);
    clampF(EXPLOSIVE_HIT_DIM,       0.0f, 1.0f);

    clampF(EXPLODE_WARN_SEC,    0.0f, 3600.0f);
    clampF(EXPLODE_BLINK_PERIOD, 0.02f, 600.0f);
    clampF(EXPLODE_BLINK_MIN,   0.0f, 1.0f);

    clampF(EXPLOSIVE_LIFE_SEC, 0.2f, 36000.0f);
    clampF(FAST_HIT_SPEED,  0.0f, 1000000.0f);
    clampF(FAST_HIT_LIFE_LOSS, 0.0f, EXPLOSIVE_LIFE_SEC);

    // 两个都关掉就什么都生成不出来了 —— 至少留一类
    if (!BLOCKS_ENABLED && !EXPLOSIVES_ENABLED) BLOCKS_ENABLED = EXPLOSIVES_ENABLED = true;
    if (BLOCK_SPAWN_WEIGHT <= 0.0f && EXPLOSIVE_SPAWN_WEIGHT <= 0.0f) {
        BLOCK_SPAWN_WEIGHT = 1.0f;
        EXPLOSIVE_SPAWN_WEIGHT = 1.0f;
    }

    if (SPAWN_KEY_DEBOUNCE_MS > 10000) SPAWN_KEY_DEBOUNCE_MS = 10000;
    if (BLOCK_SPAWN_KEY_VK == 0 || BLOCK_SPAWN_KEY_VK > 255)
        BLOCK_SPAWN_KEY_VK = VK_OEM_COMMA;
    if (EXPLOSIVE_SPAWN_KEY_VK == 0 || EXPLOSIVE_SPAWN_KEY_VK > 255)
        EXPLOSIVE_SPAWN_KEY_VK = VK_OEM_PERIOD;
    // 两个键撞在一起时以"方块键"为准(钩子里就是这么判的), 这里不强行改键,
    // 免得用户明明写了两个键却被程序偷偷换掉 —— 生成文件里的注释提醒过别写一样。
}

static void LoadConfig()
{
    const std::wstring path = ExeDir() + L"\\config.ini";

    std::string raw;
    if (!ReadWholeFile(path, raw)) {
        // 第一次运行(或文件被删了): 生成一份带说明的默认配置, 下次启动就能直接改
        WriteDefaultConfig(path);
        SanitizeConfig();
        return;
    }

    const std::wstring text = DecodeText(raw);
    std::vector<std::wstring> badKeys, badValues;

    size_t i = 0;
    while (i <= text.size()) {
        const size_t nl = text.find(L'\n', i);
        std::wstring line = text.substr(i, (nl == std::wstring::npos ? text.size() : nl) - i);
        i = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        // 去注释
        for (size_t k = 0; k < line.size(); ++k) {
            if (line[k] == L'#' || line[k] == L';') {
                if (k == 0 || iswspace(line[k - 1])) { line.erase(k); break; }
            }
        }

        const std::wstring t = TrimW(line);
        if (t.empty()) continue;

        // 顺手容忍 [小节标题] 这种习惯写法, 直接跳过
        if (t.front() == L'[' && t.back() == L']') continue;

        const size_t eq = t.find(L'=');
        if (eq == std::wstring::npos) { badKeys.push_back(t); continue; }

        const std::wstring key = NormKey(t.substr(0, eq));
        const std::wstring val = TrimW(t.substr(eq + 1));
        if (key.empty()) continue;

        const CfgEntry* hit = nullptr;
        for (const auto& en : kCfgTable)
            if (key == NormKey(en.key)) { hit = &en; break; }

        if (!hit) { badKeys.push_back(TrimW(t.substr(0, eq))); continue; }
        if (!ApplyCfgValue(*hit, val))
            badValues.push_back(std::wstring(hit->key) + L" = " + val);
    }

    SanitizeConfig();

    if (!badKeys.empty() || !badValues.empty()) {
        std::wstring msg = L"config.ini 里有读不懂的内容，这些设置已被忽略：\n\n";
        size_t shown = 0;
        for (const auto& k : badKeys) {
            if (shown++ >= 8) { msg += L"  …\n"; break; }
            msg += L"  · 不认识的键：" + k + L"\n";
        }
        for (const auto& v : badValues) {
            if (shown++ >= 8) { msg += L"  …\n"; break; }
            msg += L"  · 值看不懂：" + v + L"\n";
        }
        msg += L"\n其余设置照常生效，程序会继续运行。\n\n文件位置：\n" + path;
        MessageBoxW(nullptr, msg.c_str(), L"Falling Images - 配置", MB_OK | MB_ICONWARNING);
    }
}

// ------------------------------------------------------------
// 小工具
// ------------------------------------------------------------
static float RandF(float a, float b)
{
    return std::uniform_real_distribution<float>(a, b)(g_rng);
}

// 平台粗判用的大尺寸: 方块和爆炸物哪个大用哪个。
// (真正的"能不能接住"是 TopEdgeFits 按精灵自己的高度算的, 这里只是个粗筛)
static int MaxSpriteSize()
{
    return std::max(BLOCK_SIZE, EXPLOSIVE_SIZE);
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
    // 这个窗口把整个工作区都盖住了(无边框全屏 / 最大化)。
    // 这种窗口一律算"遮挡", 不看它的扩展样式 —— 见 BlocksAsOccluder()。
    bool coversWorkArea = false;
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
//
// ★ 但"铺满整个工作区"的窗口必须无条件算遮挡, 不能看样式。
//   很多"窗口化全屏"的应用(UWP / WinUI3 / 用 DirectComposition 合成的游戏、
//   播放器)本身就带 WS_EX_NOREDIRECTIONBITMAP / WS_EX_LAYERED。它们确确实实
//   盖住了后面的一切, 却被上面两条挡在遮挡之外 —— 于是被它盖住的窗口保留了
//   **看不见的碰撞箱**: 图片停在谁也看不见的顶边上, 悬在半空。
//   复现: 全屏 c 盖住 b, 再把另一个窗口 a 提到 c 之上, 这时 b 依然在接图片。
//   上面那两条要排除的是**面板大小的壳层浮层**(贴靠预览 / 输入法候选窗 /
//   通知), 它们都盖不满工作区, 所以这条不会把它们放回来。
static bool BlocksAsOccluder(const WindowInfo& wi)
{
    if (wi.hwnd == g_foregroundHwnd) return true;     // 前台浮层(开始菜单…)照旧遮挡
    if (wi.coversWorkArea)           return true;     // 铺满工作区: 合成方式无关

    const LONG_PTR ex = GetWindowLongPtrW(wi.hwnd, GWL_EXSTYLE);
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
        if (!BlocksAsOccluder(wi)) continue;
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
    // 这里只用"顶边上面放得下整张图片"(r.top >= 尺寸)做一个粗判,
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
    const int  coarse    = MaxSpriteSize();
    const bool bigEnough = (r.right - r.left) >= coarse &&
                           (r.bottom - r.top)  >= coarse;
    wi.platform  = (!fullscreen && !maximized && bigEnough && r.top >= coarse);
    // 把整个工作区盖住 = 无边框全屏 / 最大化。这种窗口无条件算遮挡(见 BlocksAsOccluder)。
    wi.coversWorkArea = (r.left   <= g_workX &&
                         r.top    <= g_workY &&
                         r.right  >= g_workX + g_screenW &&
                         r.bottom >= g_workY + g_screenH);
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
    // 方块: 没有引信, 不会自己爆炸, 也不会闪;
    // 只有被爆炸波及才会炸掉(见 UpdatePhysics 里的连锁判定)。
    bool isBlock   = false;
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

// 碰撞闪烁的时长 / 最浅 alpha —— 方块和爆炸物各一套配置。
// 触发和还原都必须走这两个函数: 两类的时长不一样, 用错常数就会闪到一半卡住。
static float HitFlashSec(const Sprite* s)
{
    return s->isBlock ? BLOCK_HIT_FLASH_SEC : EXPLOSIVE_HIT_FLASH_SEC;
}
static float HitDim(const Sprite* s)
{
    return s->isBlock ? BLOCK_HIT_DIM : EXPLOSIVE_HIT_DIM;
}

// ------------------------------------------------------------
// 图片资源（支持多帧 GIF 动画）
// ------------------------------------------------------------
struct ImageAsset
{
    Bitmap* bmp = nullptr;    // 缩放后的 size×size 位图, 所有精灵都引用它
    Bitmap* raw = nullptr;    // 原始解码结果(多帧 GIF 靠它换帧)
    UINT    frameCount = 1;
    UINT    frame      = 0;
    std::vector<UINT> delayMs;   // 每帧延时(毫秒)
    float   timer = 0.0f;        // 当前帧已显示的时间
    float   life  = 3.0f;        // 爆炸物: 引信秒数; 方块: 用 BLOCK_STAY_SEC, 这里不看
    int     size  = 48;          // 这张图的显示边长(方块/爆炸物各自的配置)
    bool    isBlock = false;     // true = 来自 images\blocks
    int     dx = 0, dy = 0, dw = 0, dh = 0;   // 居中缩放后的位置(相对 size×size)
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

// 把 raw 的当前帧按居中缩放布局重绘进 a.size × a.size 的 bmp
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
    if (w <= 0 || h <= 0) { a.dx = a.dy = 0; a.dw = a.dh = a.size; return; }

    const float scale = std::min((float)a.size / w, (float)a.size / h);
    a.dw = std::max(1, (int)std::lround(w * scale));
    a.dh = std::max(1, (int)std::lround(h * scale));
    a.dx = (a.size - a.dw) / 2;
    a.dy = (a.size - a.dh) / 2;
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
// 按类别挑一张图片。
//   SPAWN_BLOCK / SPAWN_EXPLOSIVE: 只从那一类里等概率挑;
//     如果那一类一张图都没有(比如 BLOCKS_ENABLED = false), 退回另一类 ——
//     按键按下去一点反应都没有比"出了另一类"更让人困惑。
//   随机(自动生成)时按权重: 每张方块图计 BLOCK_SPAWN_WEIGHT,
//     每张爆炸物图计 EXPLOSIVE_SPAWN_WEIGHT, 于是某个文件夹里图多那一类就多。
static const ImageAsset* PickAsset(SpawnKind kind)
{
    if (g_assets.empty()) return nullptr;

    auto collect = [](bool block, std::vector<const ImageAsset*>& out) {
        for (const auto& up : g_assets)
            if (up->bmp && up->isBlock == block) out.push_back(up.get());
    };

    bool wantBlock = false;
    if (kind == SPAWN_BLOCK) {
        wantBlock = true;
    } else if (kind == SPAWN_EXPLOSIVE) {
        wantBlock = false;
    } else {
        double wBlock = 0.0, wBoom = 0.0;
        for (const auto& up : g_assets) {
            if (!up->bmp) continue;
            if (up->isBlock) wBlock += (double)BLOCK_SPAWN_WEIGHT;
            else             wBoom  += (double)EXPLOSIVE_SPAWN_WEIGHT;
        }
        const double total = wBlock + wBoom;
        if (total <= 0.0) return nullptr;
        const double r = std::uniform_real_distribution<double>(0.0, total)(g_rng);
        wantBlock = (r < wBlock);
    }

    std::vector<const ImageAsset*> cand;
    collect(wantBlock, cand);
    if (cand.empty()) collect(!wantBlock, cand);   // 该类没图 -> 退回另一类
    if (cand.empty()) return nullptr;
    return cand[(size_t)RandI(0, (int)cand.size() - 1)];
}

// usePos=true 时以 (px,py) 为中心生成(鼠标位置), 并夹进工作区,
// 免得生成到屏幕外面直接看不见; 否则照旧在屏幕顶端随机横坐标生成。
static void SpawnFalling(bool usePos = false, float px = 0.0f, float py = 0.0f,
                         SpawnKind kind = SPAWN_RANDOM)
{
    if (g_assets.empty()) return;
    if ((int)g_sprites.size() >= MAX_TOTAL) return;

    int falling = 0;
    for (auto& sp : g_sprites)
        if (sp->isFalling && !sp->dead) ++falling;
    if (falling >= MAX_FALLING) return;

    const ImageAsset* pa = PickAsset(kind);
    if (!pa) return;
    const ImageAsset& a = *pa;

    const int sz = a.size;
    auto s = MakeSprite(a.bmp, 0, 0, sz, sz);
    if (!s) return;

    s->isFalling = true;
    s->isDebris  = false;
    s->isBlock   = a.isBlock;
    if (usePos) {
        s->x = px - sz * 0.5f;
        s->y = py - sz * 0.5f;
        const float maxX = (float)std::max(0, g_screenW - sz);
        const float maxY = (float)std::max(0, g_screenH - sz);
        if (s->x < 0.0f)      s->x = 0.0f;    else if (s->x > maxX) s->x = maxX;
        if (s->y < 0.0f)      s->y = 0.0f;    else if (s->y > maxY) s->y = maxY;
    } else {
        s->x = RandF(0.0f, (float)std::max(1, g_screenW - sz));
        s->y = -(float)sz;
    }

    if (a.isBlock) {
        s->baseFallSpeed = RandF(BLOCK_FALL_SPEED_MIN, BLOCK_FALL_SPEED_MAX);
        // 方块没有引信: life 是"停稳后还能待多久"的倒计时。
        // BLOCK_STAY_SEC = 0 时给它一个正数, 让它永远不会走到 0。
        s->maxLife = s->life = (BLOCK_STAY_SEC > 0.0f) ? BLOCK_STAY_SEC : 1.0f;
        s->fadeStartLife     = (BLOCK_STAY_SEC > 0.0f) ? BLOCK_FADE_SEC : 0.0f;
    } else {
        s->baseFallSpeed = RandF(EXPLOSIVE_FALL_SPEED_MIN, EXPLOSIVE_FALL_SPEED_MAX);
        s->maxLife = s->life = a.life;
        s->fadeStartLife     = 0.0f;
    }
    s->vy = s->baseFallSpeed;
    s->vx = 0.0f;
    s->gravity = 0.0f;
    s->alpha = 1.0f;
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
// radius / powerMin / powerMax / gravity 由调用方给(现在只有爆炸物会调用它)。
static void ApplyExplosionShockwave(float cx, float cy,
                                    float radius, float powerMin,
                                    float powerMax, float grav)
{
    if (radius <= 0.0f) return;

    for (auto& sp : g_sprites) {
        Sprite* s = sp.get();
        if (s->dead || !s->src) continue;

        float dx = (s->x + s->w * 0.5f) - cx;
        float dy = (s->y + s->h * 0.5f) - cy;
        float d  = std::sqrt(dx * dx + dy * dy);
        if (d > radius) continue;

        if (d < 1.0f) {
            // 正好压在爆心上, 给一个随机朝上的方向
            dx = RandF(-1.0f, 1.0f);
            dy = -1.0f;
            d  = std::sqrt(dx * dx + dy * dy);
            if (d < 0.001f) { dx = 0.0f; dy = -1.0f; d = 1.0f; }
        }

        // 越靠近爆心越猛
        const float t = 1.0f - d / radius;
        const float power = powerMin + (powerMax - powerMin) * t;

        s->vx = dx / d * power + RandF(-40.0f, 40.0f);
        s->vy = dy / d * power - RandF(40.0f, 140.0f);   // 整体略微上扬
        s->gravity   = grav;
        s->restingOn = nullptr;      // 从平台上掀下来

        // 下落中的图片: gravity > 0 会把它切到抛物线分支;
        // 碎片本来就吃 gravity/vx/vy, 直接生效。
    }
}

// 爆炸物爆炸时的冲击波(用 EXPLOSION_* 那一组参数)
static void ExplosiveShockwave(float cx, float cy)
{
    ApplyExplosionShockwave(cx, cy, EXPLOSION_RADIUS,
                            EXPLOSION_POWER_MIN, EXPLOSION_POWER_MAX,
                            EXPLOSION_GRAVITY);
}

// 某个精灵的中心是否落在以 (cx,cy) 为心、radius 为半径的爆炸范围内
static bool InsideBlast(const Sprite* s, float cx, float cy, float radius)
{
    const float dx = (s->x + s->w * 0.5f) - cx;
    const float dy = (s->y + s->h * 0.5f) - cy;
    return dx * dx + dy * dy <= radius * radius;
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
                    s->hitFlash  = HitFlashSec(s);   // 闪一下, 不再长时间变浅
                    // 被窗口以特别快的速度撞飞 -> 引信缩短(方块没有引信, 不受影响)
                    if (!s->isBlock && fabsf(winVx) >= FAST_HIT_SPEED)
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
                if (!s->isBlock) s->life -= dt;   // 方块没有引信, 由"停稳计时"接管

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
                if (!s->isBlock) s->life -= dt;   // 方块没有引信

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

        // ---- 方块的"停稳计时" ----
        // 方块没有引信, 所以它的 life 不是爆炸倒计时, 而是"停稳后还能待多久"。
        // 只有真的停住了(不受重力、竖直速度也归零了)才开始扣;
        // 一旦被炸飞/被创飞又动起来, 计时直接重置 —— 免得它在空中就淡没了。
        if (s->isBlock) {
            if (BLOCK_STAY_SEC <= 0.0f) {
                s->life = 1.0f;                       // 配置成"一直留着"
            } else if (s->gravity == 0.0f && s->vy == 0.0f) {
                s->life -= dt;
                if (s->life < 0.0f) s->life = 0.0f;   // 交给后面的死亡检查收走(不爆炸)
            } else {
                s->life = BLOCK_STAY_SEC;             // 还在飞: 不计时
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
        //     三角形包络: 前半段压到 HitDim(), 后半段回到全不透明,
        //     不再按剩余寿命慢慢发灰。
        //     时长/最浅 alpha 都按精灵自己的类别取(方块和爆炸物各一套配置)。
        if (s->hitFlash > 0.0f) {
            const float flashSec = HitFlashSec(s);
            const float dimMin   = HitDim(s);
            if (flashSec <= 0.0f) {
                s->hitFlash = 0.0f;                 // 配置成"不闪"
            } else {
                float t = 1.0f - s->hitFlash / flashSec;   // 0 -> 1
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                const float k = (t < 0.5f) ? (t * 2.0f) : ((1.0f - t) * 2.0f);  // 0->1->0
                const float dim = 1.0f - (1.0f - dimMin) * k;
                if (dim < a) a = dim;

                s->hitFlash -= dt;
                if (s->hitFlash < 0.0f) s->hitFlash = 0.0f;
            }
        }

        // (3) 临爆闪烁: 爆炸前 EXPLODE_WARN_SEC 秒开始, 每 EXPLODE_BLINK_PERIOD 秒一闪。
        //     只对"会爆炸"的精灵生效: 碎屑只是消失, 方块根本不炸, 都不闪。
        if (s->isFalling && !s->isDebris && !s->isBlock &&
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

            a->hitFlash = HitFlashSec(a);
            b->hitFlash = HitFlashSec(b);

            // 特别快的互撞 -> 双方引信都缩短(方块没有引信, 不参与)
            if (impact >= FAST_HIT_SPEED) {
                if (!a->isBlock) a->life -= FAST_HIT_LIFE_LOSS;
                if (!b->isBlock) b->life -= FAST_HIT_LIFE_LOSS;
            }
        }
    }

    // ---- 检查死亡 / 爆炸 ----
    // 三类走不同的路:
    //   · 爆炸物(引信到点)  -> 炸: 放冲击波 + 炸成碎片
    //   · 方块(停稳超时)    -> 只是消失, 不炸(它的 life 是"停留倒计时")
    //   · 碎片              -> 只是消失
    std::vector<Sprite*> toExplode;
    for (auto& sp : g_sprites) {
        if (sp->dead) continue;
        if (sp->life > 0.0f) continue;

        if (sp->isBlock || sp->isDebris || !sp->isFalling) {
            sp->dead = true;               // 方块自然消失: 不放冲击波, 也不出碎片
            continue;
        }
        sp->dead = true;
        toExplode.push_back(sp.get());
    }

    // ---- 爆炸范围内的方块会被炸掉 ----
    // 只有"爆炸物爆炸"会触发, 而且**不传播**: 被炸掉的方块只是碎掉,
    // 不会再放冲击波去炸旁边的方块。(早先那版会连锁, 一颗就能清屏。)
    // 爆心先收集好, 再统一扫方块 —— 这样本帧刚被炸掉的方块不会被当成新的爆心。
    if (BLOCK_DESTROY_IN_BLAST && EXPLOSION_RADIUS > 0.0f) {
        std::vector<const Sprite*> blasts;
        for (Sprite* s : toExplode)
            if (!s->isBlock) blasts.push_back(s);

        for (const Sprite* src : blasts) {
            const float cx = src->x + src->w * 0.5f;
            const float cy = src->y + src->h * 0.5f;
            for (auto& sp : g_sprites) {
                if (sp->dead || !sp->isBlock) continue;
                if (!InsideBlast(sp.get(), cx, cy, EXPLOSION_RADIUS)) continue;
                sp->dead = true;
                toExplode.push_back(sp.get());
            }
        }
    }

    // ---- 冲击波: 只有爆炸物会放 ----
    // 方块被炸掉时没有冲击波, 只有它自己那一堆碎片。
    // ---- 冲击波: 只有爆炸物会放 ----
    // 方块被炸掉时没有冲击波, 只有它自己那一堆碎片。
    for (Sprite* s : toExplode) {
        if (s->isBlock) continue;
        ExplosiveShockwave(s->x + s->w * 0.5f, s->y + s->h * 0.5f);
    }

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
// 三个来源:
//   images\blocks      方块    (BLOCK_MAX_IMAGES, BLOCK_SIZE, 不爆炸)
//   images\explosives  爆炸物  (EXPLOSIVE_MAX_IMAGES, EXPLOSIVE_SIZE, 有引信)
//   images 根目录      按爆炸物处理 —— 老版本的图片都放在这里, 不动它们
// 目录不存在就跳过, 不报错。文件名排序后取前 N 张。
static bool SupportedImageExt(const std::wstring& name)
{
    std::wstring lower = name;
    for (auto& ch : lower) ch = (wchar_t)towlower(ch);
    const size_t dot = lower.find_last_of(L'.');
    const std::wstring ext = (dot == std::wstring::npos) ? L"" : lower.substr(dot);
    return ext == L".png"  || ext == L".jpg"  || ext == L".jpeg" ||
           ext == L".bmp"  || ext == L".gif"  || ext == L".webp" ||
           ext == L".tif"  || ext == L".tiff" || ext == L".ico"  ||
           ext == L".jfif" || ext == L".jxl";
}

// 只扫一层(不递归), 免得 images\blocks\素材\... 这种子目录被吸进来
static void ListImageFiles(const std::wstring& dir, std::vector<std::wstring>& out)
{
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((dir + L"\\*.*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!SupportedImageExt(fd.cFileName)) continue;
        out.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

static void LoadImageFolder(const std::wstring& dir, bool isBlock, int maxCount, int size)
{
    if (maxCount <= 0) return;

    std::vector<std::wstring> files;
    ListImageFiles(dir, files);
    std::sort(files.begin(), files.end());
    if ((int)files.size() > maxCount) files.resize(maxCount);

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
        asset->life  = isBlock ? BLOCK_STAY_SEC : EXPLOSIVE_LIFE_SEC;
        asset->size  = size;
        asset->isBlock = isBlock;

        ReadFrameDelays(*asset);
        ComputeAssetLayout(*asset);

        asset->bmp = new Bitmap(size, size, PixelFormat32bppPARGB);
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

static void LoadImages()
{
    const std::wstring imgDir = ExeDir() + L"\\images";

    // 爆炸物: 先读 images\explosives, 再读 images 根目录(老版本图片留在那里)
    if (EXPLOSIVES_ENABLED) {
        LoadImageFolder(imgDir + L"\\explosives", false,
                        EXPLOSIVE_MAX_IMAGES, EXPLOSIVE_SIZE);
        const int left = EXPLOSIVE_MAX_IMAGES - (int)g_assets.size();
        if (left > 0)
            LoadImageFolder(imgDir, false, left, EXPLOSIVE_SIZE);
    }

    // 方块
    if (BLOCKS_ENABLED)
        LoadImageFolder(imgDir + L"\\blocks", true, BLOCK_MAX_IMAGES, BLOCK_SIZE);
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
// 低级键盘钩子: 两个键分别在鼠标位置生成方块 / 爆炸物
// ------------------------------------------------------------
// overlay 窗口带 WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, 永远拿不到焦点,
// 所以它收不到 WM_KEYDOWN; 而 RegisterHotKey 会把这个键从所有程序手里抢走。
// WH_KEYBOARD_LL 钩子只是"旁听": 我们照旧 CallNextHookEx 把按键原样放行,
// 别的程序完全不受影响。
// 低级钩子由安装它的线程(主线程)在自己的消息循环里回调, 所以下面这几个
// 变量不存在跨线程竞争 —— 钩子里只记一下位置, 真正的生成放在主循环里做。
static HHOOK g_kbHook       = nullptr;
// 每个键各记一个时间戳: 共用一个的话, 先按逗号再按句号会被去抖挡掉一次。
static DWORD g_lastSpawnTick[SPAWN_KIND_COUNT] = {};
static bool  g_spawnAtMouse = false;   // 有待处理的生成请求
static POINT g_spawnPoint   = {};
static SpawnKind g_spawnKind = SPAWN_RANDOM;   // 这次请求要生成哪一类

// 两次触发的最小间隔(毫秒)。长按的自动重复间隔只有 ~30ms, 会被这条挡掉;
// 而且它是"比时间戳"而不是"记按键状态", 所以就算漏掉一次 KEYUP 也不会卡死。
// (数值在 config.ini 的 SPAWN_KEY_DEBOUNCE_MS, 两个键各自计时)

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && lParam) {
        const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            // 两个键撞在一起时以"方块键"为准 —— 生成文件里的注释提醒过别写一样
            SpawnKind kind = SPAWN_KIND_COUNT;
            if (kb->vkCode == BLOCK_SPAWN_KEY_VK)          kind = SPAWN_BLOCK;
            else if (kb->vkCode == EXPLOSIVE_SPAWN_KEY_VK) kind = SPAWN_EXPLOSIVE;

            if (kind != SPAWN_KIND_COUNT) {
                const DWORD now = GetTickCount();
                if (now - g_lastSpawnTick[kind] >= SPAWN_KEY_DEBOUNCE_MS) {
                    g_lastSpawnTick[kind] = now;
                    POINT pt;
                    if (GetCursorPos(&pt)) {
                        g_spawnPoint   = pt;
                        g_spawnKind    = kind;
                        g_spawnAtMouse = true;
                    }
                }
            }
        }
    }
    // 原样放行: 别的程序该怎么收到这些键还是怎么收到
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

    // ---- 先读 config.ini: 后面所有地方用的都是配置里的值 ----
    // 放在最前面, 这样窗口尺寸、扫描间隔、寿命……全都统一来自同一份配置。
    LoadConfig();

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
            L"请在 exe 同目录下建 images 文件夹，里面放两个子文件夹：\n"
            L"    images\\blocks      方块   （不会自己爆炸）\n"
            L"    images\\explosives  爆炸物 （有引信，到点爆炸）\n\n"
            L"各放 1~" + std::to_wstring(BLOCK_MAX_IMAGES) + L" / " +
            std::to_wstring(EXPLOSIVE_MAX_IMAGES) +
            L" 张 png / jpg / bmp / gif / webp 图片，\n"
            L"放在 images 根目录里的图片按爆炸物处理。\n\n"
            L"爆炸物存活 " + std::to_wstring((int)EXPLOSIVE_LIFE_SEC) +
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

    g_spawnTimer = SPAWN_FIRST_DELAY;

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

        // ---- 按配置的两个键在鼠标位置生成方块 / 爆炸物 ----
        // 钩子回调里只记了位置和类别, 真正干活放在这里。
        // 鼠标坐标是屏幕坐标, 减去 overlay 原点换成合成缓冲坐标。
        if (MANUAL_SPAWN_ENABLED && g_spawnAtMouse) {
            g_spawnAtMouse = false;
            SpawnFalling(true,
                         (float)(g_spawnPoint.x - g_workX),
                         (float)(g_spawnPoint.y - g_workY),
                         g_spawnKind);
        }

        // ---- 自动随机下落生成 ----
        if (AUTO_SPAWN_ENABLED) {
            g_spawnTimer -= dt;
            if (g_spawnTimer <= 0.0f) {
                SpawnFalling();
                g_spawnTimer = RandF(DROPTIME_MIN, DROPTIME_MAX);
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
