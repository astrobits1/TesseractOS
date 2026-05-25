#ifndef LIBK_HASHTB_H
#define LIBK_HASHTB_H

#include <stdint.h>

enum HASHTB_BUCKET_STATE {
    HASHTB_BUCKET_USED,
    HASHTB_BUCKET_FREE,
    HASHTB_BUCKET_TOMBSTONE
};

/* 16 byte bucket size optimization:
 *
 * Key is a 64 bit unsigned integer, but bit 51 is mirrored
 * till bit 63. This is a valuable optimization as it allows for bucket
 * state to be stored within the key, making bucket only 16 bytes.
 * Which makes it cache friendly, a power of 2 and compact.
 *
 * This also allows virtual and physical pointers to be used as keys
 * because these couple most significant bits are just bit 47 mirrors
 * in case of virtual pointers, while physical pointers are capped at
 * 52 bits in x86_64 with PML4 
 *
 * For all practical purposes, if raw integers were to be used 
 * as keys they shouldnt exceed 52 bits (signed are sign extended
 * due to mirroring) */
struct bucket {
    uint64_t key;
    uint64_t value;
};
_Static_assert((sizeof(struct bucket)&(sizeof(struct bucket)-1)) == 0, \
        "'struct bucket' must be a power of 2");

/* Configurable functions */
struct hashtb_ops {
    uint64_t (*hashing_algorithm)(uint64_t key);
    void* (*buffer_allocate_pow2)(uint64_t);
    void (*buffer_free)(void*);
};
    
struct hashtb {
    struct bucket* bucket_list;
    /* Buffer size must be a power of 2 strictly
     * must be > sizeof(bucket) */
    uint64_t list_size;
    uint64_t bucket_count;
    uint64_t entry_count;
    uint64_t tombstone_count;

    struct hashtb_ops ops;
};

int hashtb_initialize(struct hashtb* tb, uint64_t buffer_size, struct hashtb_ops ops);
void hashtb_free(struct hashtb* tb);
int hashtb_insert(struct hashtb* tb, uint64_t key, uint64_t value);
int hashtb_read(struct hashtb* tb, uint64_t key, uint64_t* _value);
int hashtb_remove(struct hashtb* tb, uint64_t key);

/* Hashing algorithms */
uint64_t hash_multiplicative(uint64_t key);

#endif
