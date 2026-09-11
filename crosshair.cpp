/*
 * crosshair.c — 悬浮准心（仅中心圆点 / 比例存档）
 * [仅绘图版]
 *
 * 以显示器中心点为原点建立物理坐标系。
 * 坐标记录为 (ratioX, ratioY)：当前像素偏移 ÷ 半屏尺寸。
 * 支持鼠标拖动、方向键微调，坐标自动存档到 crosshair_config.txt。
 *
 * 编译 (MinGW):  gcc crosshair_fixed.c -o crosshair.exe -lgdi32 -lshell32 -mwindows
 * 编译 (MSVC):   cl crosshair_fixed.c /link user32.lib gdi32.lib shell32.lib
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <math.h>
#include <string.h>   /* for strrchr() */
#include <stdlib.h>   /* for _wtof() */
#include <ocidl.h>
#include <commdlg.h>
#include <commctrl.h>
#include <gdiplus.h>
using namespace Gdiplus;

/* Per-Monitor DPI Awareness V2 (Win10 1703+) */
#ifndef WM_DPICHANGED
#define WM_DPICHANGED  0x02E0
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2  ((DPI_AWARENESS_CONTEXT)-4)
#endif
#ifndef AC_SRC_OVER
#define AC_SRC_OVER  0x00
#endif
#ifndef ULW_ALPHA
#define ULW_ALPHA    0x00000002
#endif

/* ===== 可调参数 ===== */
#define STEP_SMALL      1       /* 方向键步长 */
#define STEP_BIG        10      /* Shift+方向键步长 */

/* colorkey 颜色 — 必须与背景填充色一致，此颜色区域透明穿透 */
#define COLORKEY        RGB(0, 0, 0)

/* 圆点大小以屏幕比例计量：横线=屏宽比例，竖线=屏高比例 */
#define SIZE_STEP       0.002   /* +/- 缩放步长 */
#define SIZE_MIN        0.005   /* 最小比例 */
#define CROSS_SIZE_MAX  0.20    /* 最大比例 */
#define ALPHA_STEP      15      /* 透明度步长 */
#define ALPHA_MIN       15      /* 最小透明度（低于此值几乎看不见） */
#define ALPHA_MAX       255     /* 最大透明度（完全不透明） */
#define DEFAULT_RATIO_X -0.007700  /* 位置默认 X */
#define DEFAULT_RATIO_Y  0.668000  /* 位置默认 Y */
#define DEFAULT_LINE_X   0.039000  /* 线宽默认 X */
#define DEFAULT_LINE_Y   0.039000  /* 线宽默认 Y */
#define SECOND_DOT_RATIO_X    -0.005200  /* 第二点 X 比例坐标 */
#define SECOND_DOT_RATIO_Y     0.362500  /* 第二点 Y 比例坐标 */
#define DEFAULT_SECOND_LINE 0.034000     /* 第二点线长比例，独立于第一点 */
#define SECOND_DOT_COLOR       0x00FFFF80 /* 第二点颜色 */
#define SECOND_DOT_ALPHA       100       /* 第二点透明度 */

/* ===== 预设数据 ===== */
struct CrosshairPreset {
    double   ratioX, ratioY;          /* 主准心位置比例 */
    double   lineRatioX, lineRatioY;  /* 主准心线长比例 */
    COLORREF color;                   /* 主准心颜色 */
    int      alpha;                   /* 主准心透明度 */
    double   secondRatioX, secondRatioY;  /* 第二点位置比例 */
    double   secondLineRatio;             /* 第二点线长比例 */
    COLORREF secondColor;                 /* 第二点颜色 */
    int      secondAlpha;                 /* 第二点透明度 */
    BOOL     showSecondDot;               /* 是否显示第二点 */
};

static const CrosshairPreset g_presets[] = {
    /* 预设1 FOV90 */
    { -0.009780, 0.797600, 0.039000, 0.039000, 0x0000FF, 100,
      -0.006000, 0.483333, 0.034000, 0xFFFF80, 100, TRUE },
    /* 预设2 FOV100 */
    { -0.007700, 0.668000, 0.039000, 0.039000, 0x0000FF, 100,
      -0.005200, 0.362500, 0.034000, 0xFFFF80, 100, TRUE },
    /* 预设3 FOV110 */
    { -0.007700, 0.555000, 0.039000, 0.039000, 0x0000FF, 100,
      -0.005980, 0.365300, 0.034000, 0xFFFF80, 100, TRUE },
    /* 预设4 FOV120 */
    { -0.006660, 0.456900, 0.039000, 0.039000, 0x0000FF, 100,
      -0.005199, 0.365300, 0.034000, 0xFFFF80, 100, TRUE },
};
static const int g_presetCount = (int)(sizeof(g_presets) / sizeof(g_presets[0]));
static int g_currentPreset = 1;   /* 当前预设索引，1-based，默认第 1 套 */

/* ===== 全局状态 ===== */
static ULONG_PTR g_gdiplusToken;
static double g_ratioX = DEFAULT_RATIO_X;   /* x 比例坐标 */
static double g_ratioY = DEFAULT_RATIO_Y;   /* y 比例坐标 */
static int    g_cxScreen = 0;
static int    g_cyScreen = 0;
static int    g_halfW = 0;
static int    g_halfH = 0;
static COLORREF g_crossColor = 0x0000FF; /* 准心颜色 */
static int     g_alpha      = 100;           /* 准心透明度 0-255 */
static double g_secondRatioX = SECOND_DOT_RATIO_X;
static double g_secondRatioY = SECOND_DOT_RATIO_Y;
static double g_secondLineRatio = DEFAULT_SECOND_LINE;
static COLORREF g_secondColor = SECOND_DOT_COLOR;
static int    g_secondAlpha = SECOND_DOT_ALPHA;
static BOOL   g_editingSecond = FALSE;     /* 是否在编辑第二点 */
static BOOL   g_showSecondDot = TRUE;      /* 是否显示第二点 */
static double g_lineRatioX = DEFAULT_LINE_X;  /* 横线 = 屏宽 × 此比例 */
static double g_lineRatioY = DEFAULT_LINE_Y;  /* 竖线 = 屏高 × 此比例 */
static BOOL   g_uiInitializing = FALSE;       /* 设置窗口初始化中，忽略 EN_CHANGE */

/* 将线长比例转为当前屏幕像素 */
static int LineLenXToPixels(void)
{
    return (int)(g_halfW * 2.0 * g_lineRatioX + 0.5);
}
static int LineLenYToPixels(void)
{
    return (int)(g_halfH * 2.0 * g_lineRatioY + 0.5);
}
/* 计算窗口边长（基于两点范围） */
static int CalcWindowSize(void)
{
    int lineLenX = LineLenXToPixels();
    int lineLenY = LineLenYToPixels();
    int maxLen = (lineLenX > lineLenY) ? lineLenX : lineLenY;
    int dotR = maxLen / 10;
    if (dotR < 2) dotR = 2;
    int sMaxLen = 0, dx = 0, dy = 0, sDotR = 0;
    if (g_showSecondDot) {
        int sLineLenX = (int)(g_halfW * 2.0 * g_secondLineRatio + 0.5);
        int sLineLenY = (int)(g_halfH * 2.0 * g_secondLineRatio + 0.5);
        sMaxLen = (sLineLenX > sLineLenY) ? sLineLenX : sLineLenY;
        sDotR = sMaxLen / 10;
        if (sDotR < 1) sDotR = 1;
        dx = (int)((g_secondRatioX - g_ratioX) * g_halfW + 0.5);
        dy = (int)((g_secondRatioY - g_ratioY) * g_halfH + 0.5);
    }
    int half = dotR + 2;
    int margin = 2;
    if (dx + sDotR + margin > half) half = dx + sDotR + margin;
    if (-dx + sDotR + margin > half) half = -dx + sDotR + margin;
    if (dy + sDotR + margin > half) half = dy + sDotR + margin;
    if (-dy + sDotR + margin > half) half = -dy + sDotR + margin;
    return half * 2;
}

