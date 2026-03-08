/**
* @file aesd-circular-buffer.c
* @brief Functions and data related to a circular buffer implementation
*
* @author Dan Walkes
* @date 2020-03-01
* @copyright Copyright (c) 2020
*/

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif
#include "aesd-circular-buffer.h"

struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(
    struct aesd_circular_buffer *buffer,
    size_t char_offset,
    size_t *entry_offset_byte_rtn)
{
    if (buffer == NULL || entry_offset_byte_rtn == NULL)
        return NULL;

    uint8_t i;
    uint8_t index;
    size_t cumulative = 0;

    for (i = 0; i < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; i++) {
        index = (buffer->out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

        if (!buffer->full && index == buffer->in_offs)
            break;

        if (buffer->entry[index].buffptr == NULL)
            break;

        size_t entry_size = buffer->entry[index].size;

        if (char_offset < cumulative + entry_size) {
            *entry_offset_byte_rtn = char_offset - cumulative;
            return &buffer->entry[index];
        }

        cumulative += entry_size;
    }

    return NULL;
}

/**
 * Adds entry to buffer. If the buffer is full, the oldest entry is overwritten.
 * Returns a pointer to the overwritten entry (before overwrite) so the caller
 * can free its memory. Returns NULL if no entry was overwritten.
 */
const struct aesd_buffer_entry *aesd_circular_buffer_add_entry(
    struct aesd_circular_buffer *buffer,
    const struct aesd_buffer_entry *add_entry)
{
    const struct aesd_buffer_entry *evicted = NULL;

    if (buffer == NULL || add_entry == NULL)
        return NULL;

    /* If full, save evicted entry info BEFORE overwriting */
    if (buffer->full) {
        evicted = &buffer->entry[buffer->in_offs];
        buffer->out_offs = (buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    /* Write new entry */
    buffer->entry[buffer->in_offs] = *add_entry;
    buffer->in_offs = (buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    /* Mark full if in_offs catches out_offs */
    if (buffer->in_offs == buffer->out_offs)
        buffer->full = true;

    return evicted;
}

void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer, 0, sizeof(struct aesd_circular_buffer));
}