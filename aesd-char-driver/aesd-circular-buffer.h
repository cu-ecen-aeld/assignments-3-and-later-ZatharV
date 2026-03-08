/*
* aesd-circular-buffer.h
*
*  Created on: March 1st, 2020
*      Author: Dan Walkes
*/

#ifndef AESD_CIRCULAR_BUFFER_H
#define AESD_CIRCULAR_BUFFER_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#endif

#define AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED 10

struct aesd_buffer_entry
{
    const char *buffptr;
    size_t size;
};

struct aesd_circular_buffer
{
    struct aesd_buffer_entry  entry[AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED];
    uint8_t in_offs;
    uint8_t out_offs;
    bool full;
};

extern struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(
    struct aesd_circular_buffer *buffer,
    size_t char_offset,
    size_t *entry_offset_byte_rtn);

/**
 * Adds entry to buffer. Returns pointer to the overwritten (evicted) entry
 * if the buffer was full, so caller can free its memory. Returns NULL otherwise.
 */
extern const struct aesd_buffer_entry *aesd_circular_buffer_add_entry(
    struct aesd_circular_buffer *buffer,
    const struct aesd_buffer_entry *add_entry);

extern void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer);

#define AESD_CIRCULAR_BUFFER_FOREACH(entryptr,buffer,index) \
    for(index=0, entryptr=&((buffer)->entry[index]); \
            index<AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; \
            index++, entryptr=&((buffer)->entry[index]))

#endif /* AESD_CIRCULAR_BUFFER_H */