/*
 * 单文件分发包 —— 运行期启动器（Windows）
 *
 * 工作方式：
 *   1. 从自身文件尾部读取 footer，定位内嵌的 archive
 *   2. 首次运行把 archive 解压到 %LOCALAPPDATA%\ViewTools\<产品名>\<CRC32>\
 *      （CRC32 参与目录名，因此发布新版本自动换目录，不会串版本）
 *   3. 后续运行命中缓存直接启动，秒开；缓存不完整会自动重新解压
 *   4. 命令行参数、当前工作目录、退出码原样透传给真正的程序
 *
 * 不依赖 Qt 或任何第三方 DLL，只链接系统库 + zlib（静态）。
 * 界面文本统一使用 ASCII，避免不同编译器对源码编码的处理差异。
 */
#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <zlib.h>

#include "sfx_format.h"

#define SFX_IO_BUFFER (256u * 1024u)
#define SFX_PATH_MAX  4096u
#define SFX_CMD_MAX   8192u

#if defined(_MSC_VER)
#  define SFX_SNWPRINTF _snwprintf
#else
#  define SFX_SNWPRINTF swprintf
#endif

typedef struct {
    char     *rel;    /* UTF-8 相对路径，分隔符 '/' */
    uint64_t  size;
    int       is_dir;
} sfx_entry_t;

/* ---- 包信息 ---- */
static HANDLE    g_self = INVALID_HANDLE_VALUE;
static uint64_t  g_dataOffset = 0;
static uint64_t  g_dataSize = 0;
static uint64_t  g_rawSize = 0;
static uint32_t  g_dataCrc = 0;
static int       g_deflate = 0;

/* ---- 索引与解压状态 ---- */
static sfx_entry_t *g_entries = NULL;
static uint64_t     g_entryCount = 0;
static wchar_t      g_root[SFX_PATH_MAX];

static HANDLE    g_out = INVALID_HANDLE_VALUE;
static uint64_t  g_index = 0;
static uint64_t  g_remain = 0;
static uint32_t  g_crc = 0;
static uint64_t  g_written = 0;

/* ---- 进度窗口 ---- */
static HWND      g_win = NULL;
static HWND      g_bar = NULL;
static int       g_percent = -1;

static void notify(const wchar_t *text)
{
    MessageBoxW(NULL, text, L"ViewTools", MB_OK | MB_ICONERROR);
}

static int read_at(HANDLE f, uint64_t offset, void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(f, li, NULL, FILE_BEGIN)) return 0;
    if (!ReadFile(f, buf, len, &got, NULL)) return 0;
    return got == len;
}

static wchar_t *rel_to_wide(const char *rel)
{
    int n;
    wchar_t *w;
    wchar_t *p;
    if (!rel) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, rel, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, rel, -1, w, n);
    for (p = w; *p; ++p) if (*p == L'/') *p = L'\\';
    return w;
}

static int parse_index(const unsigned char *p, uint64_t size)
{
    uint64_t off = 0;
    uint64_t i = 0;

    g_entries = (sfx_entry_t *)calloc((size_t)(g_entryCount ? g_entryCount : 1), sizeof(sfx_entry_t));
    if (!g_entries) return 0;

    while (i < g_entryCount) {
        uint16_t nlen;
        uint64_t fsize;
        char *rel;

        if (off + SFX_IDX_FIXED_SIZE > size) return 0;
        nlen = sfx_get_u16(p + off + SFX_IDX_OFF_NAME_LEN);
        fsize = sfx_get_u64(p + off + SFX_IDX_OFF_SIZE);
        if (off + SFX_IDX_FIXED_SIZE + nlen > size) return 0;

        rel = (char *)malloc((size_t)nlen + 1);
        if (!rel) return 0;
        memcpy(rel, p + off + SFX_IDX_FIXED_SIZE, nlen);
        rel[nlen] = '\0';

        g_entries[i].rel = rel;
        g_entries[i].size = fsize;
        g_entries[i].is_dir = (p[off + SFX_IDX_OFF_TYPE] == SFX_ENTRY_DIR);
        off += SFX_IDX_FIXED_SIZE + nlen;
        ++i;
    }
    return off == size;
}

