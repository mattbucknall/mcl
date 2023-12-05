/**
 * MCL - Minimal TCL-like interpreter with small memory footprint.
 *
 * Copyright 2023 Matthew T. Bucknall
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef MCL_H
#define MCL_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ======== PUBLIC CONSTANTS ========

#define MCL_MIN_HEAP_ENTRIES        16 // TODO: Figure out proper value
#define MCL_MAX_STRING_LEN          32767


// ======== PUBLIC TYPES ========

typedef enum {
    MCL_RESULT_OK,
    MCL_RESULT_OUT_OF_MEMORY,
    MCL_RESULT_RUNTIME_ERROR,
    MCL_RESULT_SYNTAX_ERROR
} mcl_result_t;

typedef void* mcl_heap_type_t;

typedef struct mcl mcl_t;


// ======== PUBLIC FUNCTIONS ========

mcl_result_t mcl_init(mcl_t* mcl, mcl_heap_type_t* heap, size_t n_heap_entries,
              void* user_data);

void* mcl_user_data(const mcl_t* ctx);


// ======== STRING ========




// ======== PRIVATE ========
#ifndef _DOXYGEN

struct mcl {
    void* user_data;
    void* heap_start;
    void* volatile heap_ptr;
    mcl_heap_type_t* stack_end;
    mcl_heap_type_t* volatile stack_ptr;
    mcl_heap_type_t* frame_ptr;
    jmp_buf* volatile jb;

#ifdef MCL_DEBUG
    uint32_t tag;
#endif // MCL_DEBUG
};

#endif // _DOXYGEN

#ifdef __cplusplus
}
#endif

#endif // MCL_H
