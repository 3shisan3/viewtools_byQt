/*
 * 单文件分发包 —— 构建期打包器（Windows）
 *
 * 用法：
 *   sfx_packer --dir <部署目录> --entry <主程序相对路径>
 *              --launcher <launcher.exe> --out <输出 exe>
 *              [--level 1-9] [--no-compress]
 *
 * 流程：
 *   1. 递归收集部署目录（自动跳过 --out 自身）
 *   2. 生成 archive（索引 + 数据区，数据区默认用 zlib 压缩）写入临时文件
 *   3. 输出文件 = launcher.exe + archive + footer
 *
 * 日志统一使用 ASCII，避免不同控制台代码页造成乱码。
 */
#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <zlib.h>

#include "sfx_format.h"

#define SFX_BUFFER_SIZE (1024u * 1024u)
#define SFX_PATH_MAX    4096u

#if defined(_MSC_VER)
#  define SFX_SNWPRINTF _snwprintf
#else
#  define SFX_SNWPRINTF swprintf
#endif

/* 写入后强制补 0，防止 _snwprintf 截断时不结尾 */
#define SFX_FIX_TERM(buf) ((buf)[SFX_PATH_MAX - 1] = L'\0')

typedef struct {
    char     *rel;    /* UTF-8 相对路径，分隔符 '/' */
    wchar_t  *full;   /* 绝对宽字符路径 */
    uint64_t  size;
    int       is_dir;
} sfx_entry_t;

static sfx_entry_t *g_entries = NULL;
static size_t       g_count = 0;
static size_t       g_cap = 0;

static char *str_narrow(const wchar_t *w)
{
    int n;
    char *s;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    s = (char *)malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static uint64_t file_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return 0;
    return ((uint64_t)data.nFileSizeHigh << 32) | (uint64_t)data.nFileSizeLow;
}

static void add_entry(char *rel, const wchar_t *full, uint64_t size, int is_dir)
{
    if (g_count == g_cap) {
        size_t ncap = g_cap ? g_cap * 2 : 128;
        sfx_entry_t *p = (sfx_entry_t *)realloc(g_entries, ncap * sizeof(sfx_entry_t));
        if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
        g_entries = p;
        g_cap = ncap;
    }
    g_entries[g_count].rel = rel;
    g_entries[g_count].size = size;
    g_entries[g_count].is_dir = is_dir;
    g_entries[g_count].full = _wcsdup(full);
    if (!g_entries[g_count].full) { fprintf(stderr, "out of memory\n"); exit(2); }
    ++g_count;
}