/* ---------------- 进度窗口（仅首次解压时显示） ---------------- */

static LRESULT CALLBACK sfx_wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) return 0;   /* 解压过程中不允许关闭 */
    return DefWindowProcW(h, m, w, l);
}

static void ui_pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void ui_show(void)
{
    INITCOMMONCONTROLSEX ic;
    WNDCLASSW wc;
    int w = 420, h = 130, x, y;

    ic.dwSize = sizeof(ic);
    ic.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&ic);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = sfx_wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"VtSingleFileProgress";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    /* IDC_APPSTARTING 的宽字符版本（32650 = IDC_APPSTARTING） */
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32650));
    RegisterClassW(&wc);

    x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_win = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                            L"VtSingleFileProgress", L"ViewTools",
                            WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                            x, y, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_win) return;

    CreateWindowExW(0, L"STATIC", L"Preparing runtime environment, please wait...",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    20, 22, w - 40, 20, g_win, NULL, NULL, NULL);
    g_bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
                            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                            20, 55, w - 40, 22, g_win, NULL, NULL, NULL);
    if (g_bar) SendMessageW(g_bar, PBM_SETRANGE32, 0, 100);
    ui_pump();
}

static void ui_progress(uint64_t done, uint64_t total)
{
    int pct;
    if (!total) return;
    pct = (int)(done * 100 / total);
    if (pct > 100) pct = 100;
    if (pct == g_percent) return;
    g_percent = pct;
    if (g_bar) SendMessageW(g_bar, PBM_SETPOS, (WPARAM)pct, 0);
    ui_pump();
}

static void ui_done(void)
{
    if (g_win) { DestroyWindow(g_win); g_win = NULL; g_bar = NULL; }
    ui_pump();
}

/* ---------------- 目录工具 ---------------- */

static void remove_tree(const wchar_t *path)
{
    wchar_t pattern[SFX_PATH_MAX];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    SFX_SNWPRINTF(pattern, SFX_PATH_MAX, L"%s\\*", path);
    pattern[SFX_PATH_MAX - 1] = L'\0';

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryW(path); return; }

    do {
        wchar_t child[SFX_PATH_MAX];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        SFX_SNWPRINTF(child, SFX_PATH_MAX, L"%s\\%s", path, fd.cFileName);
        child[SFX_PATH_MAX - 1] = L'\0';
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
        else DeleteFileW(child);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    RemoveDirectoryW(path);
}

