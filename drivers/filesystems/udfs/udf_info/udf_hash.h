#ifndef UDFS_HASH_H
#define UDFS_HASH_H

#include <stdint.h>

/* Shared VFAT-style 32-bit hash recurrence for one code unit. */
static __inline uint32_t
UDFHashCharacter(
    uint32_t Hash,
    uint32_t Character
    )
{
    return (Hash + (Character << 4) + (Character >> 4)) * 11;
}

#endif
