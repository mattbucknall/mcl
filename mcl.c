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

#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

#include "mcl.h"


// ======== ASSERTION MACROS ========

// Macro defining function to call on assertion failure
// NOTE: Function must not return!
#ifndef MCL_ASSERT_ABORT_FUNC
#define MCL_ASSERT_ABORT_FUNC       abort
#endif // MCL_ASSERT_ABORT_FUNC


// Random value used to tag context objects, used by MCL_ASSERT_CTX()
#define MCL_TAG                 0x2ECC52A4ul


// Causes panic if expr evaluates to 0 (and MCL_DEBUG is defined)
#ifdef MCL_DEBUG
#define MCL_ASSERT(expr)        do { if ( !(expr) ) { \
                                MCL_ASSERT_ABORT_FUNC(); }} while(0)
#else // MCL_DEBUG
#define MCL_ASSERT(expr)        do {} while(0)
#endif // MCL_DEBUG


// Causes panic if ctx is incorrectly tagged (and MCL_DEBUG is defined)
#define MCL_ASSERT_CTX(ctx)     MCL_ASSERT((ctx)->tag == MCL_TAG)


// ======== STACK PRIMITIVES ========

// Rounds pointer up to nearest heap entry boundary
static void* stack_round_up_ptr(void* ptr) {
    uintptr_t word = (uintptr_t) ptr;

    word += sizeof(ptr) - 1;
    word &= ~((uintptr_t) sizeof(ptr) - 1);

    return (void*) word;
}

// Determine number of available stack entries.
static size_t stack_space(const mcl_t* ctx) {
    MCL_ASSERT_CTX(ctx);

    return ctx->stack_ptr -
           (mcl_heap_type_t*) stack_round_up_ptr(ctx->heap_ptr);
}


// Determines current number of stack entries.
static size_t stack_height(const mcl_t* ctx) {
    MCL_ASSERT_CTX(ctx);
    return ctx->stack_end - ctx->stack_ptr;
}


// Determines if given pointer targets stack.
static bool stack_contains(const mcl_t* ctx, const mcl_heap_type_t* ptr) {
    MCL_ASSERT_CTX(ctx);
    return (ptr >= ctx->stack_ptr) && (ptr < ctx->stack_end);
}


// Pushes entry onto stack. NOTE: Does not check for space.
static void stack_push(mcl_t* ctx, void* ptr) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(stack_space(ctx) >= 1);
    ctx->stack_ptr -= 1;
    ctx->stack_ptr[0] = ptr;
}


// Pops entry from stack.
static void stack_pop(mcl_t* ctx) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(stack_height(ctx) > 0);
    ctx->stack_ptr += 1;
}


// Pops given number of entries from stack.
static void stack_pop_n(mcl_t* ctx, size_t n) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(stack_height(ctx) >= n);
    ctx->stack_ptr += n;
}


// Swaps stack entries.
static void stack_swap(mcl_heap_type_t* a, mcl_heap_type_t* b) {
    mcl_heap_type_t temp;

    temp = a[0];
    a[0] = b[0];
    b[0] = temp;
}


// ======== HEAP PRIMITIVES ========

// Determines number of available heap entries
static size_t heap_space(const mcl_t* ctx) {
    MCL_ASSERT_CTX(ctx);
    return (void*) (ctx->stack_ptr) - ctx->heap_ptr;
}


// Determines whether given pointer targets heap
static bool heap_contains(const mcl_t* ctx, void* ptr) {
    MCL_ASSERT_CTX(ctx);
    return (ptr >= ctx->heap_start) && (ptr < ctx->heap_ptr);
}


// Allocates space on heap and returns pointer to allocation
// NOTE: Does not check for space.
static void* heap_alloc(mcl_t* ctx, size_t size) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(size > 0);
    MCL_ASSERT(heap_space(ctx) >= size);

    void* ptr = ctx->heap_ptr;
    ctx->heap_ptr += size;
    return ptr;
}


// Grows existing allocation on heap, relocating any allocations above it.
// NOTE: Does not check for space.
static void heap_grow(mcl_t* ctx, void* ptr, size_t old_size,
                      size_t new_size) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(heap_contains(ctx, ptr));
    MCL_ASSERT(new_size > old_size);

    size_t delta = new_size - old_size;

    MCL_ASSERT(heap_space(ctx) >= delta);

    if ((ctx->heap_ptr - old_size) != ptr) {
        void* old_end = ptr + old_size;
        void* new_end = ptr + new_size;
        mcl_heap_type_t* stack_i = ctx->stack_ptr;
        mcl_heap_type_t* stack_e = ctx->stack_end;

        memmove(new_end, old_end, ctx->heap_ptr - old_end);

        while (stack_i < stack_e) {
            if (stack_i[0] > ptr && stack_i[0] < ctx->heap_ptr) {
                stack_i[0] += delta;
            }

            stack_i++;
        }
    }

    ctx->heap_ptr += delta;
}


