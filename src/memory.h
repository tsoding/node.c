#ifndef MEMORY_H_
#define MEMORY_H_

#include <assert.h>
#include <stdint.h>

#define KILO 1024
#define MEGA (1024 * KILO)
#define GIGA (1024 * MEGA)

typedef struct {
    size_t capacity;
    size_t size;
    uint8_t *buffer;
} Memory;

static inline
void *memory_alloc(Memory *memory, size_t size)
{
    assert(memory);
    assert(memory->size + size <= memory->capacity);


    void *result = memory->buffer + memory->size;
    memory->size += size;

    return result;
}

static inline
void *memory_realloc(Memory *memory, void *old_buffer, size_t old_size, size_t new_size)
{
    assert(new_size >= old_size);
    void *new_buffer = memory_alloc(memory, new_size);
    memcpy(new_buffer, old_buffer, old_size);
    return new_buffer;
}

static inline
void memory_clean(Memory *memory)
{
    assert(memory);
    memory->size = 0;
}

#endif  // MEMORY_H_
