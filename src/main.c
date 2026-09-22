/* updata.exe —— 差异化自动更新程序（Win32 GUI）
   用法：updata.exe <更新检测地址> [更新完成后启动程序的附加命令行] */
#define _WIN32_WINNT 0x0601
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "util.h"
#include "lockcheck.h"
#include "http.h"

/* ---------------- 控件 ID ---------------- */
#define IDC_LIST        1001
#define IDC_BTN_START   1002
#define IDC_BTN_STOP    1003
#define IDC_BTN_RETRY   1004
#define IDC_BTN_CLOSE   1005

#define IDC_DLG_LIST    1101
#define IDC_DLG_KILL    1102
#define IDC_DLG_SKIP    1103
#define IDC_DLG_CANCEL  1104
#define IDC_DLG_TASKMGR 1105

#define IDC_DETAIL_DESC  1201
#define IDC_DETAIL_LOG   1202
#define IDC_DETAIL_CLOSE 1203

#define TIMER_REFRESH   1
#define TIMER_CLOSE     2

/* 列 */
enum { COL_NAME = 0, COL_DIR, COL_SIZE, COL_PROGRESS, COL_SPEED, COL_STATE, COL_NOTE, COL_MAX };

/* 颜色 */
#define C_BG        RGB(243, 246, 251)
#define C_CARD      RGB(255, 255, 255)
#define C_BORDER    RGB(226, 232, 240)
#define C_TEXT      RGB(30, 41, 59)
#define C_TEXT2     RGB(100, 116, 139)
#define C_ACCENT    RGB(37, 99, 235)
#define C_ACCENT_D  RGB(29, 78, 216)
#define C_HEAD1     RGB(29, 78, 216)
#define C_HEAD2     RGB(79, 70, 229)
#define C_GREEN     RGB(22, 163, 74)
#define C_RED       RGB(220, 38, 38)
#define C_ORANGE    RGB(217, 119, 6)
#define C_TRACK     RGB(229, 231, 235)
#define C_ROW_ALT   RGB(249, 250, 252)
#define C_SEL       RGB(219, 234, 254)

#define BTN_START   0
#define BTN_STOP    1
#define BTN_RETRY   2
#define BTN_CLOSE   3
#define BTN_COUNT   4

#define DLG_KILL    0
#define DLG_SKIP    1
#define DLG_CANCEL  2
#define DLG_TASKMGR 3
#define DLG_BTN_COUNT 4

typedef struct {
    wchar_t text[64];
    int     primary;
    int     hover;
    int     pressed;
    int     enabled;
    COLORREF bg;
} BtnState;

static HINSTANCE g_hInst;
static HWND  g_hMain, g_hList;
static HWND  g_hBtn[BTN_COUNT];
static BtnState g_btn[BTN_COUNT];

static HFONT g_fTitle, g_fSub, g_fSmall, g_fBold, g_fUI, g_fBtn, g_fBig;
static int   g_dpi = 96;

static RECT  g_rcHeader, g_rcStatus, g_rcCard, g_rcProgress, g_rcStat1, g_rcStat2;
static RECT  g_rcDetailBtn, g_rcDesc, g_rcLog;   /* 头部可点击区域 */
static int   g_detailHover;
static int   g_mouseTracked;
static int   *g_lastPercent;
static LONG  *g_lastState;
static DWORD g_lastTick;
static unsigned long long g_lastBytes;
static unsigned long long g_bytesDone;
static unsigned long long g_speed;
static int   g_closeCountdown;

/* 锁占用对话框 */
static HWND      g_hDlg;
static LockInfo *g_lockList;
static int       g_lockCount;
static HWND      g_hDlgList;
static HWND      g_hDlgBtn[DLG_BTN_COUNT];
static BtnState  g_dlgBtn[DLG_BTN_COUNT];
static int       g_lockAttempts;
static wchar_t   g_dlgStatus[512];
static int       g_dlgResultGiven;

static int   S(int v) { return MulDiv(v, g_dpi, 96); }

/* ------------------------------------------------------------------ */
/* 字体与绘制辅助                                                       */
/* ------------------------------------------------------------------ */
static HFONT make_font(int pt, int bold)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(pt, g_dpi, 72);
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Microsoft YaHei UI", 32);
    return CreateFontIndirectW(&lf);
}

static void create_fonts(void)
{
    g_fUI    = make_font(9, 0);
    g_fSmall = make_font(8, 0);
    g_fBold  = make_font(9, 1);
    g_fBtn   = make_font(9, 0);
    g_fSub   = make_font(10, 0);
    g_fTitle = make_font(17, 1);
    g_fBig   = make_font(12, 1);
}

static void fill_rect(HDC hdc, RECT *rc, COLORREF color)
{
    HBRUSH br = CreateSolidBrush(color);
    FillRect(hdc, rc, br);
    DeleteObject(br);
}

static void fill_round(HDC hdc, RECT *rc, int radius, COLORREF color)
{
    HBRUSH br = CreateSolidBrush(color);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    HGDIOBJ oldBr = SelectObject(hdc, br);
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(br);
}

static void draw_text(HDC hdc, const wchar_t *text, RECT *rc, HFONT font, COLORREF color,
                      UINT format)
{
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text, -1, rc, format);
    SelectObject(hdc, oldFont);
}

static void draw_v_gradient(HDC hdc, RECT *rc, COLORREF c1, COLORREF c2)
{
    int steps = 48, i;
    int w = rc->right - rc->left;
    if (w <= 0) return;
    for (i = 0; i < steps; ++i) {
        RECT r = *rc;
        int x1 = rc->left + w * i / steps;
        int x2 = rc->left + w * (i + 1) / steps;
        BYTE rr = (BYTE)(GetRValue(c1) + (int)(GetRValue(c2) - GetRValue(c1)) * i / steps);
        BYTE gg = (BYTE)(GetGValue(c1) + (int)(GetGValue(c2) - GetGValue(c1)) * i / steps);
        BYTE bb = (BYTE)(GetBValue(c1) + (int)(GetBValue(c2) - GetBValue(c1)) * i / steps);
        r.left = x1;
        r.right = x2 + 1;
        fill_rect(hdc, &r, RGB(rr, gg, bb));
    }
}

static COLORREF state_color(LONG st)
{
    switch (st) {
    case FS_DONE:   return C_GREEN;
    case FS_FAIL:   return C_RED;
    case FS_LOCKED: return C_ORANGE;
    case FS_DOWNLOAD:
    case FS_VERIFY: return C_ACCENT;
    case FS_CURRENT: return RGB(5, 150, 105);
    default:        return C_TEXT2;
    }
}

static const wchar_t *state_text(LONG st)
{
    switch (st) {
    case FS_WAIT:        return L"等待中";
    case FS_SKIP_ACTIVE: return L"已禁用";
    case FS_CURRENT:     return L"已是最新";
    case FS_SKIP_EXISTS: return L"已存在";
    case FS_DOWNLOAD:    return L"下载中";
    case FS_VERIFY:      return L"校验中";
    case FS_DONE:        return L"已完成";
    case FS_FAIL:        return L"失败";
    case FS_CANCEL:      return L"已取消";
    case FS_LOCKED:      return L"被占用";
    default:             return L"";
    }
}