static int make_dir(const wchar_t *path)
{
    wchar_t buf[SFX_PATH_MAX];
    wchar_t *slash;
    DWORD attrs;

    attrs = GetFileAttributesW(path);
    if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

    SFX_SNWPRINTF(buf, SFX_PATH_MAX, L"%s", path);
    buf[SFX_PATH_MAX - 1] = L'\0';
    slash = wcsrchr(buf, L'\\');
    if (!slash || slash == buf) return 0;
    *slash = L'\0';
    if (!make_dir(buf)) return 0;

    return CreateDirectoryW(path, NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int make_parent_dir(const wchar_t *file)
{
    wchar_t buf[SFX_PATH_MAX];
    wchar_t *slash;
    SFX_SNWPRINTF(buf, SFX_PATH_MAX, L"%s", file);
    buf[SFX_PATH_MAX - 1] = L'\0';
    slash = wcsrchr(buf, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    return make_dir(buf);
}

/* ---------------- 解压 ---------------- */

static int open_next_file(void)
{
    while (g_index < g_entryCount) {
        sfx_entry_t *e = &g_entries[g_index];
        wchar_t path[SFX_PATH_MAX];
        wchar_t *wrel;

        if (e->is_dir) { ++g_index; continue; }

        wrel = rel_to_wide(e->rel);
        if (!wrel) return 0;
        SFX_SNWPRINTF(path, SFX_PATH_MAX, L"%s\\%s", g_root, wrel);
        path[SFX_PATH_MAX - 1] = L'\0';
        free(wrel);

        if (!make_parent_dir(path)) return 0;
        g_out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_out == INVALID_HANDLE_VALUE) return 0;
        g_remain = e->size;
        return 1;
    }
    return 0;   /* 没有更多文件 */
}

static int emit(const unsigned char *buf, size_t len)
{
    while (len > 0) {
        DWORD chunk;
        DWORD done = 0;

        if (g_out == INVALID_HANDLE_VALUE) {
            if (!open_next_file()) return 0;
        }
        if (g_remain == 0) {          /* 空文件：建好就切下一个 */
            CloseHandle(g_out);
            g_out = INVALID_HANDLE_VALUE;
            ++g_index;
            continue;
        }
        chunk = (DWORD)((len < g_remain) ? len : g_remain);
        if (!WriteFile(g_out, buf, chunk, &done, NULL) || done != chunk) return 0;
        g_crc = (uint32_t)crc32(g_crc, buf, done);
        g_written += done;
        g_remain -= done;
        buf += done;
        len -= done;
    }
    return 1;
}

static int extract(void)
{
    unsigned char *in;
    unsigned char *out;
    z_stream strm;
    LARGE_INTEGER li;
    uint64_t left = g_dataSize;
    int ok = 1;
    int zret = Z_OK;
    int finished = 0;

    in = (unsigned char *)malloc(SFX_IO_BUFFER);
    out = (unsigned char *)malloc(SFX_IO_BUFFER);
    if (!in || !out) { free(in); free(out); return 0; }

    memset(&strm, 0, sizeof(strm));
    if (g_deflate && inflateInit(&strm) != Z_OK) { free(in); free(out); return 0; }

    li.QuadPart = (LONGLONG)g_dataOffset;
    if (!SetFilePointerEx(g_self, li, NULL, FILE_BEGIN)) ok = 0;

    while (ok && left > 0 && !finished) {
        DWORD want = (DWORD)((left > SFX_IO_BUFFER) ? SFX_IO_BUFFER : left);
        DWORD got = 0;
        if (!ReadFile(g_self, in, want, &got, NULL) || got == 0) { ok = 0; break; }
        left -= got;

        if (!g_deflate) {
            if (!emit(in, got)) ok = 0;
            ui_progress(g_written, g_rawSize);
            continue;
        }

        strm.next_in = in;
        strm.avail_in = got;
        while (strm.avail_in > 0) {
            DWORD produced;
            strm.next_out = out;
            strm.avail_out = SFX_IO_BUFFER;
            zret = inflate(&strm, Z_NO_FLUSH);
            if (zret != Z_OK && zret != Z_STREAM_END) { ok = 0; break; }
            produced = SFX_IO_BUFFER - strm.avail_out;
            if (produced && !emit(out, produced)) { ok = 0; break; }
            if (zret == Z_STREAM_END) { finished = 1; break; }
        }
        ui_progress(g_written, g_rawSize);
    }

    /* 收尾：把 zlib 流的尾部刷完 */
    if (ok && g_deflate && !finished) {
        strm.next_in = in;
        strm.avail_in = 0;
        while (zret != Z_STREAM_END) {
            DWORD produced;
            strm.next_out = out;
            strm.avail_out = SFX_IO_BUFFER;
            zret = inflate(&strm, Z_FINISH);
            if (zret != Z_OK && zret != Z_STREAM_END) { ok = 0; break; }
            produced = SFX_IO_BUFFER - strm.avail_out;
            if (produced && !emit(out, produced)) { ok = 0; break; }
        }
    }

    if (g_out != INVALID_HANDLE_VALUE) { CloseHandle(g_out); g_out = INVALID_HANDLE_VALUE; }
    if (g_deflate) inflateEnd(&strm);
    free(in);
    free(out);

    if (!ok) return 0;
    return g_written == g_rawSize && g_crc == g_dataCrc;
}

/* ---------------- 缓存目录 ---------------- */

static int build_cache_dir(wchar_t *outPath, size_t cap, uint32_t crc)
{
    wchar_t base[SFX_PATH_MAX];
    wchar_t profile[SFX_PATH_MAX];
    wchar_t self[SFX_PATH_MAX];
    wchar_t name[SFX_PATH_MAX];
    wchar_t *slash;
    wchar_t *dot;

    base[0] = L'\0';
    profile[0] = L'\0';

    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, SFX_PATH_MAX) || !base[0]) {
        if (GetEnvironmentVariableW(L"USERPROFILE", profile, SFX_PATH_MAX) && profile[0]) {
            SFX_SNWPRINTF(base, SFX_PATH_MAX, L"%s\\AppData\\Local", profile);
            base[SFX_PATH_MAX - 1] = L'\0';
        }
    }
    if (!base[0] && !GetTempPathW(SFX_PATH_MAX, base)) return 0;

    if (!GetModuleFileNameW(NULL, self, SFX_PATH_MAX)) return 0;
    slash = wcsrchr(self, L'\\');
    SFX_SNWPRINTF(name, SFX_PATH_MAX, L"%s", slash ? slash + 1 : self);
    name[SFX_PATH_MAX - 1] = L'\0';
    dot = wcsrchr(name, L'.');
    if (dot) *dot = L'\0';
    if (!name[0]) return 0;

    SFX_SNWPRINTF(outPath, cap, L"%s\\ViewTools\\%s\\%08x", base, name, crc);
    if (cap) outPath[cap - 1] = L'\0';
    return 1;
}