/* 递归收集目录；exclude 为需要忽略的文件绝对路径（输出 exe 自身） */
static void collect_dir(const wchar_t *dir, const char *prefix, const wchar_t *exclude)
{
    wchar_t pattern[SFX_PATH_MAX];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    SFX_SNWPRINTF(pattern, SFX_PATH_MAX, L"%s\\*", dir);
    SFX_FIX_TERM(pattern);

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        wchar_t child[SFX_PATH_MAX];
        char *name;
        char *rel;
        size_t plen;

        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        SFX_SNWPRINTF(child, SFX_PATH_MAX, L"%s\\%s", dir, fd.cFileName);
        SFX_FIX_TERM(child);

        name = str_narrow(fd.cFileName);
        if (!name) continue;

        plen = prefix ? strlen(prefix) : 0;
        rel = (char *)malloc(plen + strlen(name) + 2);
        if (!rel) { free(name); continue; }
        if (plen) { memcpy(rel, prefix, plen); rel[plen] = '/'; ++plen; }
        memcpy(rel + plen, name, strlen(name) + 1);
        free(name);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            add_entry(rel, child, 0, 1);
            collect_dir(child, rel, exclude);
        } else {
            uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow;
            if (exclude && *exclude && _wcsicmp(child, exclude) == 0) { free(rel); continue; }
            add_entry(rel, child, size, 0);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

/* 生成 archive 到临时文件，返回 0 表示失败 */
static int build_archive(const wchar_t *tmpPath, const char *entryRel, int level, int compress,
                         uint64_t *outArcSize, uint64_t *outRawSize, uint32_t *outCrc)
{
    unsigned char head[SFX_ARC_FIXED_SIZE];
    unsigned char *in;
    unsigned char *out;
    FILE *f;
    size_t i;
    size_t entryLen = strlen(entryRel);
    uint64_t indexSize = 0;
    uint64_t rawSize = 0;
    uint64_t dataSize = 0;
    uint32_t crc = 0;
    int ok = 1;

    for (i = 0; i < g_count; ++i) indexSize += SFX_IDX_FIXED_SIZE + strlen(g_entries[i].rel);

    f = _wfopen(tmpPath, L"wb");
    if (!f) return 0;

    in = (unsigned char *)malloc(SFX_BUFFER_SIZE);
    out = (unsigned char *)malloc(SFX_BUFFER_SIZE);
    if (!in || !out) { fclose(f); free(in); free(out); return 0; }

    memset(head, 0, sizeof(head));
    memcpy(head, SFX_ARCHIVE_MAGIC, 8);
    sfx_put_u32(head + SFX_ARC_OFF_VERSION, SFX_VERSION);
    sfx_put_u32(head + SFX_ARC_OFF_FLAGS, compress ? SFX_FLAG_DEFLATE : 0u);
    sfx_put_u32(head + SFX_ARC_OFF_ENTRY_LEN, (uint32_t)entryLen);
    sfx_put_u64(head + SFX_ARC_OFF_COUNT, (uint64_t)g_count);
    sfx_put_u64(head + SFX_ARC_OFF_INDEX_SIZE, indexSize);

    if (fwrite(head, 1, sizeof(head), f) != sizeof(head)) ok = 0;
    if (ok && entryLen && fwrite(entryRel, 1, entryLen, f) != entryLen) ok = 0;

    /* 索引区 */
    for (i = 0; i < g_count && ok; ++i) {
        unsigned char e[SFX_IDX_FIXED_SIZE];
        size_t nlen = strlen(g_entries[i].rel);
        memset(e, 0, sizeof(e));
        sfx_put_u16(e + SFX_IDX_OFF_NAME_LEN, (uint16_t)nlen);
        e[SFX_IDX_OFF_TYPE] = (unsigned char)(g_entries[i].is_dir ? SFX_ENTRY_DIR : SFX_ENTRY_FILE);
        sfx_put_u64(e + SFX_IDX_OFF_SIZE, g_entries[i].size);
        if (fwrite(e, 1, sizeof(e), f) != sizeof(e)) { ok = 0; break; }
        if (fwrite(g_entries[i].rel, 1, nlen, f) != nlen) { ok = 0; break; }
    }

    /* 数据区：所有文件内容顺序拼接，可选整体 zlib 压缩 */
    if (ok) {
        z_stream s;
        memset(&s, 0, sizeof(s));
        if (compress && deflateInit(&s, level) != Z_OK) ok = 0;

        for (i = 0; i < g_count && ok; ++i) {
            FILE *src;
            if (g_entries[i].is_dir) continue;
            src = _wfopen(g_entries[i].full, L"rb");
            if (!src) { fprintf(stderr, "skip unreadable file: %s\n", g_entries[i].rel); ok = 0; break; }
            for (;;) {
                size_t n = fread(in, 1, SFX_BUFFER_SIZE, src);
                if (n == 0) break;
                crc = (uint32_t)crc32(crc, in, (uInt)n);
                rawSize += n;
                if (!compress) {
                    if (fwrite(in, 1, n, f) != n) { ok = 0; break; }
                    dataSize += n;
                    continue;
                }
                s.next_in = in;
                s.avail_in = (uInt)n;
                while (s.avail_in > 0) {
                    size_t produced;
                    s.next_out = out;
                    s.avail_out = SFX_BUFFER_SIZE;
                    if (deflate(&s, Z_NO_FLUSH) != Z_OK) { ok = 0; break; }
                    produced = SFX_BUFFER_SIZE - s.avail_out;
                    if (produced) {
                        if (fwrite(out, 1, produced, f) != produced) { ok = 0; break; }
                        dataSize += produced;
                    }
                }
            }
            fclose(src);
        }

        if (ok && compress) {
            s.next_in = in;
            s.avail_in = 0;
            for (;;) {
                size_t produced;
                int r;
                s.next_out = out;
                s.avail_out = SFX_BUFFER_SIZE;
                r = deflate(&s, Z_FINISH);
                produced = SFX_BUFFER_SIZE - s.avail_out;
                if (produced) {
                    if (fwrite(out, 1, produced, f) != produced) { ok = 0; break; }
                    dataSize += produced;
                }
                if (r == Z_STREAM_END) break;
                if (r != Z_OK && r != Z_BUF_ERROR) { ok = 0; break; }
            }
            deflateEnd(&s);
        }
    }

    /* 回填固定头中的尺寸与校验值 */
    if (ok) {
        sfx_put_u64(head + SFX_ARC_OFF_DATA_SIZE, dataSize);
        sfx_put_u64(head + SFX_ARC_OFF_RAW_SIZE, rawSize);
        sfx_put_u32(head + SFX_ARC_OFF_CRC, crc);
        fflush(f);
        if (fseek(f, 0, SEEK_SET) != 0 ||
            fwrite(head, 1, sizeof(head), f) != sizeof(head)) ok = 0;
    }

    fclose(f);
    free(in);
    free(out);

    if (!ok) return 0;
    *outRawSize = rawSize;
    *outCrc = crc;
    *outArcSize = file_size(tmpPath);
    return (*outArcSize != 0);
}

/* launcher + archive + footer 拼装成最终 exe */
static int build_output(const wchar_t *launcherPath, const wchar_t *arcPath,
                        const wchar_t *outPath, uint64_t arcSize, uint64_t *outLauncherSize)
{
    FILE *out = _wfopen(outPath, L"wb");
    FILE *src;
    unsigned char *buf;
    uint64_t launcherSize = 0;
    uint32_t arcCrc = 0;
    unsigned char foot[SFX_FOOTER_SIZE];
    int ok = 1;

    if (!out) return 0;
    buf = (unsigned char *)malloc(SFX_BUFFER_SIZE);
    if (!buf) { fclose(out); return 0; }

    src = _wfopen(launcherPath, L"rb");
    if (!src) { fclose(out); free(buf); return 0; }
    for (;;) {
        size_t n = fread(buf, 1, SFX_BUFFER_SIZE, src);
        if (n == 0) break;
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
        launcherSize += n;
    }
    fclose(src);

    if (ok) {
        src = _wfopen(arcPath, L"rb");
        if (!src) ok = 0;
    }
    if (ok) {
        for (;;) {
            size_t n = fread(buf, 1, SFX_BUFFER_SIZE, src);
            uInt chunk;
            if (n == 0) break;
            if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
            chunk = (uInt)n;
            arcCrc = (uint32_t)crc32(arcCrc, buf, chunk);
        }
        fclose(src);
    }

    if (ok) {
        memset(foot, 0, sizeof(foot));
        memcpy(foot, SFX_FOOTER_MAGIC, 8);
        sfx_put_u32(foot + SFX_FOOT_OFF_VERSION, SFX_VERSION);
        sfx_put_u64(foot + SFX_FOOT_OFF_ARC_OFFSET, launcherSize);
        sfx_put_u64(foot + SFX_FOOT_OFF_ARC_SIZE, arcSize);
        sfx_put_u64(foot + SFX_FOOT_OFF_TOTAL_SIZE, launcherSize + arcSize + SFX_FOOTER_SIZE);
        sfx_put_u32(foot + SFX_FOOT_OFF_ARC_CRC, arcCrc);
        if (fwrite(foot, 1, sizeof(foot), out) != sizeof(foot)) ok = 0;
    }

    if (fclose(out) != 0) ok = 0;
    free(buf);

    *outLauncherSize = launcherSize;
    return ok;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: sfx_packer --dir <deploy dir> --entry <main exe> "
        "--launcher <launcher.exe> --out <output.exe> [--level 1-9] [--no-compress]\n");
}