// Shrinks existing allocation on heap, relocating any allocations above it.
static void heap_shrink(mcl_t* ctx, void* ptr, size_t old_size,
                        size_t new_size) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(heap_contains(ctx, ptr));
    MCL_ASSERT(new_size < old_size);

    size_t delta = old_size - new_size;

    if ((ctx->heap_ptr - old_size) != ptr) {
        void* old_end = ptr + old_size;
        void* new_end = ptr + new_size;
        mcl_heap_type_t* stack_i = ctx->stack_ptr;
        mcl_heap_type_t* stack_e = ctx->stack_end;

        memmove(new_end, old_end, ctx->heap_ptr - old_end);

        while (stack_i < stack_e) {
            if (stack_i[0] > ptr && stack_i[0] < ctx->heap_ptr) {
                stack_i[0] -= delta;
            }

            stack_i++;
        }
    }

    ctx->heap_ptr -= delta;
}


// Frees an existing allocation, relocating any allocations above it.
static void heap_free(mcl_t* ctx, void* ptr, size_t size) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(heap_contains(ctx, ptr));
    MCL_ASSERT(size > 0);
    (void) heap_shrink(ctx, ptr, size, 0);
}


// ======== EXCEPTION HANDLING ========

typedef void (* mcl_try_func_t)(mcl_t* ctx);

// Sets up try block and invokes given function, returns throw result or
// MCL_RESULT_OK if no exception was thrown from inside try block.
// If result is anything but MCL_RESULT_OK or MCL_RESULT_OUT_OF_MEMORY, an
// error message will be on top of stack.
static mcl_result_t except_try(mcl_t* ctx, mcl_try_func_t func) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(func);

    mcl_result_t result;
    mcl_heap_type_t* saved_stack_ptr;
    mcl_heap_type_t* saved_frame_ptr;
    jmp_buf* saved_jb;
    jmp_buf jb;

    // save stack state
    saved_stack_ptr = ctx->stack_ptr;
    saved_frame_ptr = ctx->frame_ptr;
    saved_jb = ctx->jb;

    // setup new long jump and invoke function
    result = (mcl_result_t) setjmp(jb);

    if (result == MCL_RESULT_OK) {
        ctx->jb = &jb;
        func(ctx);
    }

    // restore stack state
    ctx->jb = saved_jb;
    ctx->frame_ptr = saved_frame_ptr;
    ctx->stack_ptr = saved_stack_ptr;

    // TODO: Move error message to restored top

    return result;
}


// Throws an exception causing long jump back to last defined try block
// result cannot be MCL_RESULT_OK and if result is anything but
// MCL_RESULT_OUT_OF_MEMORY, an error message string is assumed to be on top
// of stack.
static noreturn void except_throw(mcl_t* ctx, mcl_result_t result) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(result != MCL_RESULT_OK);
    MCL_ASSERT(ctx->jb);

    // long jump back to last except_try invocation
    longjmp(*(ctx->jb), result);
}


// ======== DATA PACKING ========

static void pack_u16(void* dest, uint16_t value) {
    uint8_t* dest_u8 = dest;

    dest_u8[0] = value & 0xFF;
    dest_u8[1] = (value >> 8) & 0xFF;
}


static uint16_t unpack_u16(const void* src) {
    const uint8_t* src_u8 = src;
    return (uint16_t) src_u8[1] << 8 | src_u8[0];
}


// ======== OBJECT TYPES ========

#define MCL_OBJECT_TYPE_STRING          0
#define MCL_OBJECT_TYPE_TABLE           1


// ======== STRING OBJECT PRIMITIVES ========

#define MCL_STRING_FIELD_TYPE           0
#define MCL_STRING_FIELD_REF_COUNT      1
#define MCL_STRING_FIELD_LENGTH         2
#define MCL_STRING_FIELD_CHARS          4


typedef uint8_t mcl_string_t;


// Determines whether given pointer targets a string object
static bool is_string(const void* ptr) {
    return ((const mcl_string_t*) ptr)[MCL_STRING_FIELD_TYPE] ==
           MCL_OBJECT_TYPE_STRING;
}


// Gets a string object's reference count
static uint8_t string_ref_count(const mcl_string_t* str) {
    MCL_ASSERT(is_string(str));
    return str[MCL_STRING_FIELD_REF_COUNT];
}


// Gets a string object's length
static uint16_t string_len(const mcl_string_t* str) {
    MCL_ASSERT(is_string(str));
    return unpack_u16(&str[MCL_STRING_FIELD_LENGTH]);
}


// Gets a string object's characters
static char* string_chars(mcl_string_t* str) {
    MCL_ASSERT(is_string(str));
    return (char*) &str[MCL_STRING_FIELD_CHARS];
}


