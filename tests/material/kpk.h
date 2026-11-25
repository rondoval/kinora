/* probe packet file: big-endian words; header, extradata, then (size, data) per packet */
#ifndef KPK_H
#define KPK_H
#include <stdio.h>
#include <stdint.h>
#define KPK_MAGIC 0x4B504B31u
static inline void kpk_put(FILE *file, uint32_t value)
{
    uint8_t bytes[4] = { value >> 24, value >> 16, value >> 8, value };
    fwrite(bytes, 1, 4, file);
}
static inline uint32_t kpk_get(const uint8_t **cursor)
{
    const uint8_t *bytes = *cursor;
    *cursor += 4;
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}
#endif