static HWND   g_hwnd = NULL;
static HWND   g_hSettings = NULL;
static BOOL   g_suppressActivate = FALSE;
static BOOL   g_dragging = FALSE;
static POINT  g_dragBase;       /* 拖动基准点（屏幕坐标） */
static double g_dragRatioX;     /* 拖动起始比例 X */
static double g_dragRatioY;     /* 拖动起始比例 Y */

static WCHAR  g_configPath[MAX_PATH];   /* 配置文件路径 */
static BOOL   g_transparentMode = TRUE; /* TRUE=完全穿透不响应鼠标 */
static UINT   g_trayIconId = 1;         /* 托盘图标 ID */
static NOTIFYICONDATAA g_nid;            /* 托盘图标数据 */

/* DIB 缓冲区（UpdateLayeredWindow per-pixel alpha） */
static HDC     g_dibDC   = NULL;
static HBITMAP g_dibBmp  = NULL;
static void   *g_dibBits = NULL;
static int     g_dibW    = 0;
static int     g_dibH    = 0;

/* ===== 前向声明 ===== */
static void   RecalcScreenMetrics(void);
static void   RatioToPixel(double rx, double ry, int *px, int *py);
static void   PixelToRatio(int px, int py, double *rx, double *ry);
static void   RenderAndShow(HWND hwnd, double rx, double ry);
static void   DrawCrosshair(HDC hdc, int w, int h);
static void   SaveConfig(void);
static void   LoadConfig(void);
static void   UpdateTitle(HWND hwnd);
static void   CreateTrayIcon(HWND hwnd);
static void   RemoveTrayIcon(void);
static void   ShowTrayMenu(HWND hwnd, POINT pt);
static void   ApplyLineRatio(double newRatioX, double newRatioY);
static void   ApplyPresetCore(int n);
static void   ApplyPreset(int n);
static void   ShowSettingsWindow(HWND hwnd);
static void   ToggleSettingsWindow(HWND hwnd);
static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* ---------- 屏幕参数刷新 ---------- */
static void RecalcScreenMetrics(void)
{
    g_cxScreen = GetSystemMetrics(SM_CXSCREEN) / 2;
    g_cyScreen = GetSystemMetrics(SM_CYSCREEN) / 2;
    g_halfW    = g_cxScreen;
    g_halfH    = g_cyScreen;
}

/* ---------- 比例 → 像素 ---------- */
static void RatioToPixel(double rx, double ry, int *px, int *py)
{
    *px = g_cxScreen + (int)(rx * g_halfW + 0.5);
    *py = g_cyScreen + (int)(ry * g_halfH + 0.5);
}

/* ---------- 像素 → 比例 ---------- */
static void PixelToRatio(int px, int py, double *rx, double *ry)
{
    *rx = (double)(px - g_cxScreen) / (double)g_halfW;
    *ry = (double)(py - g_cyScreen) / (double)g_halfH;
}

/* ---------- DIB 缓冲区管理 ---------- */
static void EnsureDIB(int w, int h)
{
    if (w == g_dibW && h == g_dibH)
        return;

    if (g_dibBmp) {
        DeleteObject(g_dibBmp);
        DeleteDC(g_dibDC);
    }

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;   /* top-down DIB */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdcScreen = GetDC(NULL);
    g_dibDC  = CreateCompatibleDC(hdcScreen);
    g_dibBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS,
                                &g_dibBits, NULL, 0);
    SelectObject(g_dibDC, g_dibBmp);
    ReleaseDC(NULL, hdcScreen);

    g_dibW = w;
    g_dibH = h;
}

/* ---------- 渲染并定位到比例坐标（UpdateLayeredWindow） ---------- */
static void RenderAndShow(HWND hwnd, double rx, double ry)
{
    int cx, cy;
    int sz = CalcWindowSize();
    RatioToPixel(rx, ry, &cx, &cy);

    EnsureDIB(sz, sz);
    DrawCrosshair(g_dibDC, sz, sz);

    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    SIZE sizeWnd = {sz, sz};
    POINT ptDst  = {cx - sz / 2, cy - sz / 2};
    POINT ptSrc  = {0, 0};
    UpdateLayeredWindow(hwnd, NULL, &ptDst, &sizeWnd,
                        g_dibDC, &ptSrc, 0, &blend, ULW_ALPHA);

    UpdateTitle(hwnd);
}

/* ---------- 标题栏显示当前比例 ---------- */
static void UpdateTitle(HWND hwnd)
{
    SetWindowTextW(hwnd, L"炸撤电竞");
}

/* ---------- 绘制准心（实心绿圆，抗锯齿 + per-pixel alpha） ---------- */
static void DrawCrosshair(HDC hdc, int w, int h)
{
    int cx = w / 2;
    int cy = w / 2;
    int lineLenX = LineLenXToPixels();
    int lineLenY = LineLenYToPixels();
    int maxLen = (lineLenX > lineLenY) ? lineLenX : lineLenY;
    int dotR = maxLen / 10;
    if (dotR < 2) dotR = 2;

    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    /* 透明背景 */
    g.Clear(Color(0, 0, 0, 0));

    /* 实心圆 */
    COLORREF cr = g_crossColor;
    SolidBrush brush(Color(g_alpha, GetRValue(cr), GetGValue(cr), GetBValue(cr)));
    g.FillEllipse(&brush, cx - dotR, cy - dotR, 2 * dotR, 2 * dotR);

    /* 第二点 */
    if (g_showSecondDot)
    {
        int sCx = cx + (int)((g_secondRatioX - g_ratioX) * g_halfW + 0.5);
        int sCy = cy + (int)((g_secondRatioY - g_ratioY) * g_halfH + 0.5);
        int sLineLenX = (int)(g_halfW * 2.0 * g_secondLineRatio + 0.5);
        int sLineLenY = (int)(g_halfH * 2.0 * g_secondLineRatio + 0.5);
        int sMaxLen = (sLineLenX > sLineLenY) ? sLineLenX : sLineLenY;
        int sDotR = sMaxLen / 10;
        if (sDotR < 1) sDotR = 1;
        COLORREF cr2 = g_secondColor;
        SolidBrush brush2(Color(g_secondAlpha, GetRValue(cr2), GetGValue(cr2), GetBValue(cr2)));
        g.FillEllipse(&brush2, sCx - sDotR, sCy - sDotR, 2 * sDotR, 2 * sDotR);
    }
}


/* ---------- 保存配置 ---------- */
static void SaveConfig(void)
{
    FILE *fp = _wfopen(g_configPath, L"w");
    if (fp) {
        fprintf(fp, "%.6f %.6f %.6f %.6f 0x%06X %d\n",
                g_ratioX, g_ratioY, g_lineRatioX, g_lineRatioY, g_crossColor, g_alpha);
        fprintf(fp, "%.6f %.6f %.6f 0x%06X %d %d\n",
                g_secondRatioX, g_secondRatioY, g_secondLineRatio, g_secondColor, g_secondAlpha, g_showSecondDot ? 1 : 0);
        fprintf(fp, "preset %d\n", g_currentPreset);
        fclose(fp);
    }
}

