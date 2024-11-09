

#ifndef TEMP_H
#define TEMP_H 1

void annotate_memory_inaccessible(void *ptr, size_t size);
void annotate_memory_undefined(void *ptr, size_t size);

typedef void *bucket_t;
typedef struct slab_list_item_t slab_list_item_t;

// Represents the allocated memory block of size 'slab_min_size'
// Internally, it splits the memory block into chunks. The number of
// chunks depends of the size of a Bucket which created the Slab.
// Note: Bucket's methods are responsible for thread safety of Slab access,
// so no locking happens here.
typedef struct slab_t {
    // Pointer to the allocated memory of slab_min_size bytes
    void *mem_ptr;
    size_t slab_size;

    // Represents the current state of each chunk: if the bit is set then the
    // chunk is allocated, and if the chunk is free for allocation otherwise
    bool *chunks;
    size_t num_chunks;

    // Total number of allocated chunks at the moment.
    size_t num_allocated;

    // The bucket which the slab belongs to
    bucket_t bucket;

    // Hints where to start search for free chunk in a slab
    size_t first_free_chunk_idx;

    // Store iterator to the corresponding node in avail/unavail list
    // to achieve O(1) removal
    slab_list_item_t *iter;
} slab_t;

typedef struct slab_list_item_t {
    slab_t *val;
    struct slab_list_item_t *prev, *next;
} slab_list_item_t;

slab_t *create_slab(bucket_t bucket);
void destroy_slab(slab_t *slab);

void *slab_get(const slab_t *slab);
void *slab_get_end(const slab_t *slab);
bucket_t slab_get_bucket(const slab_t *slab);
void *slab_get_chunk(slab_t *slab);
size_t slab_get_num_chunks(const slab_t *slab);
size_t slab_get_chunk_size(const slab_t *slab);
size_t slab_get_num_allocated(const slab_t *slab);

bool slab_has_avail(const slab_t *slab);
void slab_free_chunk(slab_t *slab, void *ptr);

void slab_reg(slab_t *slab);
void slab_unreg(slab_t *slab);

#endif // TEMP_H