int main(void)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *dir = NULL;
    const wchar_t *entry = NULL;
    const wchar_t *launcher = NULL;
    const wchar_t *out = NULL;
    int level = 6;
    int compress = 1;
    int i;
    wchar_t dirAbs[SFX_PATH_MAX];
    wchar_t outAbs[SFX_PATH_MAX];
    wchar_t tmpPath[SFX_PATH_MAX];
    char *entryRel;
    char *relCheck;
    uint64_t arcSize = 0;
    uint64_t rawSize = 0;
    uint64_t launcherSize = 0;
    uint64_t outSize = 0;
    uint32_t crc = 0;
    size_t dirLen;

    if (!argv || argc < 2) { usage(); return 2; }

    for (i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--dir") == 0 && i + 1 < argc) dir = argv[++i];
        else if (wcscmp(argv[i], L"--entry") == 0 && i + 1 < argc) entry = argv[++i];
        else if (wcscmp(argv[i], L"--launcher") == 0 && i + 1 < argc) launcher = argv[++i];
        else if (wcscmp(argv[i], L"--out") == 0 && i + 1 < argc) out = argv[++i];
        else if (wcscmp(argv[i], L"--level") == 0 && i + 1 < argc) level = _wtoi(argv[++i]);
        else if (wcscmp(argv[i], L"--no-compress") == 0) compress = 0;
        else { usage(); return 2; }
    }

    if (!dir || !entry || !launcher || !out) { usage(); return 2; }
    if (level < 1) level = 1;
    if (level > 9) level = 9;

    if (!_wfullpath(dirAbs, dir, SFX_PATH_MAX)) { fprintf(stderr, "invalid --dir\n"); return 2; }
    if (!_wfullpath(outAbs, out, SFX_PATH_MAX)) { fprintf(stderr, "invalid --out\n"); return 2; }

    /* 去掉目录末尾的分隔符 */
    dirLen = wcslen(dirAbs);
    while (dirLen > 3 && (dirAbs[dirLen - 1] == L'\\' || dirAbs[dirLen - 1] == L'/')) {
        dirAbs[dirLen - 1] = L'\0';
        --dirLen;
    }

    entryRel = str_narrow(entry);
    if (!entryRel) { fprintf(stderr, "invalid --entry\n"); return 2; }
    relCheck = entryRel;
    for (; *relCheck; ++relCheck) if (*relCheck == '\\') *relCheck = '/';

    wprintf(L"[sfx] collecting: %s\n", dirAbs);
    collect_dir(dirAbs, NULL, outAbs);
    wprintf(L"[sfx] entries: %llu\n", (unsigned long long)g_count);
    if (g_count == 0) { fwprintf(stderr, L"nothing to pack\n"); return 2; }

    SFX_SNWPRINTF(tmpPath, SFX_PATH_MAX, L"%s.sfx.tmp", outAbs);
    SFX_FIX_TERM(tmpPath);

    if (!build_archive(tmpPath, entryRel, level, compress, &arcSize, &rawSize, &crc)) {
        fprintf(stderr, "failed to build archive\n");
        _wremove(tmpPath);
        return 1;
    }

    if (!build_output(launcher, tmpPath, outAbs, arcSize, &launcherSize)) {
        fprintf(stderr, "failed to build output\n");
        _wremove(tmpPath);
        return 1;
    }
    _wremove(tmpPath);

    outSize = file_size(outAbs);
    wprintf(L"[sfx] packed %llu files, %.1f MB -> %.1f MB (ratio %.0f%%), output %s (%.1f MB)\n",
            (unsigned long long)g_count,
            rawSize / 1048576.0,
            arcSize / 1048576.0,
            rawSize ? (arcSize * 100.0 / rawSize) : 0.0,
            outAbs,
            outSize / 1048576.0);
    return 0;
}