/* ---------- 加载配置 ---------- */
static void LoadConfig(void)
{
    FILE *fp = _wfopen(g_configPath, L"r");
    if (fp) {
        double rx = DEFAULT_LINE_X, ry = DEFAULT_LINE_Y;
        unsigned int hexColor = 0;
        int alpha = 255;
        if (fscanf(fp, "%lf %lf %lf %lf", &g_ratioX, &g_ratioY, &rx, &ry) >= 2) {
            if (g_ratioX < -2.0) g_ratioX = -2.0;
            if (g_ratioX >  2.0) g_ratioX =  2.0;
            if (g_ratioY < -2.0) g_ratioY = -2.0;
            if (g_ratioY >  2.0) g_ratioY =  2.0;
            /* 兼容旧格式：> CROSS_SIZE_MAX 的值为旧像素值，转为比例 */
            if (rx > CROSS_SIZE_MAX) { double sw = g_halfW * 2.0; if (sw>0) rx = (double)(int)rx / sw; }
            if (ry > CROSS_SIZE_MAX) { double sh = g_halfH * 2.0; if (sh>0) ry = (double)(int)ry / sh; }
            if (rx >= SIZE_MIN && rx <= CROSS_SIZE_MAX) g_lineRatioX = rx;
            if (ry >= SIZE_MIN && ry <= CROSS_SIZE_MAX) g_lineRatioY = ry;
            /* 读取 hex 颜色，格式 0xRRGGBB */
            if (fscanf(fp, "%x", &hexColor) == 1)
                g_crossColor = (COLORREF)hexColor;
            /* 读取透明度（兼容旧格式无此项） */
            if (fscanf(fp, "%d", &alpha) == 1) {
                if (alpha < ALPHA_MIN) alpha = ALPHA_MIN;
                if (alpha > ALPHA_MAX) alpha = ALPHA_MAX;
                g_alpha = alpha;
            }
            /* 读取第二点参数（兼容旧格式无此项） */
            {
                double sOx, sOy, sSf;
                unsigned int sColor;
                int sAlpha, sShow;
                int count = fscanf(fp, "%lf %lf %lf %x %d %d", &sOx, &sOy, &sSf, &sColor, &sAlpha, &sShow);
                if (count >= 5) {
                    g_secondRatioX = sOx;
                    g_secondRatioY = sOy;
                    if (sSf >= SIZE_MIN && sSf <= CROSS_SIZE_MAX) g_secondLineRatio = sSf;
                    g_secondColor = (COLORREF)sColor;
                    if (sAlpha >= ALPHA_MIN && sAlpha <= ALPHA_MAX) g_secondAlpha = sAlpha;
                    if (count >= 6) g_showSecondDot = (sShow != 0);
                }
            }
            /* 读取预设索引行（兼容缺失该行时默认 1） */
            {
                int presetIdx = 1;
                if (fscanf(fp, "preset %d", &presetIdx) == 1) {
                    if (presetIdx < 1) presetIdx = 1;
                    if (presetIdx > g_presetCount) presetIdx = g_presetCount;
                }
                /* 仅恢复内存状态，不在此处 SaveConfig，避免改写准心数据行 */
                ApplyPresetCore(presetIdx);
            }
        }
        fclose(fp);
    }
}

/* ---------- 应用线长比例 ---------- */
static void ApplyLineRatio(double newRatioX, double newRatioY)
{
    if (newRatioX < SIZE_MIN) newRatioX = SIZE_MIN;
    if (newRatioX > CROSS_SIZE_MAX) newRatioX = CROSS_SIZE_MAX;
    if (newRatioY < SIZE_MIN) newRatioY = SIZE_MIN;
    if (newRatioY > CROSS_SIZE_MAX) newRatioY = CROSS_SIZE_MAX;
    g_lineRatioX = newRatioX;
    g_lineRatioY = newRatioY;
}

/* ---------- 应用预设（仅写全局状态，不重绘不存档） ---------- */
static void ApplyPresetCore(int n)
{
    if (n < 1) n = 1;
    if (n > g_presetCount) n = g_presetCount;
    const CrosshairPreset *p = &g_presets[n - 1];
    g_ratioX = p->ratioX;
    g_ratioY = p->ratioY;
    g_lineRatioX = p->lineRatioX;
    g_lineRatioY = p->lineRatioY;
    g_crossColor = p->color;
    g_alpha = p->alpha;
    g_secondRatioX = p->secondRatioX;
    g_secondRatioY = p->secondRatioY;
    g_secondLineRatio = p->secondLineRatio;
    g_secondColor = p->secondColor;
    g_secondAlpha = p->secondAlpha;
    g_showSecondDot = p->showSecondDot;
    g_currentPreset = n;
}

/* ---------- 应用预设（重绘 + 存档） ---------- */
static void ApplyPreset(int n)
{
    ApplyPresetCore(n);
    if (g_hwnd) RenderAndShow(g_hwnd, g_ratioX, g_ratioY);
    SaveConfig();
}

/* ===== 系统托盘 ===== */
#define WM_TRAYICON  (WM_APP + 1)
#define IDM_EXIT     1001
#define IDM_TOGGLE   1002
#define IDM_RESET    1003
#define IDM_COLOR    1004
#define IDM_SIZE     1005
#define IDM_SECOND_DOT 1006
#define IDM_ALPHA_INC 1007
#define IDM_ALPHA_DEC 1008
#define IDM_ALPHA     1009
#define IDM_SECOND_SIZE  1011
#define IDM_SECOND_COLOR 1012
#define IDM_SECOND_ALPHA 1013
#define IDM_SECOND_EDIT  1014
#define IDM_SECOND_RESET 1015
#define IDM_SECOND_TOGGLE 1016
#define IDM_PRESET_BASE   1100   /* 预设 1..4：1100/1101/1102/1103 */

