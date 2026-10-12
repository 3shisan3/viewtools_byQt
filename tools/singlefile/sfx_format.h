/*
 * 单文件分发包的二进制格式定义（构建期打包器 packer.c 与运行期启动器 launcher.c 共用）
 *
 * 单文件 exe 的布局：
 *   [ launcher.exe 原始内容 ]
 *   [ archive：64 字节固定头 + 主程序相对路径 + 条目索引 + 数据区 ]
 *   [ footer：48 字节，固定位于文件末尾，便于从尾部定位 ]
 *
 * 所有整数统一按小端逐字节读写，不使用结构体直接映射，避免编译器对齐差异。
 */
#ifndef SFX_FORMAT_H
#define SFX_FORMAT_H

#include <stdint.h>
#include <stddef.h>

#define SFX_FOOTER_MAGIC  "VTSFX010"
#define SFX_ARCHIVE_MAGIC "VTARC010"
#define SFX_FOOTER_SIZE   48u
#define SFX_VERSION       1u

/* archive.flags 位定义 */
#define SFX_FLAG_DEFLATE  0x1u   /* 数据区为 zlib 压缩流，否则为原始字节 */

/* 索引条目类型 */
#define SFX_ENTRY_FILE 0u
#define SFX_ENTRY_DIR  1u

/* ---- footer 字段偏移（共 48 字节） ---- */
#define SFX_FOOT_OFF_MAGIC      0u   /* char[8]  */
#define SFX_FOOT_OFF_VERSION    8u   /* u32      */
#define SFX_FOOT_OFF_FLAGS      12u  /* u32，保留 */
#define SFX_FOOT_OFF_ARC_OFFSET 16u  /* u64，archive 起始偏移 */
#define SFX_FOOT_OFF_ARC_SIZE   24u  /* u64，archive 字节数   */
#define SFX_FOOT_OFF_TOTAL_SIZE 32u  /* u64，整个 exe 字节数（完整性校验） */
#define SFX_FOOT_OFF_ARC_CRC    40u  /* u32，archive 的 CRC32（决定缓存目录名） */
#define SFX_FOOT_OFF_RESERVED   44u  /* u32 */

/* ---- archive 固定头（64 字节，其后紧跟主程序相对路径） ---- */
#define SFX_ARC_OFF_MAGIC       0u   /* char[8]  */
#define SFX_ARC_OFF_VERSION     8u   /* u32      */
#define SFX_ARC_OFF_FLAGS       12u  /* u32      */
#define SFX_ARC_OFF_ENTRY_LEN   16u  /* u32，主程序相对路径字节数 */
#define SFX_ARC_OFF_RESERVED    20u  /* u32 */
#define SFX_ARC_OFF_COUNT       24u  /* u64，条目数量 */
#define SFX_ARC_OFF_INDEX_SIZE  32u  /* u64，索引区字节数 */
#define SFX_ARC_OFF_DATA_SIZE   40u  /* u64，数据区字节数（压缩后） */
#define SFX_ARC_OFF_RAW_SIZE    48u  /* u64，数据区字节数（解压后） */
#define SFX_ARC_OFF_CRC         56u  /* u32，解压后数据的 CRC32 */
#define SFX_ARC_OFF_RESERVED2   60u  /* u32 */
#define SFX_ARC_FIXED_SIZE      64u

/* ---- 索引条目（16 字节固定头 + UTF-8 名称，路径分隔符为 '/'） ---- */
#define SFX_IDX_OFF_NAME_LEN    0u   /* u16 */
#define SFX_IDX_OFF_TYPE        2u   /* u8  */
#define SFX_IDX_OFF_PAD         3u   /* u8  */
#define SFX_IDX_OFF_RESERVED    4u   /* u32 */
#define SFX_IDX_OFF_SIZE        8u   /* u64 */
#define SFX_IDX_FIXED_SIZE      16u

static inline void sfx_put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

static inline void sfx_put_u32(unsigned char *p, uint32_t v)
{
    unsigned int i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)((v >> (8u * i)) & 0xffu);
}

static inline void sfx_put_u64(unsigned char *p, uint64_t v)
{
    unsigned int i;
    for (i = 0; i < 8; ++i) p[i] = (unsigned char)((v >> (8u * i)) & 0xffu);
}

static inline uint16_t sfx_get_u16(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t sfx_get_u32(const unsigned char *p)
{
    uint32_t v = 0;
    unsigned int i;
    for (i = 0; i < 4; ++i) v |= ((uint32_t)p[i]) << (8u * i);
    return v;
}

static inline uint64_t sfx_get_u64(const unsigned char *p)
{
    uint64_t v = 0;
    unsigned int i;
    for (i = 0; i < 8; ++i) v |= ((uint64_t)p[i]) << (8u * i);
    return v;
}

#endif /* SFX_FORMAT_H */