/* ------------------------------------------------------------------ */
/* 自绘按钮                                                            */
/* ------------------------------------------------------------------ */
static void btn_paint(LPDRAWITEMSTRUCT dis, BtnState *bs)
{
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    int pressed = (dis->itemState & ODS_SELECTED) ? 1 : 0;
    int disabled = (dis->itemState & ODS_DISABLED) ? 1 : 0;
    COLORREF bg, fg;
    int radius = S(6);

    fill_rect(hdc, &rc, bs->bg);

    if (bs->primary) {
        bg = disabled ? RGB(147, 180, 240) : (pressed ? C_ACCENT_D : (bs->hover ? RGB(59, 130, 246) : C_ACCENT));
        fg = RGB(255, 255, 255);
    } else {
        bg = disabled ? RGB(240, 242, 246) : (pressed ? RGB(226, 232, 240) : (bs->hover ? RGB(241, 245, 249) : RGB(255, 255, 255)));
        fg = disabled ? RGB(148, 163, 184) : (bs->hover ? C_ACCENT_D : C_TEXT);
    }

    fill_round(hdc, &rc, radius, bg);
    if (!bs->primary) {
        HBRUSH br = CreateSolidBrush(pressed ? RGB(203, 213, 225) : C_BORDER);
        FrameRect(hdc, &rc, br);
        DeleteObject(br);
    }
    draw_text(hdc, bs->text, &rc, g_fBtn, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static LRESULT CALLBACK btn_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, DWORD_PTR ref)
{
    BtnState *bs = (BtnState *)ref;
    switch (msg) {
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme;
        if (!bs->hover) {
            bs->hover = 1;
            InvalidateRect(hwnd, NULL, TRUE);
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE:
        bs->hover = 0;
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static HWND btn_create(HWND parent, int id, BtnState *bs, const wchar_t *text, int primary,
                       COLORREF bg, RECT rc, HFONT font)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                             rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                             parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
    if (!h) return NULL;
    lstrcpynW(bs->text, text, 64);
    bs->primary = primary;
    bs->bg = bg;
    bs->enabled = 1;
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    SetWindowSubclass(h, btn_subclass, 1, (DWORD_PTR)bs);
    return h;
}

static void btn_enable(HWND h, BtnState *bs, int enable)
{
    if (!h) return;
    bs->enabled = enable ? 1 : 0;
    EnableWindow(h, enable ? TRUE : FALSE);
    InvalidateRect(h, NULL, TRUE);
}

static int btn_index_from_hwnd(HWND hwndItem, HWND *arr, int n)
{
    int i;
    for (i = 0; i < n; ++i) {
        if (arr[i] == hwndItem) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* 主窗口绘制                                                          */
/* ------------------------------------------------------------------ */
static void compute_layout(HWND hwnd)
{
    RECT rc;
    int pad = S(16), headerH = S(104);
    int btnH = S(34), btnW = S(100), gap = S(10), y;
    int barH = S(10), statH1 = S(20), statH2 = S(18);

    GetClientRect(hwnd, &rc);

    g_rcHeader.left = 0; g_rcHeader.top = 0;
    g_rcHeader.right = rc.right; g_rcHeader.bottom = headerH;

    /* 头部：描述 / 更新日志（可点击查看全文）与右上角“查看详情” */
    g_rcDesc.left = S(90); g_rcDesc.top = S(52);
    g_rcDesc.right = rc.right - S(20); g_rcDesc.bottom = g_rcDesc.top + S(20);

    g_rcLog.left = S(90); g_rcLog.top = S(74);
    g_rcLog.right = rc.right - S(20); g_rcLog.bottom = g_rcLog.top + S(20);

    g_rcDetailBtn.left = rc.right - S(20) - S(88);
    g_rcDetailBtn.top = S(22);
    g_rcDetailBtn.right = g_rcDetailBtn.left + S(88);
    g_rcDetailBtn.bottom = g_rcDetailBtn.top + S(30);

    /* 底部区域：自下而上排布，避免与按钮重叠 */
    y = rc.bottom - S(16) - btnH;                       /* 按钮行顶边 */

    g_rcStat2.left = pad; g_rcStat2.right = rc.right - pad;
    g_rcStat2.bottom = y - S(12);
    g_rcStat2.top = g_rcStat2.bottom - statH2;

    g_rcStat1.left = pad; g_rcStat1.right = rc.right - pad;
    g_rcStat1.bottom = g_rcStat2.top - S(4);
    g_rcStat1.top = g_rcStat1.bottom - statH1;

    g_rcProgress.left = pad; g_rcProgress.right = rc.right - pad;
    g_rcProgress.bottom = g_rcStat1.top - S(8);
    g_rcProgress.top = g_rcProgress.bottom - barH;

    /* 状态行 */
    g_rcStatus.left = pad; g_rcStatus.top = headerH + S(14);
    g_rcStatus.right = rc.right - pad; g_rcStatus.bottom = g_rcStatus.top + S(22);

    /* 列表卡片 */
    g_rcCard.left = pad;
    g_rcCard.right = rc.right - pad;
    g_rcCard.top = g_rcStatus.bottom + S(10);
    g_rcCard.bottom = g_rcProgress.top - S(18);
    if (g_rcCard.bottom < g_rcCard.top + S(60)) g_rcCard.bottom = g_rcCard.top + S(60);

    if (g_hList) {
        MoveWindow(g_hList, g_rcCard.left + 1, g_rcCard.top + 1,
                   g_rcCard.right - g_rcCard.left - 2, g_rcCard.bottom - g_rcCard.top - 2, TRUE);
    }

    /* 按钮右对齐：关闭 / 重新检测 / 停止 / 开始更新 */
    {
        int x = rc.right - pad - btnW;
        if (g_hBtn[BTN_CLOSE]) MoveWindow(g_hBtn[BTN_CLOSE], x, y, btnW, btnH, TRUE);
        x -= gap + btnW;
        if (g_hBtn[BTN_RETRY]) MoveWindow(g_hBtn[BTN_RETRY], x, y, btnW, btnH, TRUE);
        x -= gap + btnW;
        if (g_hBtn[BTN_STOP]) MoveWindow(g_hBtn[BTN_STOP], x, y, btnW, btnH, TRUE);
        x -= gap + S(110);
        if (g_hBtn[BTN_START]) MoveWindow(g_hBtn[BTN_START], x, y, S(110), btnH, TRUE);

        /* 统计文字不要压到按钮上 */
        if (g_rcStat1.right > x - S(12)) g_rcStat1.right = x - S(12);
        if (g_rcStat2.right > x - S(12)) g_rcStat2.right = x - S(12);
    }

    /* 列宽自适应 */
    if (g_hList && g_app.fileCount > 0) {
        int w = g_rcCard.right - g_rcCard.left - 2;
        ListView_SetColumnWidth(g_hList, COL_NAME, S(170));
        ListView_SetColumnWidth(g_hList, COL_DIR, S(110));
        ListView_SetColumnWidth(g_hList, COL_SIZE, S(90));
        ListView_SetColumnWidth(g_hList, COL_PROGRESS, S(150));
        ListView_SetColumnWidth(g_hList, COL_SPEED, S(100));
        ListView_SetColumnWidth(g_hList, COL_STATE, S(80));
        ListView_SetColumnWidth(g_hList, COL_NOTE, w > S(1000) ? w - S(700) : S(200));
    }
}

/* 头部图标里的“向下更新箭头” */
static void draw_update_glyph(HDC hdc, RECT box, COLORREF color)
{
    int w = box.right - box.left;
    int cx = (box.left + box.right) / 2;
    HBRUSH br = CreateSolidBrush(color);
    HGDIOBJ oldBr = SelectObject(hdc, br);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    RECT stem;
    POINT tri[3];
    int stemW = w / 9 > 2 ? w / 9 : 3;

    stem.left = cx - stemW / 2;
    stem.right = stem.left + stemW;
    stem.top = box.top + w / 5;
    stem.bottom = box.top + w * 55 / 100;
    FillRect(hdc, &stem, br);

    tri[0].x = cx - w / 5; tri[0].y = stem.bottom - stemW / 2;
    tri[1].x = cx + w / 5; tri[1].y = stem.bottom - stemW / 2;
    tri[2].x = cx;         tri[2].y = box.bottom - w / 4;
    Polygon(hdc, tri, 3);

    stem.left = box.left + w / 4;
    stem.right = box.right - w / 4;
    stem.top = box.bottom - w / 5;
    stem.bottom = stem.top + (stemW / 2 + 1);
    FillRect(hdc, &stem, br);

    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(br);
}

static void paint_header(HDC hdc)
{
    RECT rc = g_rcHeader;
    RECT r, rTitle, rChip;
    wchar_t buf[512];
    SIZE tsz, vsz;
    int x0 = S(90), rightLimit = rc.right - S(20);
    int titleRight;
    int chipLeft = 0, chipW = 0;
    HGDIOBJ oldFont;

    /* 标题不要被右上角“查看详情”压住 */
    titleRight = g_rcDetailBtn.left - S(14);
    if (titleRight < x0 + S(80)) titleRight = rightLimit;

    draw_v_gradient(hdc, &rc, C_HEAD1, C_HEAD2);

    /* 图标块 + 更新箭头 */
    r.left = S(18); r.top = S(20); r.right = r.left + S(56); r.bottom = r.top + S(56);
    fill_round(hdc, &r, S(12), RGB(255, 255, 255));
    draw_update_glyph(hdc, r, C_ACCENT);

    /* 用标题字体测量标题宽度，版本徽标紧跟在标题后面 */
    oldFont = SelectObject(hdc, g_fTitle);
    GetTextExtentPoint32W(hdc, g_app.name[0] ? g_app.name : L"软件更新",
                          (int)wcslen(g_app.name[0] ? g_app.name : L"软件更新"), &tsz);
    SelectObject(hdc, oldFont);

    if (g_app.version[0]) {
        oldFont = SelectObject(hdc, g_fBold);
        _snwprintf(buf, 512, L"v%s", g_app.version);
        GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &vsz);
        SelectObject(hdc, oldFont);

        chipW = vsz.cx + S(20);
        chipLeft = x0 + tsz.cx + S(12);

        rTitle.left = x0; rTitle.top = S(14);
        rTitle.right = titleRight;
        rTitle.bottom = rTitle.top + S(34);

        if (chipLeft + chipW <= rTitle.right - S(10)) {
            rChip.left = chipLeft; rChip.right = chipLeft + chipW;
            rChip.top = S(21); rChip.bottom = rChip.top + S(23);
            fill_round(hdc, &rChip, S(11), RGB(255, 255, 255));
            draw_text(hdc, buf, &rChip, g_fBold, C_HEAD1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            rTitle.right = rChip.left - S(10);
            if (rTitle.right < rTitle.left + S(60)) rTitle.right = titleRight;
        }
    } else {
        rTitle.left = x0; rTitle.top = S(14);
        rTitle.right = titleRight;
        rTitle.bottom = rTitle.top + S(34);
    }

    /* 标题 */
    draw_text(hdc, g_app.name[0] ? g_app.name : L"软件更新", &rTitle, g_fTitle, RGB(255, 255, 255),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* 右上角“查看详情” */
    {
        COLORREF bgc = g_detailHover ? RGB(255, 255, 255) : RGB(236, 242, 255);
        COLORREF fgc = g_detailHover ? C_ACCENT_D : C_HEAD1;
        fill_round(hdc, &g_rcDetailBtn, S(8), bgc);
        draw_text(hdc, L"查看详情", &g_rcDetailBtn, g_fBtn, fgc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* 描述（过长省略，点击可看全文） */
    draw_text(hdc, g_app.desc[0] ? g_app.desc : L"（暂无描述）", &g_rcDesc, g_fSub,
              RGB(219, 234, 254), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* 更新日志（过长省略，点击可看全文） */
    _snwprintf(buf, 512, L"更新日志：%s", g_app.changelog[0] ? g_app.changelog : L"-");
    buf[511] = 0;
    draw_text(hdc, buf, &g_rcLog, g_fSmall, RGB(191, 219, 254),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void format_duration(DWORD ms, wchar_t *out, int cap)
{
    DWORD sec = ms / 1000;
    _snwprintf(out, (size_t)cap, L"%02lu:%02lu", sec / 60, sec % 60);
    out[cap - 1] = 0;
}

static void paint_footer(HDC hdc)
{
    wchar_t buf[512], s1[64], s2[64], s3[64], s4[64];
    int pct = 0;
    RECT r;

    /* 总进度条 */
    {
        RECT track = g_rcProgress;
        int radius = S(10);
        fill_round(hdc, &track, radius, C_TRACK);
        if (g_app.bytesTotal > 0) {
            pct = (int)(g_bytesDone * 100 / g_app.bytesTotal);
            if (pct > 100) pct = 100;
        }
        if (pct > 0) {
            RECT fill = track;
            int w = track.right - track.left;
            fill.right = fill.left + (w * pct / 100);
            if (fill.right - fill.left < radius) fill.right = fill.left + radius;
            fill_round(hdc, &fill, radius, g_app.failCount > 0 ? C_ACCENT : C_GREEN);
        }
    }

    /* 统计 1 */
    util_fmt_size(g_app.bytesTotal, s1, 64);
    util_fmt_size(g_bytesDone, s2, 64);
    util_fmt_speed(g_speed, s3, 64);
    if (g_speed > 0 && g_app.bytesTotal > g_bytesDone) {
        unsigned long long remain = (g_app.bytesTotal - g_bytesDone) / g_speed;
        util_fmt_time(remain, s4, 64);
    } else {
        lstrcpynW(s4, L"--", 64);
    }

    _snwprintf(buf, 512, L"总进度 %d%%    已下载 %s / %s    速度 %s    剩余 %s", pct, s2, s1, s3, s4);
    buf[511] = 0;
    r = g_rcStat1;
    draw_text(hdc, buf, &r, g_fBold, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* 统计 2 */
    {
        wchar_t t[64];
        DWORD elapsed = g_app.startTick ? ((g_app.endTick ? g_app.endTick : GetTickCount()) - g_app.startTick) : 0;
        format_duration(elapsed, t, 64);
        _snwprintf(buf, 512, L"下载线程 %d    需更新 %d 个文件    完成 %d    失败 %d    跳过 %d    耗时 %s",
                   g_app.threadCount, g_app.updateCount, g_app.doneCount, g_app.failCount,
                   g_app.skipCount, t);
        buf[511] = 0;
    }
    r = g_rcStat2;
    draw_text(hdc, buf, &r, g_fSmall, C_TEXT2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void paint_main(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ oldBmp;
    RECT rc, r;

    hdc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &rc);

    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    oldBmp = SelectObject(mem, bmp);

    fill_rect(mem, &rc, C_BG);
    paint_header(mem);

    /* 状态行 */
    r = g_rcStatus;
    draw_text(mem, g_app.status, &r, g_fBig, C_TEXT,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* 列表卡片边框 */
    {
        HBRUSH border = CreateSolidBrush(C_BORDER);
        FrameRect(mem, &g_rcCard, border);
        DeleteObject(border);
    }

    paint_footer(mem);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------------ */
/* 列表                                                                */
/* ------------------------------------------------------------------ */
static void lv_set_text(HWND hList, int item, int sub, const wchar_t *text)
{
    ListView_SetItemText(hList, item, sub, (LPWSTR)text);
}

static void list_add_file(int index)
{
    FileTask *t = &g_app.files[index];
    LVITEMW it;
    wchar_t buf[64];
    wchar_t dir[512];

    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = index;
    it.iSubItem = 0;
    it.pszText = t->nameW;
    ListView_InsertItem(g_hList, &it);

    /* 目录 */
    {
        const wchar_t *s = wcsrchr(t->relW, L'\\');
        if (s && s != t->relW) {
            size_t n = (size_t)(s - t->relW);
            if (n > 511) n = 511;
            wcsncpy(dir, t->relW, n);
            dir[n] = 0;
        } else {
            lstrcpynW(dir, L"(根目录)", 512);
        }
        lv_set_text(g_hList, index, COL_DIR, dir);
    }

    util_fmt_size(t->size, buf, 64);
    lv_set_text(g_hList, index, COL_SIZE, buf);
    lv_set_text(g_hList, index, COL_PROGRESS, L"0%");
    lv_set_text(g_hList, index, COL_SPEED, L"-");
    lv_set_text(g_hList, index, COL_STATE, state_text(t->state));

    if (t->state == FS_CURRENT) {
        lv_set_text(g_hList, index, COL_NOTE, L"本地文件已是最新");
    } else if (t->state == FS_SKIP_ACTIVE) {
        lv_set_text(g_hList, index, COL_NOTE, L"服务端已禁用");
    } else if (t->state == FS_SKIP_EXISTS) {
        lv_set_text(g_hList, index, COL_NOTE, L"文件已存在，无需更新");
    } else if (t->needUpdate) {
        wchar_t note[128];
        _snwprintf(note, 128, L"SHA256 %.8S...", t->hash);
        note[127] = 0;
        lv_set_text(g_hList, index, COL_NOTE, note);
    }
}

static void list_rebuild(void)
{
    int i;
    ListView_DeleteAllItems(g_hList);
    if (!g_app.files) return;
    for (i = 0; i < g_app.fileCount; ++i) list_add_file(i);
}

static void draw_progress_cell(LPNMLVCUSTOMDRAW cd)
{
    int item = (int)cd->nmcd.dwItemSpec;
    HDC hdc = cd->nmcd.hdc;
    HWND hList = cd->nmcd.hdr.hwndFrom;
    RECT rc, bar, txt, r;
    FileTask *t = &g_app.files[item];
    LONG st = t->state;
    int pct = 0, selected;
    COLORREF bg, fill, fg;
    wchar_t buf[32];
    unsigned long long got;

    if (ListView_GetSubItemRect(hList, item, COL_PROGRESS, LVIR_BOUNDS, &rc) == FALSE) return;

    selected = (ListView_GetItemState(hList, item, LVIS_SELECTED) & LVIS_SELECTED) ? 1 : 0;
    bg = selected ? C_SEL : ((item % 2) ? C_ROW_ALT : C_CARD);
    fill_rect(hdc, &rc, bg);

    EnterCriticalSection(&g_statsLock);
    got = t->got;
    LeaveCriticalSection(&g_statsLock);

    if (t->needUpdate && t->size > 0) {
        pct = (int)(got * 100 / t->size);
        if (pct > 100) pct = 100;
        if (st == FS_DONE) pct = 100;
    } else if (!t->needUpdate) {
        pct = 0;
    }

    /* 左侧百分比文字 */
    _snwprintf(buf, 32, L"%d%%", pct);
    if (t->needUpdate || st == FS_DONE) {
        txt = rc;
        txt.left += S(8);
        txt.right = txt.left + S(48);
        draw_text(hdc, buf, &txt, g_fSmall, (st == FS_FAIL || st == FS_LOCKED) ? C_RED : C_TEXT2,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        txt = rc;
        txt.left += S(8);
        draw_text(hdc, L"-", &txt, g_fSmall, C_TEXT2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    /* 右侧进度条 */
    bar = rc;
    bar.left += S(60);
    bar.right -= S(10);
    if (bar.right > bar.left + S(10)) {
        int h = S(8);
        int cy = (rc.top + rc.bottom) / 2;
        bar.top = cy - h / 2;
        bar.bottom = bar.top + h;
        fill_round(hdc, &bar, h, C_TRACK);
        if (pct > 0) {
            RECT f = bar;
            int w = bar.right - bar.left;
            f.right = f.left + w * pct / 100;
            if (f.right - f.left < h) f.right = f.left + h;
            if (st == FS_DONE) fill = C_GREEN;
            else if (st == FS_FAIL) fill = C_RED;
            else if (st == FS_LOCKED) fill = C_ORANGE;
            else if (st == FS_CANCEL) fill = RGB(148, 163, 184);
            else fill = C_ACCENT;
            fill_round(hdc, &f, h, fill);
        }
    }
    (void)fg;
    (void)r;
}

static LRESULT list_custom_draw(LPNMLVCUSTOMDRAW cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        int item = (int)cd->nmcd.dwItemSpec;
        int selected = (cd->nmcd.uItemState & CDIS_SELECTED) ? 1 : 0;
        if (selected) {
            cd->clrTextBk = C_SEL;
            cd->clrText = C_TEXT;
        } else {
            cd->clrTextBk = (item % 2) ? C_ROW_ALT : C_CARD;
            cd->clrText = C_TEXT;
        }
        return CDRF_NOTIFYSUBITEMDRAW;
    }
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        int item = (int)cd->nmcd.dwItemSpec;
        int selected = (ListView_GetItemState(g_hList, item, LVIS_SELECTED) & LVIS_SELECTED) ? 1 : 0;
        if (cd->iSubItem == COL_PROGRESS) {
            draw_progress_cell(cd);
            return CDRF_SKIPDEFAULT;
        }
        if (selected) {
            cd->clrText = C_TEXT;
            return CDRF_NEWFONT;
        }
        if (cd->iSubItem == COL_STATE) {
            LONG st = g_app.files[item].state;
            cd->clrText = state_color(st);
            return CDRF_NEWFONT;
        }
        if (cd->iSubItem == COL_SPEED || cd->iSubItem == COL_SIZE) {
            cd->clrText = C_TEXT2;
            return CDRF_NEWFONT;
        }
        if (cd->iSubItem == COL_NOTE) {
            cd->clrText = C_TEXT2;
            return CDRF_NEWFONT;
        }
        cd->clrText = C_TEXT;
        return CDRF_NEWFONT;
    }
    }
    return CDRF_DODEFAULT;
}

static LRESULT header_custom_draw(LPNMCUSTOMDRAW cd)
{
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        HDC hdc = cd->hdc;
        RECT rc = cd->rc;
        int col = (int)cd->dwItemSpec;
        wchar_t text[128] = L"";
        HDITEMW hi;
        RECT tr;

        ZeroMemory(&hi, sizeof(hi));
        hi.mask = HDI_TEXT | HDI_FORMAT;
        hi.pszText = text;
        hi.cchTextMax = 128;
        SendMessageW(cd->hdr.hwndFrom, HDM_GETITEMW, (WPARAM)col, (LPARAM)&hi);

        fill_rect(hdc, &rc, RGB(248, 250, 252));
        {
            RECT line = rc;
            line.top = line.bottom - 1;
            fill_rect(hdc, &line, C_BORDER);
        }
        tr = rc;
        tr.left += S(10);
        tr.right -= S(8);
        if (hi.fmt & HDF_RIGHT) {
            draw_text(hdc, text, &tr, g_fBold, C_TEXT2, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else {
            draw_text(hdc, text, &tr, g_fBold, C_TEXT2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

/* ------------------------------------------------------------------ */
/* 占用对话框                                                          */
/* ------------------------------------------------------------------ */
static void dlg_refresh_list(void)
{
    int i, k;
    SendMessageW(g_hDlgList, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_lockCount; ++i) {
        LockInfo *li = &g_lockList[i];
        wchar_t line[600];
        if (!li->locked) continue;
        if (li->appCount > 0) {
            _snwprintf(line, 600, L"%s    ←    %s (PID %lu)", li->rel, li->apps[0], li->pids[0]);
        } else {
            _snwprintf(line, 600, L"%s    ←    未知进程（可能是系统占用）", li->rel);
        }
        line[599] = 0;
        SendMessageW(g_hDlgList, LB_ADDSTRING, 0, (LPARAM)line);
        for (k = 1; k < li->appCount; ++k) {
            _snwprintf(line, 600, L"        ←    %s (PID %lu)", li->apps[k], li->pids[k]);
            line[599] = 0;
            SendMessageW(g_hDlgList, LB_ADDSTRING, 0, (LPARAM)line);
        }
    }
}

static void dlg_finish(int decision)
{
    if (g_dlgResultGiven) return;
    g_dlgResultGiven = 1;
    update_lock_decision(decision);
    if (g_hDlg) DestroyWindow(g_hDlg);
}

static void dlg_do_kill(void)
{
    int i, remaining = 0, killed;

    g_lockAttempts++;
    killed = lock_kill_infos(g_lockList, g_lockCount);
    for (i = 0; i < g_lockCount; ++i) {
        if (!g_lockList[i].locked) continue;
        if (lock_is_file_locked(g_lockList[i].path)) remaining++;
        else g_lockList[i].locked = 0;
    }

    dlg_refresh_list();

    if (remaining == 0) {
        lstrcpynW(g_dlgStatus, L"已成功结束占用进程，继续更新...", 512);
        InvalidateRect(g_hDlg, NULL, TRUE);
        Sleep(200);
        dlg_finish(0);
        return;
    }

    _snwprintf(g_dlgStatus, 512,
               L"自动结束失败：仍有 %d 个文件被占用（结束进程 %d 个）。请手动结束相关进程，或重启电脑后重试。",
               remaining, killed);
    g_dlgStatus[511] = 0;
    InvalidateRect(g_hDlg, NULL, TRUE);

    MessageBoxW(g_hDlg,
                L"自动结束进程失败，文件仍被占用。\n\n请手动结束相关进程后点击“自动结束进程并重试”，\n"
                L"或重启电脑后重新运行本更新程序。",
                L"需要手动处理", MB_OK | MB_ICONWARNING);
}

static void dlg_layout(HWND hwnd)
{
    RECT rc;
    int pad = S(18), btnH = S(34), y, i;
    int w1 = S(180), w2 = S(100), w3 = S(100), w4 = S(120);

    GetClientRect(hwnd, &rc);

    if (g_hDlgList) {
        MoveWindow(g_hDlgList, pad, S(84), rc.right - pad * 2, rc.bottom - S(84) - S(150), TRUE);
    }
    y = rc.bottom - pad - btnH;
    i = 0;
    if (g_hDlgBtn[DLG_KILL])    MoveWindow(g_hDlgBtn[DLG_KILL], pad, y, w1, btnH, TRUE);
    if (g_hDlgBtn[DLG_SKIP])    MoveWindow(g_hDlgBtn[DLG_SKIP], pad + w1 + S(10), y, w2, btnH, TRUE);
    if (g_hDlgBtn[DLG_CANCEL])  MoveWindow(g_hDlgBtn[DLG_CANCEL], pad + w1 + w2 + S(20), y, w3, btnH, TRUE);
    if (g_hDlgBtn[DLG_TASKMGR]) MoveWindow(g_hDlgBtn[DLG_TASKMGR], rc.right - pad - w4, y, w4, btnH, TRUE);
    (void)i;
}

static LRESULT CALLBACK LockDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        RECT rc, br;
        int pad = S(18), btnH = S(34);

        GetClientRect(hwnd, &rc);

        g_hDlgList = CreateWindowExW(0, L"LISTBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
                                         LBS_NOINTEGRALHEIGHT | LBS_DISABLENOSCROLL,
                                     pad, S(84), rc.right - pad * 2, rc.bottom - S(234),
                                     hwnd, (HMENU)IDC_DLG_LIST, g_hInst, NULL);
        SendMessageW(g_hDlgList, WM_SETFONT, (WPARAM)g_fUI, TRUE);

        br.left = pad; br.top = 0; br.right = pad + S(180); br.bottom = btnH;
        g_hDlgBtn[DLG_KILL] = btn_create(hwnd, IDC_DLG_KILL, &g_dlgBtn[DLG_KILL],
                                         L"自动结束进程并重试", 1, C_CARD, br, g_fBtn);
        br.left = pad + S(190); br.right = br.left + S(100);
        g_hDlgBtn[DLG_SKIP] = btn_create(hwnd, IDC_DLG_SKIP, &g_dlgBtn[DLG_SKIP],
                                         L"跳过并继续", 0, C_CARD, br, g_fBtn);
        br.left = pad + S(300); br.right = br.left + S(100);
        g_hDlgBtn[DLG_CANCEL] = btn_create(hwnd, IDC_DLG_CANCEL, &g_dlgBtn[DLG_CANCEL],
                                           L"取消更新", 0, C_CARD, br, g_fBtn);
        br.left = 0; br.right = S(120);
        g_hDlgBtn[DLG_TASKMGR] = btn_create(hwnd, IDC_DLG_TASKMGR, &g_dlgBtn[DLG_TASKMGR],
                                            L"打开任务管理器", 0, C_CARD, br, g_fBtn);

        dlg_refresh_list();
        dlg_layout(hwnd);
        return 0;
    }

    case WM_SIZE:
        dlg_layout(hwnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc, r;
        GetClientRect(hwnd, &rc);
        fill_rect(hdc, &rc, C_CARD);

        r.left = S(18); r.top = S(16); r.right = rc.right - S(18); r.bottom = r.top + S(26);
        draw_text(hdc, L"更新前检测到文件被占用", &r, g_fBig, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        r.top = S(46); r.bottom = r.top + S(20);
        draw_text(hdc, L"以下文件正在被其它程序使用，需要先结束这些进程才能完成更新：",
                  &r, g_fUI, C_TEXT2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        r.left = S(18); r.right = rc.right - S(18);
        r.top = rc.bottom - S(140); r.bottom = r.top + S(40);
        if (g_dlgStatus[0]) {
            draw_text(hdc, g_dlgStatus, &r, g_fBold, C_ORANGE,
                      DT_LEFT | DT_TOP | DT_WORDBREAK);
        } else {
            draw_text(hdc, L"提示：自动结束失败时，可手动结束进程，或重启电脑后再运行更新程序。",
                      &r, g_fUI, C_TEXT2, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        int idx;
        if (dis->CtlType != ODT_BUTTON) break;
        idx = btn_index_from_hwnd(dis->hwndItem, g_hDlgBtn, DLG_BTN_COUNT);
        if (idx < 0) break;
        btn_paint(dis, &g_dlgBtn[idx]);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_DLG_KILL) { dlg_do_kill(); return 0; }
        if (id == IDC_DLG_SKIP) { lstrcpynW(g_dlgStatus, L"已跳过被占用文件", 512); dlg_finish(1); return 0; }
        if (id == IDC_DLG_CANCEL) { dlg_finish(2); return 0; }
        if (id == IDC_DLG_TASKMGR) { lock_open_task_manager(); return 0; }
        return 0;
    }

    case WM_CLOSE:
        dlg_finish(2);
        return 0;

    case WM_DESTROY:
        if (!g_dlgResultGiven) {
            g_dlgResultGiven = 1;
            update_lock_decision(2);
        }
        g_hDlg = NULL;
        if (g_hMain) PostMessageW(g_hMain, WM_NULL, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_lock_dialog(LockInfo *list, int count)
{
    RECT rc, r;
    int w = S(620), h = S(430);
    MSG msg;

    g_lockList = list;
    g_lockCount = count;
    g_lockAttempts = 0;
    g_dlgStatus[0] = 0;
    g_dlgResultGiven = 0;

    GetWindowRect(g_hMain, &rc);
    r.left = rc.left + ((rc.right - rc.left) - w) / 2;
    r.top = rc.top + ((rc.bottom - rc.top) - h) / 2;

    EnableWindow(g_hMain, FALSE);
    g_hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"UpdataLockWnd", L"文件占用提示",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             r.left, r.top, w, h, g_hMain, NULL, g_hInst, NULL);
    if (!g_hDlg) {
        EnableWindow(g_hMain, TRUE);
        update_lock_decision(2);
        return;
    }
    SetForegroundWindow(g_hDlg);

    while (IsWindow(g_hDlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hMain, TRUE);
    SetForegroundWindow(g_hMain);
    if (!g_dlgResultGiven) {
        g_dlgResultGiven = 1;
        update_lock_decision(2);
    }
}

/* ------------------------------------------------------------------ */
/* 软件信息详情窗口（点击头部描述 / 更新日志 / “查看详情”打开）           */
/* ------------------------------------------------------------------ */
static HWND     g_hDetail;
static HWND     g_hDetailDesc, g_hDetailLog, g_hDetailClose;
static BtnState g_detailBtnSt;

#define DETAIL_EDIT1_Y  S(96)
#define DETAIL_EDIT1_H  S(112)
#define DETAIL_LABEL2_Y (DETAIL_EDIT1_Y + DETAIL_EDIT1_H + S(16))
#define DETAIL_EDIT2_Y  (DETAIL_LABEL2_Y + S(22))

static void detail_layout(HWND hwnd)
{
    RECT rc;
    int pad = S(20), btnH = S(34), btnW = S(100);
    int editW, editH2;

    GetClientRect(hwnd, &rc);
    editW = rc.right - pad * 2;
    editH2 = rc.bottom - DETAIL_EDIT2_Y - S(66);
    if (editH2 < S(60)) editH2 = S(60);

    if (g_hDetailDesc) MoveWindow(g_hDetailDesc, pad, DETAIL_EDIT1_Y, editW, DETAIL_EDIT1_H, TRUE);
    if (g_hDetailLog) MoveWindow(g_hDetailLog, pad, DETAIL_EDIT2_Y, editW, editH2, TRUE);
    if (g_hDetailClose) {
        MoveWindow(g_hDetailClose, rc.right - pad - btnW, rc.bottom - S(18) - btnH, btnW, btnH, TRUE);
    }
}

static LRESULT CALLBACK DetailProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        RECT br, rc;
        GetClientRect(hwnd, &rc);

        g_hDetailDesc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_app.desc,
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                                            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                        0, 0, 100, 100, hwnd, (HMENU)IDC_DETAIL_DESC, g_hInst, NULL);
        g_hDetailLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_app.changelog,
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                                           ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                       0, 0, 100, 100, hwnd, (HMENU)IDC_DETAIL_LOG, g_hInst, NULL);
        SendMessageW(g_hDetailDesc, WM_SETFONT, (WPARAM)g_fUI, TRUE);
        SendMessageW(g_hDetailLog, WM_SETFONT, (WPARAM)g_fUI, TRUE);

        ZeroMemory(&br, sizeof(br));
        g_hDetailClose = btn_create(hwnd, IDC_DETAIL_CLOSE, &g_detailBtnSt, L"关闭", 1, C_CARD, br, g_fBtn);

        detail_layout(hwnd);
        return 0;
    }

    case WM_SIZE:
        detail_layout(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, C_TEXT);
        SetBkColor(hdc, C_CARD);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc, r;
        wchar_t buf[512];

        GetClientRect(hwnd, &rc);
        fill_rect(hdc, &rc, C_CARD);

        r.left = S(20); r.top = S(14); r.right = rc.right - S(20); r.bottom = r.top + S(28);
        draw_text(hdc, L"软件信息", &r, g_fBig, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        _snwprintf(buf, 512, L"名称：%s        版本：%s", g_app.name[0] ? g_app.name : L"软件更新",
                   g_app.version[0] ? g_app.version : L"-");
        buf[511] = 0;
        r.left = S(20); r.top = S(46); r.right = rc.right - S(20); r.bottom = r.top + S(20);
        draw_text(hdc, buf, &r, g_fUI, C_TEXT2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        r.left = S(20); r.top = DETAIL_EDIT1_Y - S(22); r.right = rc.right - S(20); r.bottom = r.top + S(20);
        draw_text(hdc, L"软件描述", &r, g_fBold, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        r.left = S(20); r.top = DETAIL_LABEL2_Y; r.right = rc.right - S(20); r.bottom = r.top + S(20);
        draw_text(hdc, L"更新日志", &r, g_fBold, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlType != ODT_BUTTON || dis->hwndItem != g_hDetailClose) break;
        btn_paint(dis, &g_detailBtnSt);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_DETAIL_CLOSE) { DestroyWindow(hwnd); return 0; }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_hDetail = NULL;
        g_hDetailDesc = g_hDetailLog = g_hDetailClose = NULL;
        if (g_hMain) PostMessageW(g_hMain, WM_NULL, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_detail_dialog(void)
{
    RECT rc, r;
    int w = S(680), h = S(560);
    MSG msg;

    GetWindowRect(g_hMain, &rc);
    r.left = rc.left + ((rc.right - rc.left) - w) / 2;
    r.top = rc.top + ((rc.bottom - rc.top) - h) / 2;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;

    EnableWindow(g_hMain, FALSE);
    g_hDetail = CreateWindowExW(WS_EX_DLGMODALFRAME, L"UpdataDetailWnd", L"软件信息",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                r.left, r.top, w, h, g_hMain, NULL, g_hInst, NULL);
    if (!g_hDetail) {
        EnableWindow(g_hMain, TRUE);
        return;
    }
    SetForegroundWindow(g_hDetail);

    while (IsWindow(g_hDetail) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hDetail, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hMain, TRUE);
    SetForegroundWindow(g_hMain);
}

/* ------------------------------------------------------------------ */
/* 统计刷新                                                            */
/* ------------------------------------------------------------------ */
static void refresh_stats(void)
{
    DWORD now = GetTickCount();
    DWORD dt = now - g_lastTick;
    unsigned long long done = 0;
    int i;

    if (dt == 0) dt = 1;

    EnterCriticalSection(&g_statsLock);
    for (i = 0; i < g_app.fileCount; ++i) {
        FileTask *t = &g_app.files[i];
        LONG st = t->state;
        if (!t->needUpdate) continue;
        if (st == FS_DONE) {
            done += t->size;
        } else if (st == FS_FAIL || st == FS_LOCKED) {
            done += t->size;
        } else {
            done += t->got;
        }
        t->speed = (unsigned long long)((t->got - t->lastGot) * 1000 / dt);
        t->lastGot = t->got;
    }
    LeaveCriticalSection(&g_statsLock);

    g_bytesDone = done;
    if (done >= g_lastBytes) g_speed = (done - g_lastBytes) * 1000 / dt;
    else g_speed = 0;
    g_lastBytes = done;
    g_lastTick = now;
}

static void refresh_rows(void)
{
    int i;
    wchar_t buf[64], sp[64];
    unsigned long long got;

    if (!g_hList || !g_app.files) return;

    for (i = 0; i < g_app.fileCount; ++i) {
        FileTask *t = &g_app.files[i];
        LONG st = t->state;
        int pct = 0;

        EnterCriticalSection(&g_statsLock);
        got = t->got;
        LeaveCriticalSection(&g_statsLock);

        if (t->needUpdate && t->size > 0) {
            pct = (int)(got * 100 / t->size);
            if (pct > 100) pct = 100;
        }
        if (st == FS_DONE) pct = 100;

        if (g_lastState[i] == st && g_lastPercent[i] == pct) {
            /* 速度列仍可能需要刷新 */
            if (st == FS_DOWNLOAD) {
                util_fmt_speed(t->speed, sp, 64);
                lv_set_text(g_hList, i, COL_SPEED, sp);
            }
            continue;
        }
        g_lastState[i] = st;
        g_lastPercent[i] = pct;

        lv_set_text(g_hList, i, COL_STATE, state_text(st));
        if (st == FS_DOWNLOAD || st == FS_VERIFY) {
            util_fmt_speed(t->speed, sp, 64);
            lv_set_text(g_hList, i, COL_SPEED, sp);
            _snwprintf(buf, 64, L"%d%%", pct);
            lv_set_text(g_hList, i, COL_PROGRESS, buf);
        } else if (st == FS_DONE) {
            lv_set_text(g_hList, i, COL_SPEED, L"-");
            lv_set_text(g_hList, i, COL_PROGRESS, L"100%");
            lv_set_text(g_hList, i, COL_NOTE, L"更新成功");
        } else if (st == FS_FAIL) {
            lv_set_text(g_hList, i, COL_SPEED, L"-");
            lv_set_text(g_hList, i, COL_NOTE, t->errW[0] ? t->errW : L"更新失败");
        } else if (st == FS_LOCKED) {
            lv_set_text(g_hList, i, COL_SPEED, L"-");
            lv_set_text(g_hList, i, COL_NOTE, t->errW[0] ? t->errW : L"文件被占用，已跳过");
        } else if (st == FS_CANCEL) {
            lv_set_text(g_hList, i, COL_SPEED, L"-");
            lv_set_text(g_hList, i, COL_NOTE, L"已取消");
        }
    }
    InvalidateRect(g_hList, NULL, FALSE);
}

static void set_status(const wchar_t *text)
{
    lstrcpynW(g_app.status, text, 512);
    if (g_hMain) {
        RECT r = g_rcStatus;
        InvalidateRect(g_hMain, &r, FALSE);
    }
}

static void update_ui_state(void)
{
    int running = (g_app.running != 0);
    int hasUpdate = g_app.manifestReady && g_app.updateCount > 0;

    btn_enable(g_hBtn[BTN_START], &g_btn[BTN_START], !running && hasUpdate);
    btn_enable(g_hBtn[BTN_STOP], &g_btn[BTN_STOP], running);
    btn_enable(g_hBtn[BTN_RETRY], &g_btn[BTN_RETRY], !running);
}

/* ------------------------------------------------------------------ */
/* 主窗口                                                              */
/* ------------------------------------------------------------------ */
static void on_fetch_done(void)
{
    int i, need = 0;
    wchar_t buf[512];

    g_app.doneCount = 0;
    g_app.failCount = 0;
    g_app.skipCount = 0;

    if (g_lastPercent) free(g_lastPercent);
    if (g_lastState) free(g_lastState);
    g_lastPercent = (int *)calloc((size_t)(g_app.fileCount + 1), sizeof(int));
    g_lastState = (LONG *)calloc((size_t)(g_app.fileCount + 1), sizeof(LONG));
    for (i = 0; i < g_app.fileCount; ++i) {
        g_lastState[i] = -1;
        if (g_app.files[i].needUpdate) need++;
    }

    list_rebuild();
    update_ui_state();
    compute_layout(g_hMain);
    InvalidateRect(g_hMain, NULL, TRUE);

    if (need == 0) {
        _snwprintf(buf, 512, L"当前已是最新版本（%s），无需更新。", g_app.version[0] ? g_app.version : L"");
        set_status(buf);
        util_log(L"[检测更新] 无需更新");
    } else {
        _snwprintf(buf, 512, L"发现 %d 个文件需要更新，共 %d 个文件，更新即将开始...", need, g_app.fileCount);
        set_status(buf);
        update_start_download(g_hMain);
    }
}

static void on_update_done(void)
{
    wchar_t buf[512];
    int launched = 0;
    DWORD elapsed = g_app.endTick - g_app.startTick;

    refresh_stats();
    refresh_rows();

    if (g_app.cancelled) {
        set_status(L"更新已取消。");
        update_ui_state();
        return;
    }

    if (g_app.failCount == 0 && g_app.skipCount == 0) {
        launched = update_launch_autorun();
        if (launched > 0) {
            _snwprintf(buf, 512, L"更新完成，已启动 %d 个程序，窗口将在 3 秒后自动关闭。", launched);
        } else {
            _snwprintf(buf, 512, L"更新完成，共更新 %d 个文件（耗时 %lu 秒）。",
                       g_app.doneCount, (unsigned long)(elapsed / 1000));
        }
        set_status(buf);
        util_log(L"[完成] %s", buf);
        g_closeCountdown = 3;
        SetTimer(g_hMain, TIMER_CLOSE, 1000, NULL);
    } else {
        _snwprintf(buf, 512, L"更新结束：成功 %d 个，失败 %d 个，跳过 %d 个。可点击“开始更新”重试。",
                   g_app.doneCount, g_app.failCount, g_app.skipCount);
        set_status(buf);
    }
    update_ui_state();
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HWND hHeader;
        int i;
        RECT rc;

        GetClientRect(hwnd, &rc);

        g_hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                      LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                                  0, 0, 100, 100, hwnd, (HMENU)IDC_LIST, g_hInst, NULL);
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(g_hList, C_CARD);
        ListView_SetTextBkColor(g_hList, C_CARD);
        ListView_SetTextColor(g_hList, C_TEXT);
        SendMessageW(g_hList, WM_SETFONT, (WPARAM)g_fUI, TRUE);
        {
            struct { const wchar_t *t; int w; int fmt; } cols[COL_MAX] = {
                { L"文件名", 170, LVCFMT_LEFT },
                { L"目录", 110, LVCFMT_LEFT },
                { L"大小", 90, LVCFMT_RIGHT },
                { L"进度", 150, LVCFMT_LEFT },
                { L"速度", 100, LVCFMT_RIGHT },
                { L"状态", 80, LVCFMT_LEFT },
                { L"说明", 200, LVCFMT_LEFT }
            };
            for (i = 0; i < COL_MAX; ++i) {
                LVCOLUMNW col;
                ZeroMemory(&col, sizeof(col));
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
                col.pszText = (LPWSTR)cols[i].t;
                col.cx = S(cols[i].w);
                col.fmt = cols[i].fmt;
                col.iSubItem = i;
                ListView_InsertColumn(g_hList, i, &col);
            }
        }
        hHeader = ListView_GetHeader(g_hList);
        if (hHeader) SendMessageW(hHeader, WM_SETFONT, (WPARAM)g_fBold, TRUE);

        /* 按钮 */
        {
            RECT br;
            ZeroMemory(&br, sizeof(br));
            g_hBtn[BTN_START] = btn_create(hwnd, IDC_BTN_START, &g_btn[BTN_START], L"开始更新", 1, C_BG, br, g_fBtn);
            g_hBtn[BTN_STOP]  = btn_create(hwnd, IDC_BTN_STOP, &g_btn[BTN_STOP], L"停止", 0, C_BG, br, g_fBtn);
            g_hBtn[BTN_RETRY] = btn_create(hwnd, IDC_BTN_RETRY, &g_btn[BTN_RETRY], L"重新检测", 0, C_BG, br, g_fBtn);
            g_hBtn[BTN_CLOSE] = btn_create(hwnd, IDC_BTN_CLOSE, &g_btn[BTN_CLOSE], L"关闭", 0, C_BG, br, g_fBtn);
        }

        set_status(L"正在获取更新信息...");
        SetTimer(hwnd, TIMER_REFRESH, 250, NULL);
        g_lastTick = GetTickCount();
        g_lastBytes = 0;
        update_ui_state();
        update_start_fetch(hwnd);
        return 0;
    }

    case WM_SIZE:
        compute_layout(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = S(760);
        mmi->ptMinTrackSize.y = S(560);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        POINT pt;
        int hit;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        hit = g_app.manifestReady && (PtInRect(&g_rcDetailBtn, pt) || PtInRect(&g_rcDesc, pt) ||
                                      PtInRect(&g_rcLog, pt));
        if (hit != g_detailHover) {
            g_detailHover = hit;
            InvalidateRect(hwnd, &g_rcHeader, FALSE);
        }
        if (!g_mouseTracked) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            if (TrackMouseEvent(&tme)) g_mouseTracked = 1;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_mouseTracked = 0;
        if (g_detailHover) {
            g_detailHover = 0;
            InvalidateRect(hwnd, &g_rcHeader, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (g_app.manifestReady && (PtInRect(&g_rcDetailBtn, pt) || PtInRect(&g_rcDesc, pt) ||
                                        PtInRect(&g_rcLog, pt))) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDOWN: {
        POINT pt;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        if (g_app.manifestReady && (PtInRect(&g_rcDetailBtn, pt) || PtInRect(&g_rcDesc, pt) ||
                                    PtInRect(&g_rcLog, pt))) {
            show_detail_dialog();
            return 0;
        }
        break;
    }

    case WM_PAINT:
        paint_main(hwnd);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_REFRESH) {
            refresh_stats();
            refresh_rows();
            if (g_app.running) {
                wchar_t buf[512];
                int pct = g_app.bytesTotal > 0 ? (int)(g_bytesDone * 100 / g_app.bytesTotal) : 0;
                _snwprintf(buf, 512, L"正在下载更新文件... 已完成 %d%%", pct > 100 ? 100 : pct);
                lstrcpynW(g_app.status, buf, 512);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wp == TIMER_CLOSE) {
            wchar_t buf[512];
            if (g_closeCountdown > 0) g_closeCountdown--;
            if (g_closeCountdown <= 0) {
                KillTimer(hwnd, TIMER_CLOSE);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            } else {
                _snwprintf(buf, 512, L"更新完成，已启动 %d 个程序，窗口将在 %d 秒后自动关闭。",
                           g_app.launchedCount, g_closeCountdown);
                set_status(buf);
            }
        }
        return 0;

    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)lp;
        if (hdr->hwndFrom == g_hList && hdr->code == NM_CUSTOMDRAW) {
            return list_custom_draw((LPNMLVCUSTOMDRAW)lp);
        }
        if (hdr->hwndFrom == ListView_GetHeader(g_hList) && hdr->code == NM_CUSTOMDRAW) {
            return header_custom_draw((LPNMCUSTOMDRAW)lp);
        }
        break;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        int idx;
        if (dis->CtlType != ODT_BUTTON) break;
        idx = btn_index_from_hwnd(dis->hwndItem, g_hBtn, BTN_COUNT);
        if (idx < 0) break;
        btn_paint(dis, &g_btn[idx]);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        /* 更新进行中只允许停止/关闭，避免误操作破坏正在下载的任务 */
        if (g_app.running && id != IDC_BTN_STOP && id != IDC_BTN_CLOSE) return 0;
        if (id == IDC_BTN_START) {
            g_app.cancelled = 0;
            g_app.doneCount = 0;
            g_app.failCount = 0;
            g_app.skipCount = 0;
            g_app.finished = 0;
            g_app.endTick = 0;
            g_bytesDone = 0;
            g_lastBytes = 0;
            g_speed = 0;
            {
                int i;
                for (i = 0; i < g_app.fileCount; ++i) {
                    if (g_app.files[i].needUpdate) {
                        g_app.files[i].skipTask = 0;
                        InterlockedExchange(&g_app.files[i].state, FS_WAIT);
                    }
                    if (g_lastState) g_lastState[i] = -1;
                }
            }
            set_status(L"正在检测文件占用...");
            update_start_download(hwnd);
            update_ui_state();
            return 0;
        }
        if (id == IDC_BTN_STOP) {
            update_stop();
            set_status(L"正在停止下载...");
            return 0;
        }
        if (id == IDC_BTN_RETRY) {
            set_status(L"正在获取更新信息...");
            if (g_lastState && g_app.fileCount) {
                int i;
                for (i = 0; i < g_app.fileCount; ++i) g_lastState[i] = -1;
            }
            update_start_fetch(hwnd);
            return 0;
        }
        if (id == IDC_BTN_CLOSE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        return 0;
    }

    case WM_UPD_STATUS: {
        wchar_t *text = (wchar_t *)lp;
        if (text) {
            set_status(text);
            free(text);
        }
        return 0;
    }

    case WM_UPD_FETCH_DONE:
        g_lastTick = GetTickCount();
        g_lastBytes = 0;
        g_speed = 0;
        g_bytesDone = 0;
        on_fetch_done();
        return 0;

    case WM_UPD_FETCH_FAIL: {
        wchar_t *text = (wchar_t *)lp;
        wchar_t buf[600];
        _snwprintf(buf, 600, L"获取更新信息失败：%s", text ? text : L"未知错误");
        buf[599] = 0;
        set_status(buf);
        util_log(L"[错误] %s", buf);
        MessageBoxW(hwnd, buf, L"更新失败", MB_OK | MB_ICONERROR);
        if (text) free(text);
        update_ui_state();
        return 0;
    }

    case WM_UPD_LOCK_FOUND: {
        LockInfo *list = (LockInfo *)lp;
        int count = (int)wp;
        if (list) show_lock_dialog(list, count);
        else update_lock_decision(0);
        if (list) free(list);
        return 0;
    }

    case WM_UPD_UPDATE_DONE:
        on_update_done();
        return 0;

    case WM_CLOSE: {
        if (g_app.running) {
            if (MessageBoxW(hwnd, L"更新正在进行中，确定要退出吗？", L"确认退出",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            update_stop();
            Sleep(1200);
        }
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        KillTimer(hwnd, TIMER_CLOSE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */
static void quote_arg(const wchar_t *src, wchar_t *dst, int cap)
{
    int needQuote = (wcschr(src, L' ') != NULL || wcschr(src, L'\t') != NULL);
    if (needQuote) _snwprintf(dst, (size_t)cap, L"\"%s\"", src);
    else lstrcpynW(dst, src, cap);
    dst[cap - 1] = 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    HWND hwnd;
    MSG msg;
    HDC hdc;
    int argc = 0, i;
    LPWSTR *argv;

    (void)hPrev;
    (void)lpCmdLine;

    g_hInst = hInst;
    update_init();

    /* 命令行参数：1=更新检测地址，2..=附加命令行 */
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2) {
        /* 未提供更新地址：不弹提示，直接退出（返回码 2） */
        util_log(L"[启动] 未提供更新检测地址，程序退出");
        if (argv) LocalFree(argv);
        return 2;
    }
    lstrcpynW(g_app.baseUrl, argv[1], 2048);
    {
        int off = 0;
        for (i = 2; i < argc; ++i) {
            wchar_t q[1024];
            quote_arg(argv[i], q, 1024);
            if (off > 0 && off < 1020) {
                g_app.extraArgs[off++] = L' ';
                g_app.extraArgs[off] = 0;
            }
            lstrcpynW(g_app.extraArgs + off, q, 1024 - off);
            off = (int)wcslen(g_app.extraArgs);
        }
    }
    LocalFree(argv);

    hdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);
    if (g_dpi <= 0) g_dpi = 96;

    create_fonts();

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    /* 主窗口类 */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"UpdataMainWnd";
    wc.style = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassExW(&wc)) return 1;

    /* 占用对话框类 */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = LockDlgProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"UpdataLockWnd";
    RegisterClassExW(&wc);

    /* 软件信息详情窗口类 */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DetailProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"UpdataDetailWnd";
    RegisterClassExW(&wc);

    {
        RECT rc;
        int w = S(960), h = S(660);
        int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        hwnd = CreateWindowExW(0, L"UpdataMainWnd", L"软件更新程序",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               x, y, w, h, NULL, NULL, hInst, NULL);
        (void)rc;
    }
    if (!hwnd) return 1;

    g_hMain = hwnd;
    ShowWindow(hwnd, nCmdShow ? nCmdShow : SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    update_free_all();
    return 0;
}
