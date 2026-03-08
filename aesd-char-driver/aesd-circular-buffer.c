/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer imlementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(
        struct aesd_circular_buffer *buffer,
        size_t char_offset,
        size_t *entry_offset_byte_rtn)
{
    size_t curr_offset = 0;
    uint8_t idx;
    uint8_t i;
    uint8_t count;

    if (!buffer || !entry_offset_byte_rtn)
        return NULL;

    /* Empty buffer: in_offs == out_offs and not full */
    if ((buffer->in_offs == buffer->out_offs) && !buffer->full)
        return NULL;

    /* How many valid entries are there? */
    if (buffer->full) {
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else if (buffer->in_offs >= buffer->out_offs) {
        count = buffer->in_offs - buffer->out_offs;
    } else {
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED
                - buffer->out_offs
                + buffer->in_offs;
    }

    /* Walk entries from oldest (out_offs) forward, count bytes */
    for (i = 0; i < count; i++) {
        struct aesd_buffer_entry *entry;

        idx = (buffer->out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        entry = &buffer->entry[idx];

        if (char_offset < curr_offset + entry->size) {
            *entry_offset_byte_rtn = char_offset - curr_offset;
            return entry;
        }

        curr_offset += entry->size;
    }

    /* char_offset beyond total data */
    return NULL;
}


/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
const char *aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    /**
    * TODO: implement per description
    */

    const char *retptr = NULL;

    // Check for NULL pointers before dereferencing
    if((buffer == NULL) || (add_entry == NULL))
    {
        return retptr;
    }

    if (buffer->full) {
        retptr = buffer->entry[buffer->in_offs].buffptr;
    }

    // Add entry
    buffer->entry[buffer->in_offs] = *add_entry;

    // Add and wrap around
    buffer->in_offs = (buffer->in_offs + 1)%AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    // If full, increment out index
    if(buffer->full)
    {
        buffer->out_offs = (buffer->out_offs + 1)%AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    // Check if buffer is full
    if(buffer->in_offs == buffer->out_offs)
    {
        buffer->full = true;
    }

    return retptr;
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}