static void CreateTrayIcon(HWND hwnd)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = g_trayIconId;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
    strncpy(g_nid.szTip, "Crosshair", sizeof(g_nid.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon(void)
{
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hwnd, POINT pt)
{
    HMENU hMenu = CreatePopupMenu();
    {
        HMENU hSecond = CreatePopupMenu();
        AppendMenuW(hSecond, MF_STRING, IDM_SECOND_TOGGLE, g_showSecondDot ? L"关闭第二点" : L"开启第二点");
        AppendMenuW(hSecond, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hSecond, MF_STRING, IDM_SECOND_SIZE, L"调节大小...");
        AppendMenuW(hSecond, MF_STRING, IDM_SECOND_COLOR, L"选择颜色...");
        AppendMenuW(hSecond, MF_STRING, IDM_SECOND_ALPHA, L"调节透明度...");
        AppendMenuW(hSecond, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hSecond, MF_STRING, IDM_TOGGLE, g_transparentMode ? L"解除锁定" : L"锁定准心");
        AppendMenuW(hSecond, MF_STRING, IDM_SECOND_RESET, L"回归默认位置");
        AppendMenuW(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hSecond, L"第二点");
    }
    /* 预设子菜单（仅菜单切换，无快捷键） */
    {
        HMENU hPreset = CreatePopupMenu();
        const wchar_t *presetNames[] = { L"FOV90", L"FOV100", L"FOV110", L"FOV120" };
        for (int i = 0; i < g_presetCount; i++) {
            UINT flags = MF_STRING;
            if (g_currentPreset == i + 1) flags |= MF_CHECKED;
            AppendMenuW(hPreset, flags, IDM_PRESET_BASE + i, presetNames[i]);
        }
        AppendMenuW(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hPreset, L"预设");
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_TOGGLE, g_transparentMode ? L"解除锁定" : L"锁定准心");
    AppendMenuW(hMenu, MF_STRING, IDM_RESET,  L"回归默认位置");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_COLOR,  L"选择颜色...");
    AppendMenuW(hMenu, MF_STRING, IDM_SIZE, L"调节大小...");
    AppendMenuW(hMenu, MF_STRING, IDM_ALPHA, L"调节透明度...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT,   L"退出");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

/* ---------- 窗口过程 ---------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE:
        /* 确定配置文件路径（exe 同目录） */
        {
            WCHAR exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            WCHAR *slash = wcsrchr(exePath, L'\\');
            if (slash) *slash = L'\0';
            swprintf(g_configPath, MAX_PATH, L"%ls\\准心配置文件.txt", exePath);
        }
        RecalcScreenMetrics();
        LoadConfig();
        RenderAndShow(hwnd, g_ratioX, g_ratioY);
        CreateTrayIcon(hwnd);
        SetWindowLong(hwnd, GWL_EXSTYLE,
            GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        break;

    case WM_PAINT:
        RenderAndShow(hwnd, g_ratioX, g_ratioY);
        return 0;

    case WM_ERASEBKGND:
        return 1; /* 禁止系统擦除，避免闪烁 */

    case WM_LBUTTONDOWN:
        if (!g_transparentMode) {
            g_dragging = TRUE;
            GetCursorPos(&g_dragBase);
            /* 判断鼠标离哪个点更近 */
            {
                int cx1, cy1, cx2, cy2;
                RatioToPixel(g_ratioX, g_ratioY, &cx1, &cy1);
                RatioToPixel(g_secondRatioX, g_secondRatioY, &cx2, &cy2);
                int d1 = (g_dragBase.x - cx1) * (g_dragBase.x - cx1) + (g_dragBase.y - cy1) * (g_dragBase.y - cy1);
                int d2 = (g_dragBase.x - cx2) * (g_dragBase.x - cx2) + (g_dragBase.y - cy2) * (g_dragBase.y - cy2);
                g_editingSecond = (d2 < d1);
            }
            if (g_editingSecond) {
                g_dragRatioX = g_secondRatioX;
                g_dragRatioY = g_secondRatioY;
            } else {
                g_dragRatioX = g_ratioX;
                g_dragRatioY = g_ratioY;
            }
            SetCapture(hwnd);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            int dx = pt.x - g_dragBase.x;
            int dy = pt.y - g_dragBase.y;
            if (g_editingSecond) {
                g_secondRatioX = g_dragRatioX + (double)dx / (double)g_halfW;
                g_secondRatioY = g_dragRatioY + (double)dy / (double)g_halfH;
            } else {
                g_ratioX = g_dragRatioX + (double)dx / (double)g_halfW;
                g_ratioY = g_dragRatioY + (double)dy / (double)g_halfH;
            }
            RenderAndShow(hwnd, g_ratioX, g_ratioY);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = FALSE;
            ReleaseCapture();
            SaveConfig();
        }
        return 0;

    case WM_KEYDOWN:
        {
            int step = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? STEP_BIG : STEP_SMALL;
            switch (wp) {
            case VK_UP:
                if (g_editingSecond) g_secondRatioY -= (double)step / g_halfH;
                else g_ratioY -= (double)step / g_halfH;
                break;
            case VK_DOWN:
                if (g_editingSecond) g_secondRatioY += (double)step / g_halfH;
                else g_ratioY += (double)step / g_halfH;
                break;
            case VK_LEFT:
                if (g_editingSecond) g_secondRatioX -= (double)step / g_halfW;
                else g_ratioX -= (double)step / g_halfW;
                break;
            case VK_RIGHT:
                if (g_editingSecond) g_secondRatioX += (double)step / g_halfW;
                else g_ratioX += (double)step / g_halfW;
                break;
            case 'R':
                g_ratioX = 0.0;
                g_ratioY = 0.0;
                break;
            case 'S':
                SaveConfig();
                break;
            case 'L':
                LoadConfig();
                break;
            case 'T':
                g_transparentMode = !g_transparentMode;
                if (g_transparentMode) {
                    SetWindowLong(hwnd, GWL_EXSTYLE,
                        GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
                } else {
                    SetWindowLong(hwnd, GWL_EXSTYLE,
                        GetWindowLong(hwnd, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
                }
                /* 刷新分层属性以应用 exstyle 变更 */
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                break;
            case VK_ESCAPE:
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            case VK_OEM_PLUS:
            case VK_ADD:
                ApplyLineRatio(g_lineRatioX + SIZE_STEP, g_lineRatioY + SIZE_STEP);
                SaveConfig();
                break;
            case VK_OEM_MINUS:
            case VK_SUBTRACT:
                ApplyLineRatio(g_lineRatioX - SIZE_STEP, g_lineRatioY - SIZE_STEP);
                SaveConfig();
                break;
            default:
                return 0;
            }
            RenderAndShow(hwnd, g_ratioX, g_ratioY);
        }
        return 0;

    case WM_SIZE:
        if (wp == SIZE_RESTORED)
            RenderAndShow(hwnd, g_ratioX, g_ratioY);
        return 0;

    case WM_ACTIVATE:
        if (g_suppressActivate) {
            g_suppressActivate = FALSE;
            return 0;
        }
        return 0;

    case WM_CLOSE:
        /* 关闭程序 */
        SaveConfig();
        PostQuitMessage(0);
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            ShowTrayMenu(hwnd, pt);
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowSettingsWindow(hwnd);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TOGGLE:
            g_transparentMode = !g_transparentMode;
            if (g_transparentMode) {
                SetWindowLong(hwnd, GWL_EXSTYLE,
                    GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
            } else {
                SetWindowLong(hwnd, GWL_EXSTYLE,
                    GetWindowLong(hwnd, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
            }
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            UpdateTitle(hwnd);
            return 0;
        case IDM_RESET:
            g_ratioX = DEFAULT_RATIO_X;
            g_ratioY = DEFAULT_RATIO_Y;
            RenderAndShow(hwnd, g_ratioX, g_ratioY);
            SaveConfig();
            return 0;

        case IDM_SECOND_RESET:
            g_secondRatioX = SECOND_DOT_RATIO_X;
            g_secondRatioY = SECOND_DOT_RATIO_Y;
            RenderAndShow(hwnd, g_ratioX, g_ratioY);
            SaveConfig();
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        case IDM_COLOR:
        {
            CHOOSECOLORW cc = {0};
            static COLORREF acrCustClr[16] = {0};
            cc.lStructSize  = sizeof(cc);
            cc.hwndOwner    = hwnd;
            cc.rgbResult    = g_crossColor;
            cc.lpCustColors = acrCustClr;
            cc.Flags        = CC_RGBINIT | CC_FULLOPEN;
            if (ChooseColorW(&cc)) {
                g_crossColor = cc.rgbResult;
                RenderAndShow(hwnd, g_ratioX, g_ratioY);
                SaveConfig();
            }
            return 0;
        }
        case IDM_SIZE:
            g_editingSecond = FALSE;
            {
                HWND hDlg = CreateWindowExW(
                    WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
                    L"SizeDlgClass", L"调节大小",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                    CW_USEDEFAULT, CW_USEDEFAULT, 320, 140,
                    hwnd, NULL, GetModuleHandle(NULL), NULL);
                if (hDlg) {
                    ShowWindow(hDlg, SW_SHOW);
                    EnableWindow(hwnd, FALSE);
                    MSG dmsg;
                    while (GetMessage(&dmsg, NULL, 0, 0)) {
                        if (!IsDialogMessage(hDlg, &dmsg)) {
                            TranslateMessage(&dmsg);
                            DispatchMessage(&dmsg);
                        }
                    }
                    EnableWindow(hwnd, TRUE);
                    SetForegroundWindow(hwnd);
                    RenderAndShow(hwnd, g_ratioX, g_ratioY);
                    SaveConfig();
                }
            }
            return 0;
        case IDM_ALPHA_INC:
        case IDM_ALPHA_DEC:
        case IDM_ALPHA:
            g_editingSecond = FALSE;
            {
                HWND hDlg = CreateWindowExW(
                    WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
                    L"AlphaDlgClass", L"调节透明度",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                    CW_USEDEFAULT, CW_USEDEFAULT, 320, 140,
                    hwnd, NULL, GetModuleHandle(NULL), NULL);
                if (hDlg) {
                    ShowWindow(hDlg, SW_SHOW);
                    EnableWindow(hwnd, FALSE);
                    MSG dmsg;
                    while (GetMessage(&dmsg, NULL, 0, 0)) {
                        if (!IsDialogMessage(hDlg, &dmsg)) {
                            TranslateMessage(&dmsg);
                            DispatchMessage(&dmsg);
                        }
                    }
                    EnableWindow(hwnd, TRUE);
                    SetForegroundWindow(hwnd);
                    RenderAndShow(hwnd, g_ratioX, g_ratioY);
                    SaveConfig();
                }
            }
            return 0;
        case IDM_SECOND_SIZE:
            {
                BOOL prevEditing = g_editingSecond;
                g_editingSecond = TRUE;
                HWND hDlg = CreateWindowExW(
                    WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
                    L"SizeDlgClass", L"第二点 - 调节大小",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                    CW_USEDEFAULT, CW_USEDEFAULT, 320, 140,
                    hwnd, NULL, GetModuleHandle(NULL), NULL);
                if (hDlg) {
                    ShowWindow(hDlg, SW_SHOW);
                    EnableWindow(hwnd, FALSE);
                    MSG dmsg;
                    while (GetMessage(&dmsg, NULL, 0, 0)) {
                        if (!IsDialogMessage(hDlg, &dmsg)) {
                            TranslateMessage(&dmsg);
                            DispatchMessage(&dmsg);
                        }
                    }
                    EnableWindow(hwnd, TRUE);
                    SetForegroundWindow(hwnd);
                }
            g_editingSecond = prevEditing;
            }
            RenderAndShow(hwnd, g_ratioX, g_ratioY);
            SaveConfig();
            return 0;
        case IDM_SECOND_COLOR:
            {
                CHOOSECOLORW cc = {0};
                static COLORREF acrCustClr[16] = {0};
                cc.lStructSize  = sizeof(cc);
                cc.hwndOwner    = hwnd;
                cc.rgbResult    = g_secondColor;
                cc.lpCustColors = acrCustClr;
                cc.Flags        = CC_RGBINIT | CC_FULLOPEN;
                if (ChooseColorW(&cc)) {
                    g_secondColor = cc.rgbResult;
                    RenderAndShow(hwnd, g_ratioX, g_ratioY);
                    SaveConfig();
                }
            }
            return 0;
        case IDM_SECOND_ALPHA:
            {
                BOOL prevEditing = g_editingSecond;
                g_editingSecond = TRUE;
                HWND hDlg = CreateWindowExW(
                    WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
                    L"AlphaDlgClass", L"第二点 - 调节透明度",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                    CW_USEDEFAULT, CW_USEDEFAULT, 320, 140,
                    hwnd, NULL, GetModuleHandle(NULL), NULL);
                if (hDlg) {
                    ShowWindow(hDlg, SW_SHOW);
                    EnableWindow(hwnd, FALSE);
                    MSG dmsg;
                    while (GetMessage(&dmsg, NULL, 0, 0)) {
                        if (!IsDialogMessage(hDlg, &dmsg)) {
                            TranslateMessage(&dmsg);
                            DispatchMessage(&dmsg);
                        }
                    }
                    EnableWindow(hwnd, TRUE);
                    SetForegroundWindow(hwnd);
                }
                g_editingSecond = prevEditing;
            }
            RenderAndShow(hwnd, g_ratioX, g_ratioY);
            SaveConfig();
            return 0;
        case IDM_PRESET_BASE:
        case IDM_PRESET_BASE + 1:
        case IDM_PRESET_BASE + 2:
        case IDM_PRESET_BASE + 3:
            ApplyPreset((int)(LOWORD(wp) - IDM_PRESET_BASE) + 1);
            return 0;
        }
        case IDM_SECOND_TOGGLE:
            g_showSecondDot = !g_showSecondDot;
            RenderAndShow(hwnd, g_ratioX, g_ratioY);
            SaveConfig();
            return 0;
        return 0;
    case WM_DESTROY:
        SaveConfig();
        RemoveTrayIcon();
        if (g_dibBmp) { DeleteObject(g_dibBmp); DeleteDC(g_dibDC); }
        PostQuitMessage(0);
        return 0;

    case WM_DPICHANGED:
        RecalcScreenMetrics();
        RenderAndShow(hwnd, g_ratioX, g_ratioY);
        return 0;

    case WM_DISPLAYCHANGE:
        /* 分辨率变更后重新校准 */
        RecalcScreenMetrics();
        RenderAndShow(hwnd, g_ratioX, g_ratioY);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ===== 大小调节弹窗 ===== */
static LRESULT CALLBACK SizeDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
    {
        int trackW = 220, trackH = 30, margin = 15, btnW = 70, btnH = 28;
        int cx = margin, cy = margin;
        double cur = g_editingSecond ? g_secondLineRatio : g_lineRatioX;
        int sliderPos = (int)(cur * 1000.0 + 0.5);

        /* 提示标签 */
        CreateWindowW(L"STATIC", L"线长比例",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            cx, cy, 80, trackH,
            hwnd, (HMENU)3, GetModuleHandle(NULL), NULL);

        /* 滑块 */
        CreateWindowW(TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            cx, cy + 22, trackW, trackH,
            hwnd, (HMENU)1, GetModuleHandle(NULL), NULL);
        SendMessageW(GetDlgItem(hwnd, 1), TBM_SETRANGE, TRUE, MAKELONG(5, 200));
        SendMessageW(GetDlgItem(hwnd, 1), TBM_SETPOS, TRUE, sliderPos);

        /* 数值标签 */
        wchar_t buf[32];
        swprintf(buf, 32, L"%.3f", cur);
        CreateWindowW(L"STATIC", buf,
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            cx + trackW + 8, cy + 22, 60, trackH,
            hwnd, (HMENU)2, GetModuleHandle(NULL), NULL);

        /* 确定按钮 */
        CreateWindowW(L"BUTTON", L"确定",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            (320 - 30 - btnW) / 2, cy + trackH + 32, btnW, btnH,
            hwnd, (HMENU)IDOK, GetModuleHandle(NULL), NULL);

        /* 居中 */
        RECT rc;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
        RECT rcWnd; GetWindowRect(hwnd, &rcWnd);
        SetWindowPos(hwnd, NULL,
            rc.left + (rc.right - rc.left - (rcWnd.right - rcWnd.left)) / 2,
            rc.top  + (rc.bottom - rc.top - (rcWnd.bottom - rcWnd.top)) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER);
        break;
    }
    case WM_HSCROLL:
    {
        int pos = (int)SendMessageW(GetDlgItem(hwnd, 1), TBM_GETPOS, 0, 0);
        double v = pos * 0.001;
        wchar_t buf[32];
        swprintf(buf, 32, L"%.3f", v);
        SetWindowTextW(GetDlgItem(hwnd, 2), buf);
        if (g_editingSecond) {
            g_secondLineRatio = v;
        } else {
            ApplyLineRatio(v, v);
        }
        RenderAndShow(g_hwnd, g_ratioX, g_ratioY);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && HIWORD(wp) == BN_CLICKED) {
            DestroyWindow(hwnd);
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    default:
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

/* ===== 透明度调节弹窗 ===== */
static LRESULT CALLBACK AlphaDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
    {
        int trackW = 220, trackH = 30, margin = 15, btnW = 70, btnH = 28;
        int cx = margin, cy = margin;
        int curAlpha = g_editingSecond ? g_secondAlpha : g_alpha;

        /* 滑块 */
        CreateWindowW(TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            cx, cy, trackW, trackH,
            hwnd, (HMENU)1, GetModuleHandle(NULL), NULL);
        SendMessageW(GetDlgItem(hwnd, 1), TBM_SETRANGE, TRUE, MAKELONG(ALPHA_MIN, ALPHA_MAX));
        SendMessageW(GetDlgItem(hwnd, 1), TBM_SETPOS, TRUE, curAlpha);

        /* 数值标签 */
        wchar_t buf[32];
        swprintf(buf, 32, L"%d", curAlpha);
        CreateWindowW(L"STATIC", buf,
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            cx + trackW + 8, cy, 50, trackH,
            hwnd, (HMENU)2, GetModuleHandle(NULL), NULL);

        /* 确定按钮 */
        CreateWindowW(L"BUTTON", L"确定",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            (320 - 30 - btnW) / 2, cy + trackH + 8, btnW, btnH,
            hwnd, (HMENU)IDOK, GetModuleHandle(NULL), NULL);

        /* 居中 */
        RECT rc;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
        RECT rcWnd; GetWindowRect(hwnd, &rcWnd);
        SetWindowPos(hwnd, NULL,
            rc.left + (rc.right - rc.left - (rcWnd.right - rcWnd.left)) / 2,
            rc.top  + (rc.bottom - rc.top - (rcWnd.bottom - rcWnd.top)) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER);
        break;
    }
    case WM_HSCROLL:
    {
        int pos = (int)SendMessageW(GetDlgItem(hwnd, 1), TBM_GETPOS, 0, 0);
        wchar_t buf[32];
        swprintf(buf, 32, L"%d", pos);
        SetWindowTextW(GetDlgItem(hwnd, 2), buf);
        if (g_editingSecond) {
            g_secondAlpha = pos;
        } else {
            g_alpha = pos;
        }
        RenderAndShow(g_hwnd, g_ratioX, g_ratioY);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)
            DestroyWindow(hwnd);
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    default:
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

/* ===== 设置主窗口（苹果风格） ===== */

/* 控件 ID */
#define CID_POS_X       201
#define CID_POS_Y       202
#define CID_SIZE_SLIDER 203
#define CID_SIZE_LABEL  204
#define CID_COLOR_BTN   205
#define CID_COLOR_PREV  206
#define CID_ALPHA_SLIDER 207
#define CID_ALPHA_LABEL  208
#define CID_PRESET_BASE_SETTINGS 210
#define CID_SECOND_TOGGLE_SETTINGS 220
#define CID_SECOND_COLOR_BTN  221
#define CID_SECOND_COLOR_PREV 222
#define CID_SECOND_ALPHA_SLIDER 223
#define CID_SECOND_ALPHA_LABEL  224
#define CID_SECOND_SIZE_SLIDER  225
#define CID_SECOND_SIZE_LABEL   226
#define CID_SECOND_POS_X        227
#define CID_SECOND_POS_Y        228

#define COLOR_BG        RGB(246, 246, 246)

static void RefreshAllControls(HWND hDlg)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%.6f", g_ratioX);
    SetWindowTextW(GetDlgItem(hDlg, CID_POS_X), buf);
    swprintf(buf, 64, L"%.6f", g_ratioY);
    SetWindowTextW(GetDlgItem(hDlg, CID_POS_Y), buf);

    int sliderPos = (int)(g_lineRatioX * 1000.0 + 0.5);
    SendMessageW(GetDlgItem(hDlg, CID_SIZE_SLIDER), TBM_SETPOS, TRUE, sliderPos);
    swprintf(buf, 64, L"%.3f", g_lineRatioX);
    SetWindowTextW(GetDlgItem(hDlg, CID_SIZE_LABEL), buf);

    SendMessageW(GetDlgItem(hDlg, CID_ALPHA_SLIDER), TBM_SETPOS, TRUE, g_alpha);
    swprintf(buf, 64, L"%d", g_alpha);
    SetWindowTextW(GetDlgItem(hDlg, CID_ALPHA_LABEL), buf);

    SendMessageW(GetDlgItem(hDlg, CID_SECOND_TOGGLE_SETTINGS), BM_SETCHECK,
        g_showSecondDot ? BST_CHECKED : BST_UNCHECKED, 0);

    int sSliderPos = (int)(g_secondLineRatio * 1000.0 + 0.5);
    SendMessageW(GetDlgItem(hDlg, CID_SECOND_SIZE_SLIDER), TBM_SETPOS, TRUE, sSliderPos);
    swprintf(buf, 64, L"%.3f", g_secondLineRatio);
    SetWindowTextW(GetDlgItem(hDlg, CID_SECOND_SIZE_LABEL), buf);

    SendMessageW(GetDlgItem(hDlg, CID_SECOND_ALPHA_SLIDER), TBM_SETPOS, TRUE, g_secondAlpha);
    swprintf(buf, 64, L"%d", g_secondAlpha);
    SetWindowTextW(GetDlgItem(hDlg, CID_SECOND_ALPHA_LABEL), buf);

    swprintf(buf, 64, L"%.6f", g_secondRatioX);
    SetWindowTextW(GetDlgItem(hDlg, CID_SECOND_POS_X), buf);
    swprintf(buf, 64, L"%.6f", g_secondRatioY);
    SetWindowTextW(GetDlgItem(hDlg, CID_SECOND_POS_Y), buf);

    for (int i = 0; i < g_presetCount; i++) {
        SendMessageW(GetDlgItem(hDlg, CID_PRESET_BASE_SETTINGS + i), BM_SETCHECK,
            (g_currentPreset == i + 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    InvalidateRect(GetDlgItem(hDlg, CID_COLOR_PREV), NULL, TRUE);
    InvalidateRect(GetDlgItem(hDlg, CID_SECOND_COLOR_PREV), NULL, TRUE);
}

static void ShowSettingsWindow(HWND hwnd)
{
    if (g_hSettings && IsWindow(g_hSettings)) {
        ShowWindow(g_hSettings, SW_SHOW);
        SetForegroundWindow(g_hSettings);
        RefreshAllControls(g_hSettings);
        return;
    }
    g_hSettings = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"SettingsDlgClass", L"炸撤电竞",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 590,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    if (g_hSettings) {
        RECT rc;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
        RECT rcWnd; GetWindowRect(g_hSettings, &rcWnd);
        SetWindowPos(g_hSettings, NULL,
            rc.left + (rc.right - rc.left - (rcWnd.right - rcWnd.left)) / 2,
            rc.top  + (rc.bottom - rc.top - (rcWnd.bottom - rcWnd.top)) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER);
        ShowWindow(g_hSettings, SW_SHOW);
    }
}

static void ToggleSettingsWindow(HWND hwnd)
{
    if (g_hSettings && IsWindow(g_hSettings) && IsWindowVisible(g_hSettings)) {
        g_suppressActivate = TRUE;
        ShowWindow(g_hSettings, SW_HIDE);
    } else {
        ShowSettingsWindow(hwnd);
    }
}

static HBRUSH hSettingsBgBrush = NULL;

static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
    {
        hSettingsBgBrush = CreateSolidBrush(COLOR_BG);

        g_uiInitializing = TRUE;

        int rowY = 44;
        int labelW = 70, editW = 90;
        int col1X = 20;
        int col2X = 250;

        /* === 位置 X / Y === */
        CreateWindowW(L"STATIC", L"位置 X",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            col1X, rowY, labelW, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowW(L"EDIT", L"0.000000",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT,
            col1X + labelW + 2, rowY, editW, 22,
            hwnd, (HMENU)CID_POS_X, GetModuleHandle(NULL), NULL);

        CreateWindowW(L"STATIC", L"位置 Y",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            col2X, rowY, labelW, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowW(L"EDIT", L"0.000000",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT,
            col2X + labelW + 2, rowY, editW, 22,
            hwnd, (HMENU)CID_POS_Y, GetModuleHandle(NULL), NULL);

        rowY += 38;

        /* === 线长滑块 === */
        CreateWindowW(L"STATIC", L"线长比例",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            col1X, rowY, labelW, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowW(TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            col1X + labelW + 2, rowY, 170, 26,
            hwnd, (HMENU)CID_SIZE_SLIDER, GetModuleHandle(NULL), NULL);
        SendMessageW(GetDlgItem(hwnd, CID_SIZE_SLIDER), TBM_SETRANGE, TRUE, MAKELONG(5, 200));
        CreateWindowW(L"STATIC", L"0.000",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            col1X + labelW + 180, rowY, 55, 22,
            hwnd, (HMENU)CID_SIZE_LABEL, GetModuleHandle(NULL), NULL);

        rowY += 38;

        /* === 颜色 === */
        CreateWindowW(L"STATIC", L"准心颜色",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            col1X, rowY, labelW, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowW(L"BUTTON", L"取色...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            col1X + labelW + 2, rowY, 60, 22,
            hwnd, (HMENU)CID_COLOR_BTN, GetModuleHandle(NULL), NULL);
        CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY,
            col1X + labelW + 68, rowY, 30, 22,
            hwnd, (HMENU)CID_COLOR_PREV, GetModuleHandle(NULL), NULL);

        rowY += 38;

        /* === 透明度 === */
        CreateWindowW(L"STATIC", L"透明度",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            col1X, rowY, labelW, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowW(TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            col1X + labelW + 2, rowY, 170, 26,
            hwnd, (HMENU)CID_ALPHA_SLIDER, GetModuleHandle(NULL), NULL);
        SendMessageW(GetDlgItem(hwnd, CID_ALPHA_SLIDER), TBM_SETRANGE, TRUE, MAKELONG(ALPHA_MIN, ALPHA_MAX));
        CreateWindowW(L"STATIC", L"0",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            col1X + labelW + 180, rowY, 55, 22,
            hwnd, (HMENU)CID_ALPHA_LABEL, GetModuleHandle(NULL), NULL);

        rowY += 42;

        /* === 第二点 GROUPBOX === */
        CreateWindowW(L"GROUPBOX", L"第二准心",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            col1X, rowY, 420, 182,
            hwnd, NULL, GetModuleHandle(NULL), NULL);
        int r2x = col1X + 15;
        int r2y = rowY + 22;

        CreateWindowW(L"BUTTON", L"启用第二点",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            r2x, r2y, 100, 20,
            hwnd, (HMENU)CID_SECOND_TOGGLE_SETTINGS, GetModuleHandle(NULL), NULL);

        CreateWindowW(L"STATIC", L"颜色",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            r2x + 110, r2y, 30, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowW(L"BUTTON", L"取色...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            r2x + 145, r2y, 55, 20,
            hwnd, (HMENU)CID_SECOND_COLOR_BTN, GetModuleHandle(NULL), NULL);
        CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY,
            r2x + 205, r2y, 25, 20,
            hwnd, (HMENU)CID_SECOND_COLOR_PREV, GetModuleHandle(NULL), NULL);

        r2y += 28;

        CreateWindowW(L"STATIC", L"线长",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            r2x, r2y, 30, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowW(TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            r2x + 35, r2y, 170, 26,
            hwnd, (HMENU)CID_SECOND_SIZE_SLIDER, GetModuleHandle(NULL), NULL);
        SendMessageW(GetDlgItem(hwnd, CID_SECOND_SIZE_SLIDER), TBM_SETRANGE, TRUE, MAKELONG(5, 200));
        CreateWindowW(L"STATIC", L"0.000",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            r2x + 215, r2y, 55, 22,
            hwnd, (HMENU)CID_SECOND_SIZE_LABEL, GetModuleHandle(NULL), NULL);

        r2y += 32;

        CreateWindowW(L"STATIC", L"透明度",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            r2x, r2y, 30, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowW(TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            r2x + 35, r2y, 170, 26,
            hwnd, (HMENU)CID_SECOND_ALPHA_SLIDER, GetModuleHandle(NULL), NULL);
        SendMessageW(GetDlgItem(hwnd, CID_SECOND_ALPHA_SLIDER), TBM_SETRANGE, TRUE, MAKELONG(ALPHA_MIN, ALPHA_MAX));
        CreateWindowW(L"STATIC", L"0",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            r2x + 215, r2y, 55, 22,
            hwnd, (HMENU)CID_SECOND_ALPHA_LABEL, GetModuleHandle(NULL), NULL);

        r2y += 32;

        CreateWindowW(L"STATIC", L"位置 X",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            r2x, r2y, labelW, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowW(L"EDIT", L"0.000000",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT,
            r2x + labelW + 2, r2y, editW, 22,
            hwnd, (HMENU)CID_SECOND_POS_X, GetModuleHandle(NULL), NULL);

        CreateWindowW(L"STATIC", L"位置 Y",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            r2x + 180, r2y, labelW, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowW(L"EDIT", L"0.000000",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT,
            r2x + 180 + labelW + 2, r2y, editW, 22,
            hwnd, (HMENU)CID_SECOND_POS_Y, GetModuleHandle(NULL), NULL);

        rowY += 197;

        /* === 预设 GROUPBOX === */
        CreateWindowW(L"GROUPBOX", L"视野预设",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            col1X, rowY, 420, 55,
            hwnd, NULL, GetModuleHandle(NULL), NULL);
        const wchar_t *presetNames[] = { L"FOV90", L"FOV100", L"FOV110", L"FOV120" };
        int px = col1X + 20;
        for (int i = 0; i < g_presetCount; i++) {
            CreateWindowW(L"BUTTON", presetNames[i],
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0),
                px + i * 85, rowY + 22, 80, 20,
                hwnd, (HMENU)(CID_PRESET_BASE_SETTINGS + i), GetModuleHandle(NULL), NULL);
        }

        RefreshAllControls(hwnd);
        g_uiInitializing = FALSE;
        break;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, COLOR_BG);
        return (LRESULT)hSettingsBgBrush;
    }

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlID == CID_COLOR_PREV) {
            HBRUSH hBr = CreateSolidBrush(g_crossColor);
            FillRect(dis->hDC, &dis->rcItem, hBr);
            DeleteObject(hBr);
            DrawEdge(dis->hDC, &dis->rcItem, EDGE_SUNKEN, BF_RECT);
        } else if (dis->CtlID == CID_SECOND_COLOR_PREV) {
            HBRUSH hBr = CreateSolidBrush(g_secondColor);
            FillRect(dis->hDC, &dis->rcItem, hBr);
            DeleteObject(hBr);
            DrawEdge(dis->hDC, &dis->rcItem, EDGE_SUNKEN, BF_RECT);
        }
        return TRUE;
    }

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID((HWND)lp);
        int pos = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
        wchar_t buf[32];

        if (id == CID_SIZE_SLIDER) {
            double v = pos * 0.001;
            ApplyLineRatio(v, v);
            swprintf(buf, 32, L"%.3f", g_lineRatioX);
            SetWindowTextW(GetDlgItem(hwnd, CID_SIZE_LABEL), buf);
        } else if (id == CID_ALPHA_SLIDER) {
            g_alpha = pos;
            swprintf(buf, 32, L"%d", pos);
            SetWindowTextW(GetDlgItem(hwnd, CID_ALPHA_LABEL), buf);
        } else if (id == CID_SECOND_SIZE_SLIDER) {
            double v = pos * 0.001;
            if (v >= SIZE_MIN && v <= CROSS_SIZE_MAX) g_secondLineRatio = v;
            swprintf(buf, 32, L"%.3f", g_secondLineRatio);
            SetWindowTextW(GetDlgItem(hwnd, CID_SECOND_SIZE_LABEL), buf);
        } else if (id == CID_SECOND_ALPHA_SLIDER) {
            g_secondAlpha = pos;
            swprintf(buf, 32, L"%d", pos);
            SetWindowTextW(GetDlgItem(hwnd, CID_SECOND_ALPHA_LABEL), buf);
        }
        if (g_hwnd) RenderAndShow(g_hwnd, g_ratioX, g_ratioY);
        break;
    }

    case WM_COMMAND:
    {
        WORD id = LOWORD(wp);
        WORD code = HIWORD(wp);

        if (id == CID_COLOR_BTN && code == BN_CLICKED) {
            CHOOSECOLORW cc = {0};
            static COLORREF acrCustClr[16] = {0};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hwnd;
            cc.rgbResult = g_crossColor;
            cc.lpCustColors = acrCustClr;
            cc.Flags = CC_RGBINIT | CC_FULLOPEN;
            if (ChooseColorW(&cc)) {
                g_crossColor = cc.rgbResult;
                InvalidateRect(GetDlgItem(hwnd, CID_COLOR_PREV), NULL, TRUE);
                if (g_hwnd) RenderAndShow(g_hwnd, g_ratioX, g_ratioY);
            }
            return TRUE;
        }

        if (id == CID_SECOND_COLOR_BTN && code == BN_CLICKED) {
            CHOOSECOLORW cc = {0};
            static COLORREF acrCustClr[16] = {0};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hwnd;
            cc.rgbResult = g_secondColor;
            cc.lpCustColors = acrCustClr;
            cc.Flags = CC_RGBINIT | CC_FULLOPEN;
            if (ChooseColorW(&cc)) {
                g_secondColor = cc.rgbResult;
                InvalidateRect(GetDlgItem(hwnd, CID_SECOND_COLOR_PREV), NULL, TRUE);
                if (g_hwnd) RenderAndShow(g_hwnd, g_ratioX, g_ratioY);
            }
            return TRUE;
        }

        if (id == CID_SECOND_TOGGLE_SETTINGS) {
            g_showSecondDot = (SendMessageW(GetDlgItem(hwnd, CID_SECOND_TOGGLE_SETTINGS), BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (g_hwnd) RenderAndShow(g_hwnd, g_ratioX, g_ratioY);
            return TRUE;
        }

        if ((id == CID_POS_X || id == CID_POS_Y) && code == EN_CHANGE) {
            if (g_uiInitializing) return TRUE;
            wchar_t buf[64];
            if (id == CID_POS_X) {
                GetWindowTextW(GetDlgItem(hwnd, CID_POS_X), buf, 64);
                g_ratioX = _wtof(buf);
            } else {
                GetWindowTextW(GetDlgItem(hwnd, CID_POS_Y), buf, 64);
                g_ratioY = _wtof(buf);
            }
            if (g_hwnd) RenderAndShow(g_hwnd, g_ratioX, g_ratioY);
            return TRUE;
        }

        if ((id == CID_SECOND_POS_X || id == CID_SECOND_POS_Y) && code == EN_CHANGE) {
            if (g_uiInitializing) return TRUE;
            wchar_t buf[64];
            if (id == CID_SECOND_POS_X) {
                GetWindowTextW(GetDlgItem(hwnd, CID_SECOND_POS_X), buf, 64);
                g_secondRatioX = _wtof(buf);
            } else {
                GetWindowTextW(GetDlgItem(hwnd, CID_SECOND_POS_Y), buf, 64);
                g_secondRatioY = _wtof(buf);
            }
            if (g_hwnd) RenderAndShow(g_hwnd, g_ratioX, g_ratioY);
            SaveConfig();
            return TRUE;
        }

        if (id >= CID_PRESET_BASE_SETTINGS && id < CID_PRESET_BASE_SETTINGS + g_presetCount) {
            int presetIdx = id - CID_PRESET_BASE_SETTINGS + 1;
            ApplyPresetCore(presetIdx);
            RefreshAllControls(hwnd);
            if (g_hwnd) RenderAndShow(g_hwnd, g_ratioX, g_ratioY);
            SaveConfig();
            return TRUE;
        }
        break;
    }

    case WM_SYSCOMMAND:
        break;

    case WM_CLOSE:
        SaveConfig();
        if (g_hwnd) DestroyWindow(g_hwnd);
        DestroyWindow(hwnd);
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        g_hSettings = NULL;
        if (hSettingsBgBrush) { DeleteObject(hSettingsBgBrush); hSettingsBgBrush = NULL; }
        SaveConfig();
        return 0;

    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH hBr = CreateSolidBrush(COLOR_BG);
        FillRect((HDC)wp, &rc, hBr);
        DeleteObject(hBr);
        return TRUE;
    }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ---------- 入口 ---------- */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
    /* 初始化 GDI+ */
    GdiplusStartupInput gpsi;
    GdiplusStartup(&g_gdiplusToken, &gpsi, NULL);

    /* 初始化通用控件（TrackBar 等） */
    InitCommonControls();

    /* Per-Monitor DPI V2：动态加载，失败回退 System DPI Aware */
    {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *FnSetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
        FnSetProcessDpiAwarenessContext pfn =
            (FnSetProcessDpiAwarenessContext)GetProcAddress(hUser32,
                "SetProcessDpiAwarenessContext");
        if (pfn) {
            pfn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            SetProcessDPIAware();
        }
    }

    /* 注册窗口类 */
    const wchar_t *CLASS = L"CrosshairWnd";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(1));
    wc.hCursor       = LoadCursor(NULL, IDC_CROSS);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CLASS;
    RegisterClassW(&wc);

    /* 注册透明度弹窗窗口类 */
    {
        WNDCLASSW awc = {0};
        awc.lpfnWndProc   = AlphaDlgProc;
        awc.hInstance     = hInst;
        awc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        awc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        awc.lpszClassName = L"AlphaDlgClass";
        RegisterClassW(&awc);
    }

    /* 注册大小调节弹窗窗口类 */
    {
        WNDCLASSW swc = {0};
        swc.lpfnWndProc   = SizeDlgProc;
        swc.hInstance     = hInst;
        swc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        swc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        swc.lpszClassName = L"SizeDlgClass";
        RegisterClassW(&swc);
    }

    /* 注册设置主窗口窗口类 */
    {
        WNDCLASSW stwc = {0};
        stwc.lpfnWndProc   = (WNDPROC)SettingsDlgProc;
        stwc.hInstance     = hInst;
        stwc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        stwc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        stwc.lpszClassName = L"SettingsDlgClass";
        RegisterClassW(&stwc);
    }

    /* 创建分层置顶窗口 */
    {
        int scrW = GetSystemMetrics(SM_CXSCREEN);
        int scrH = GetSystemMetrics(SM_CYSCREEN);
        g_halfW = scrW / 2;
        g_halfH = scrH / 2;
        int sz = CalcWindowSize();
        g_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW,
            CLASS, L"", WS_POPUP,
            0, 0, sz, sz,
            NULL, NULL, hInst, NULL);
    }

    if (!g_hwnd) return 1;

    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    /* 弹出设置窗口 */
    ShowSettingsWindow(g_hwnd);

    /* 释放焦点，使任务栏单击能触发 WM_ACTIVATE */
    SetForegroundWindow(GetDesktopWindow());

    /* 消息循环 */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}