static int marker_valid(const wchar_t *cache, uint32_t crc)
{
    wchar_t path[SFX_PATH_MAX];
    char text[32];
    FILE *f;
    unsigned int v = 0;
    int hit = 0;

    SFX_SNWPRINTF(path, SFX_PATH_MAX, L"%s\\.sfx_ready", cache);
    path[SFX_PATH_MAX - 1] = L'\0';
    f = _wfopen(path, L"rb");
    if (!f) return 0;
    if (fscanf(f, "%31s", text) == 1 && sscanf(text, "%x", &v) == 1 && v == crc) hit = 1;
    fclose(f);
    return hit;
}

static int write_marker(const wchar_t *cache, uint32_t crc)
{
    wchar_t path[SFX_PATH_MAX];
    FILE *f;
    SFX_SNWPRINTF(path, SFX_PATH_MAX, L"%s\\.sfx_ready", cache);
    path[SFX_PATH_MAX - 1] = L'\0';
    f = _wfopen(path, L"wb");
    if (!f) return 0;
    fprintf(f, "%08x", crc);
    fclose(f);
    return 1;
}

/* 清理历史版本缓存：只清理本产品目录下、非当前 CRC 的子目录 */
static void cleanup_old_cache(const wchar_t *cache, uint32_t crc)
{
    wchar_t product[SFX_PATH_MAX];
    wchar_t current[16];
    wchar_t pattern[SFX_PATH_MAX];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t *slash;

    SFX_SNWPRINTF(product, SFX_PATH_MAX, L"%s", cache);
    product[SFX_PATH_MAX - 1] = L'\0';
    slash = wcsrchr(product, L'\\');
    if (!slash) return;
    *slash = L'\0';

    SFX_SNWPRINTF(current, 16, L"%08x", crc);
    current[15] = L'\0';

    SFX_SNWPRINTF(pattern, SFX_PATH_MAX, L"%s\\*", product);
    pattern[SFX_PATH_MAX - 1] = L'\0';

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t child[SFX_PATH_MAX];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (_wcsicmp(fd.cFileName, current) == 0) continue;
        SFX_SNWPRINTF(child, SFX_PATH_MAX, L"%s\\%s", product, fd.cFileName);
        child[SFX_PATH_MAX - 1] = L'\0';
        remove_tree(child);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---------------- 启动真正的程序 ---------------- */

static const wchar_t *skip_argv0(const wchar_t *cmd)
{
    if (!cmd) return L"";
    if (*cmd == L'"') {
        ++cmd;
        while (*cmd && *cmd != L'"') ++cmd;
        if (*cmd) ++cmd;
    } else {
        while (*cmd && *cmd != L' ' && *cmd != L'\t') ++cmd;
    }
    while (*cmd == L' ' || *cmd == L'\t') ++cmd;
    return cmd;
}

static int run_entry(const wchar_t *cache, const char *entryRel)
{
    wchar_t target[SFX_PATH_MAX];
    wchar_t cmd[SFX_CMD_MAX];
    wchar_t cwd[SFX_PATH_MAX];
    wchar_t *wrel;
    const wchar_t *args;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    wrel = rel_to_wide(entryRel);
    if (!wrel) { notify(L"Invalid package: bad entry name."); return 1; }
    SFX_SNWPRINTF(target, SFX_PATH_MAX, L"%s\\%s", cache, wrel);
    target[SFX_PATH_MAX - 1] = L'\0';
    free(wrel);

    if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) {
        notify(L"Failed to start: main program is missing.");
        return 1;
    }

    args = skip_argv0(GetCommandLineW());
    SFX_SNWPRINTF(cmd, SFX_CMD_MAX, L"\"%s\"%s%s", target, (*args ? L" " : L""), args);
    cmd[SFX_CMD_MAX - 1] = L'\0';

    if (!GetCurrentDirectoryW(SFX_PATH_MAX, cwd)) cwd[0] = L'\0';

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(target, cmd, NULL, NULL, TRUE, 0, NULL,
                        cwd[0] ? cwd : NULL, &si, &pi)) {
        notify(L"Failed to start the application.");
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

/* ---------------- 入口 ---------------- */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    wchar_t self[SFX_PATH_MAX];
    wchar_t cache[SFX_PATH_MAX];
    unsigned char foot[SFX_FOOTER_SIZE];
    unsigned char head[SFX_ARC_FIXED_SIZE];
    unsigned char *index = NULL;
    char entryRel[512];
    LARGE_INTEGER size;
    uint64_t total;
    uint64_t arcOffset;
    uint64_t arcSize;
    uint64_t indexSize;
    uint64_t dataOffset;
    uint32_t arcCrc;
    uint32_t entryLen;
    uint64_t i;

    (void)hInst; (void)hPrev; (void)lpCmdLine; (void)nCmdShow;

    if (!GetModuleFileNameW(NULL, self, SFX_PATH_MAX)) return 1;
    g_self = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_self == INVALID_HANDLE_VALUE) { notify(L"Cannot open the package file."); return 1; }

    if (!GetFileSizeEx(g_self, &size) || (uint64_t)size.QuadPart < SFX_FOOTER_SIZE) {
        CloseHandle(g_self);
        notify(L"Corrupted package file.");
        return 1;
    }
    total = (uint64_t)size.QuadPart;

    if (!read_at(g_self, total - SFX_FOOTER_SIZE, foot, SFX_FOOTER_SIZE) ||
        memcmp(foot, SFX_FOOTER_MAGIC, 8) != 0) {
        CloseHandle(g_self);
        notify(L"Incomplete package: payload not found.");
        return 1;
    }
    arcOffset = sfx_get_u64(foot + SFX_FOOT_OFF_ARC_OFFSET);
    arcSize   = sfx_get_u64(foot + SFX_FOOT_OFF_ARC_SIZE);
    arcCrc    = sfx_get_u32(foot + SFX_FOOT_OFF_ARC_CRC);
    if (sfx_get_u32(foot + SFX_FOOT_OFF_VERSION) != SFX_VERSION ||
        sfx_get_u64(foot + SFX_FOOT_OFF_TOTAL_SIZE) != total ||
        arcOffset + arcSize + SFX_FOOTER_SIZE != total) {
        CloseHandle(g_self);
        notify(L"Corrupted package: bad footer.");
        return 1;
    }

    if (!read_at(g_self, arcOffset, head, SFX_ARC_FIXED_SIZE) ||
        memcmp(head, SFX_ARCHIVE_MAGIC, 8) != 0) {
        CloseHandle(g_self);
        notify(L"Corrupted package: bad archive header.");
        return 1;
    }
    g_deflate    = (sfx_get_u32(head + SFX_ARC_OFF_FLAGS) & SFX_FLAG_DEFLATE) != 0;
    entryLen     = sfx_get_u32(head + SFX_ARC_OFF_ENTRY_LEN);
    g_entryCount = sfx_get_u64(head + SFX_ARC_OFF_COUNT);
    indexSize    = sfx_get_u64(head + SFX_ARC_OFF_INDEX_SIZE);
    g_dataSize   = sfx_get_u64(head + SFX_ARC_OFF_DATA_SIZE);
    g_rawSize    = sfx_get_u64(head + SFX_ARC_OFF_RAW_SIZE);
    g_dataCrc    = sfx_get_u32(head + SFX_ARC_OFF_CRC);

    if (entryLen == 0 || entryLen >= sizeof(entryRel) || g_entryCount == 0 ||
        indexSize == 0 || indexSize > (64u * 1024u * 1024u)) {
        CloseHandle(g_self);
        notify(L"Corrupted package: bad archive description.");
        return 1;
    }
    if (!read_at(g_self, arcOffset + SFX_ARC_FIXED_SIZE, entryRel, entryLen)) {
        CloseHandle(g_self);
        notify(L"Corrupted package: cannot read entry name.");
        return 1;
    }
    entryRel[entryLen] = '\0';

    dataOffset = arcOffset + SFX_ARC_FIXED_SIZE + entryLen + indexSize;
    if (dataOffset + g_dataSize + SFX_FOOTER_SIZE != total) {
        CloseHandle(g_self);
        notify(L"Corrupted package: size mismatch.");
        return 1;
    }
    g_dataOffset = dataOffset;

    index = (unsigned char *)malloc((size_t)indexSize);
    if (!index) { CloseHandle(g_self); notify(L"Out of memory."); return 1; }
    if (!read_at(g_self, arcOffset + SFX_ARC_FIXED_SIZE + entryLen, index, (DWORD)indexSize) ||
        !parse_index(index, indexSize)) {
        free(index);
        CloseHandle(g_self);
        notify(L"Corrupted package: bad index.");
        return 1;
    }
    free(index);

    if (!build_cache_dir(cache, SFX_PATH_MAX, arcCrc)) {
        CloseHandle(g_self);
        notify(L"Cannot locate the application data directory.");
        return 1;
    }

    if (!marker_valid(cache, arcCrc)) {
        wchar_t stage[SFX_PATH_MAX];
        int ok;

        /* 先解压到带 pid 的暂存目录，成功后再接管正式目录：
           避免解压中断或并发启动时互相破坏 */
        SFX_SNWPRINTF(stage, SFX_PATH_MAX, L"%s.%u.tmp", cache, GetCurrentProcessId());
        stage[SFX_PATH_MAX - 1] = L'\0';
        remove_tree(stage);
        if (!make_dir(stage)) {
            CloseHandle(g_self);
            notify(L"Cannot create the runtime directory.");
            return 1;
        }

        /* 先建目录树，保证空目录也存在 */
        for (i = 0; i < g_entryCount; ++i) {
            wchar_t path[SFX_PATH_MAX];
            wchar_t *wrel;
            if (!g_entries[i].is_dir) continue;
            wrel = rel_to_wide(g_entries[i].rel);
            if (!wrel) break;
            SFX_SNWPRINTF(path, SFX_PATH_MAX, L"%s\\%s", stage, wrel);
            path[SFX_PATH_MAX - 1] = L'\0';
            free(wrel);
            make_dir(path);
        }

        SFX_SNWPRINTF(g_root, SFX_PATH_MAX, L"%s", stage);
        g_root[SFX_PATH_MAX - 1] = L'\0';

        ui_show();
        ok = extract();
        ui_done();

        if (!ok) {
            remove_tree(stage);
            CloseHandle(g_self);
            notify(L"Failed to unpack the runtime environment.");
            return 1;
        }
        write_marker(stage, arcCrc);

        if (marker_valid(cache, arcCrc)) {
            remove_tree(stage);                 /* 期间别的进程已经准备好了 */
        } else {
            remove_tree(cache);
            if (MoveFileExW(stage, cache, 0)) {
                cleanup_old_cache(cache, arcCrc);
            } else if (marker_valid(cache, arcCrc)) {
                remove_tree(stage);
            } else {
                SFX_SNWPRINTF(cache, SFX_PATH_MAX, L"%s", stage);   /* 退化为使用暂存目录 */
                cache[SFX_PATH_MAX - 1] = L'\0';
            }
        }
    }

    CloseHandle(g_self);
    return run_entry(cache, entryRel);
}
