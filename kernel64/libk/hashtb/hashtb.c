#include <libk/hashtb.h>
#include <common/drivers/vga/vga.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define HASHTB_THRESHOLD_LF 0.75

static inline enum HASHTB_BUCKET_STATE hashtb_bucket_get_state(struct bucket* b) {
    return (b->key>>56)&0xF;
}

static inline uint64_t hashtb_bucket_get_key(struct bucket* b) {
    return (uint64_t)((int64_t)(b->key<<11))>>11;
}

static inline uint64_t hashtb_bucket_get_value(struct bucket* b) {
    return b->value;
}

static void hashtb_bucket_set_state(struct bucket* b, enum HASHTB_BUCKET_STATE state) {
    b->key &= (1ULL<<52)-1;
    b->key |= ((uint64_t)state&0xF)<<56;
}

static void hashtb_bucket_set_key(struct bucket* b, uint64_t key) {
    b->key &= ~((1ULL<<52)-1);
    b->key |= key&((1ULL<<52)-1);
}

static inline void hashtb_bucket_set_value(struct bucket* b, uint64_t value) {
    b->value = value;
}

/* Initialize all entries to NULL */
static void hashtb_initialize_bucket_list(struct hashtb* tb) {
    for (uint64_t i=0; i<tb->bucket_count; i++) {
        struct bucket* b = &tb->bucket_list[i];
        hashtb_bucket_set_state(b, HASHTB_BUCKET_FREE);
        hashtb_bucket_set_key(b, 0);
        hashtb_bucket_set_value(b, 0);
    }
}

static struct bucket* hashtb_lookup_bucket(struct hashtb* tb, uint64_t key) {
    uint64_t index = tb->ops.hashing_algorithm(key) & (tb->bucket_count-1);
    struct bucket* b = &tb->bucket_list[index];
    uint64_t initial_index = index;

    for (;;) {
        enum HASHTB_BUCKET_STATE state = hashtb_bucket_get_state(b);
        if (state == HASHTB_BUCKET_FREE) {
            /* Not found */
            return NULL;
        } else if (state == HASHTB_BUCKET_USED) {
            /* Found */
            if (hashtb_bucket_get_key(b) == key)
                break;
        }

        /* Tombstones and used are stepped over in linear probe */
        index++;
        index &= tb->bucket_count-1;
        b = &tb->bucket_list[index];

        /* If all buckets are used, to prevent an infinite loop
         * we add this, but this should never practically occur 
         * when used properly */
        if (initial_index == index)
            return NULL;
    }

    return b;
}

static void hashtb_rehash(struct hashtb* tb, struct bucket* old_buffer) {
    for (uint64_t i=0; i<tb->bucket_count>>1; i++) {
        struct bucket* b = &old_buffer[i];

        if (hashtb_bucket_get_state(b) == HASHTB_BUCKET_USED) {
            uint64_t key = hashtb_bucket_get_key(b);
            uint64_t value = hashtb_bucket_get_value(b);
            hashtb_insert(tb, key, value);
        }
    }
}

int hashtb_initialize(struct hashtb* tb, uint64_t buffer_size, struct hashtb_ops ops) {
    if ((buffer_size&(buffer_size-1)) != 0) {
//        printf("Buffer size must be a power of 2\n");
        return 1;
    }
    if (buffer_size < sizeof(struct bucket)) {
//        printf("Buffer is too small\n");
        return 2;
    }

    void* buffer = ops.buffer_allocate_pow2(buffer_size);
    if (buffer == NULL)
        return 3;

    tb->bucket_list = buffer;
    tb->list_size = buffer_size;
    /* sizeof(struct bucket) must strictly be a power of 2 */
    tb->bucket_count = buffer_size/sizeof(struct bucket);
    tb->entry_count = 0;
    tb->tombstone_count = 0;

    tb->ops = ops;
    /* Initialize all entries */
    hashtb_initialize_bucket_list(tb);

    return 0;
}

void hashtb_free(struct hashtb* tb) {
    /* Free buffer */
    if (tb->bucket_list != NULL)
        tb->ops.buffer_free(tb->bucket_list);
}

int hashtb_insert(struct hashtb* tb, uint64_t key, uint64_t value) {
    /* Extract higher bits for maximum entropy */
    uint64_t index = tb->ops.hashing_algorithm(key) & (tb->bucket_count-1);
    struct bucket* b = &tb->bucket_list[index];
    uint64_t initial_index = index;

    struct bucket* first_tombstone = NULL;
    /* Linear Probing  */
    for (;;) {
        enum HASHTB_BUCKET_STATE state = hashtb_bucket_get_state(b);
        uint64_t b_key = hashtb_bucket_get_key(b);

        if (state == HASHTB_BUCKET_TOMBSTONE && first_tombstone == NULL)
            first_tombstone = b;
        else if (state == HASHTB_BUCKET_USED && b_key == key) {
            /* If key already exists then overwrite */
            hashtb_bucket_set_value(b, value);
            return 0;
        } else if (state == HASHTB_BUCKET_FREE)
            break;

        index++;
        index &= tb->bucket_count-1;
        b = &tb->bucket_list[index];

        /* If all buckets are used, to prevent an infinite loop
         * we add this, but this should never practically occur 
         * when used properly */
        if (initial_index == index) {
            if (first_tombstone == NULL)
                return 1;
            else
                break;
        }
    }

    /* New insert, free has been found */
    if (first_tombstone != NULL) {
        /* Tombstone can be reused */
        b = first_tombstone;
        tb->tombstone_count--;
    }

    hashtb_bucket_set_key(b, key);
    hashtb_bucket_set_value(b, value);
    hashtb_bucket_set_state(b, HASHTB_BUCKET_USED);
    tb->entry_count++;

    /* Check load factor and trigger rehashing if necessary */
    double load_factor = ((double)tb->entry_count+tb->tombstone_count)/tb->bucket_count;
    if (load_factor > HASHTB_THRESHOLD_LF) {
        struct bucket* new_buffer = tb->ops.buffer_allocate_pow2(tb->list_size<<1);
        if (new_buffer == NULL)
            return 2;

        /* Free old buffer */
        struct bucket* old_buffer = tb->bucket_list;

        /* Update hashtb */
        tb->bucket_list = new_buffer;
        tb->list_size<<=1;
        tb->bucket_count<<=1;
        tb->entry_count = 0;
        tb->tombstone_count = 0;

        hashtb_initialize_bucket_list(tb);
        /* Rehash all entries to fit the bigger space */
        hashtb_rehash(tb, old_buffer);

        tb->ops.buffer_free(old_buffer);
    }

    return 0;
}

int hashtb_read(struct hashtb* tb, uint64_t key, uint64_t* _value) {
    struct bucket* b = hashtb_lookup_bucket(tb, key);

    if (b != NULL) {
        *_value = hashtb_bucket_get_value(b);
        return 0;
    } else
        return 1;
}

int hashtb_remove(struct hashtb* tb, uint64_t key) {
    struct bucket* b = hashtb_lookup_bucket(tb, key);

    if (b != NULL) {
        hashtb_bucket_set_state(b, HASHTB_BUCKET_TOMBSTONE);
        tb->entry_count--;
        tb->tombstone_count++;
        return 0;
    } else
        return 1;
}