// Allocates new string object on heap
static mcl_string_t* string_alloc(mcl_t* ctx, uint16_t len) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(len <= MCL_MAX_STRING_LEN);

    size_t size;
    mcl_string_t* str;

    // check for sufficient heap space
    size = 5 + len;

    if ( size > heap_space(ctx) ) {
        except_throw(ctx, MCL_RESULT_OUT_OF_MEMORY); // no return
    }

    // allocate space for string
    str = heap_alloc(ctx, size);

    // initialise fields
    str[MCL_STRING_FIELD_TYPE] = MCL_OBJECT_TYPE_STRING;
    str[MCL_STRING_FIELD_REF_COUNT] = 1;
    pack_u16(&str[MCL_STRING_FIELD_LENGTH], len);
    str[MCL_STRING_FIELD_CHARS + len] = '\0';

    return str;
}


// Increments a string's reference count
static mcl_string_t* string_ref(mcl_t* ctx, mcl_string_t* str) {
    MCL_ASSERT(ctx);
    MCL_ASSERT(heap_contains(ctx, str));
    MCL_ASSERT(is_string(str));

    if ( str[MCL_STRING_FIELD_REF_COUNT] == UINT8_MAX ) {
        // TODO: throw ref count overflow exception
        abort();
    }

    str[MCL_STRING_FIELD_REF_COUNT]++;

    return str;
}


// Decrements a string's reference count and deallocates the string if it
// reaches zero.
static void string_unref(mcl_t* ctx, mcl_string_t* str) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(heap_contains(ctx, str));
    MCL_ASSERT(is_string(str));

    if ( str[MCL_STRING_FIELD_REF_COUNT] == 1 ) {
        heap_free(ctx, str, string_len(str) + 5);
    } else {
        str[MCL_STRING_FIELD_REF_COUNT]--;
    }
}


// ======== DEBUG FUNCTIONS ========

#ifdef MCL_DEBUG

#include <stdio.h>


static void hex_dump(const void* data, size_t n_bytes) {
    const uint8_t* data_i = data;
    const uint8_t* data_e = data_i + n_bytes;

    while(data_i < data_e) {
        const uint8_t* row_e = data_i + 16;

        printf("%p:  ", data_i);

        while(data_i < row_e) {
            if ( data_i < data_e ) {
                printf("%02X ", *data_i);
            } else {
                printf("   ");
            }

            data_i++;
        }

        printf("  |");
        data_i -= 16;

        while(data_i < row_e) {
            if ( data_i < data_e) {
                char c = (char) *data_i;

                if ( c >= ' ' && c <= '~' ) {
                    printf("%c", c);
                } else {
                    printf(".");
                }
            }

            data_i++;
        }

        printf("|\n");
    }
}


void mcl_debug_dump(const mcl_t* ctx) {
    printf("Frame Ptr: %p => stack[%lu]\n", ctx->frame_ptr,
           (mcl_heap_type_t*) ctx->frame_ptr - ctx->stack_ptr);

    printf("Stack (top first):\n\n");

    for (mcl_heap_type_t* i = ctx->stack_ptr; i < ctx->stack_end; i++) {
        void* ptr = *i;

        printf("%p [%lu]: %p ", i, i - ctx->stack_ptr, ptr);

        if (heap_contains(ctx, ptr)) {
//            printf("=> '%.*s' (ref count = %u, len = %u)",
//                   string_len(ptr),
//                   string_chars(ptr),
//                   string_ref_count(ptr),
//                   string_len(ptr));
        } else if (stack_contains(ctx, ptr)) {
            printf("=> stack[%lu]",
                   (mcl_heap_type_t*) ptr - ctx->stack_ptr);
        } else if ( ptr == ctx->stack_end ) {
            printf("=> stack end");
        }

        printf("\n");
    }

    printf("\n\nHeap:\n\n");
    hex_dump(ctx->heap_start, ctx->heap_ptr - ctx->heap_start);
    printf("\n");
}

#endif // MCL_DEBUG


// ======== CONTEXT ========

void ctx_construct(mcl_t* mcl) {

}


mcl_result_t mcl_init(mcl_t* mcl, mcl_heap_type_t* heap, size_t n_heap_entries,
                      void* user_data) {
    MCL_ASSERT(mcl);
    MCL_ASSERT(heap);

    mcl_result_t result;

    // validate arguments
    if (!mcl || !heap || n_heap_entries < MCL_MIN_HEAP_ENTRIES) {
        return MCL_RESULT_OUT_OF_MEMORY;
    }

    // initialise context
    memset(mcl, 0, sizeof(*mcl));
    mcl->user_data = user_data;
    mcl->heap_start = heap;
    mcl->heap_ptr = heap;
    mcl->stack_end = heap + n_heap_entries;
    mcl->stack_ptr = mcl->stack_end;
    mcl->frame_ptr = mcl->stack_end;
    mcl->jb = NULL;

#ifdef MCL_DEBUG
    mcl->tag = MCL_TAG;
#endif // MCL_DEBUG

    // construct environment
    result = except_try(mcl, ctx_construct);

    // untag context if construction failed
#ifdef MCL_DEBUG
    if (result != MCL_RESULT_OK) {
        mcl->tag = 0;
    }
#endif // MCL_DEBUG

    return result;
}


void* mcl_user_data(const mcl_t* ctx) {
    MCL_ASSERT_CTX(ctx);
    return ctx->user_data;
}
