/*
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


// ======== FORWARD DECLARATIONS ========

static noreturn void except_throw(mcl_t* ctx, mcl_result_t result);


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


// Pops entry from stack, returning its value
static void* stack_pop(mcl_t* ctx) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(stack_height(ctx) > 0);

    void* ptr;

    ptr = ctx->stack_ptr[0];
    ctx->stack_ptr += 1;
    return ptr;
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


// ======== DATA PACKING ========

// Writes unsigned 16-bit integer at arbitrary memory address without
// violating alignment rules.
static void pack_u16(void* dest, uint16_t value) {
    uint8_t* dest_u8 = dest;

    dest_u8[0] = value & 0xFF;
    dest_u8[1] = (value >> 8) & 0xFF;
}


// Reads unsigned 16-bit integer from arbitrary memory address without
// violating alignment rules.
static uint16_t unpack_u16(const void* src) {
    const uint8_t* src_u8 = src;
    return (uint16_t) src_u8[1] << 8 | src_u8[0];
}


// ======== STRING OBJECT PRIMITIVES ========

#define MCL_STRING_FIELD_REF_COUNT      0
#define MCL_STRING_FIELD_LENGTH         1
#define MCL_STRING_FIELD_CHARS          3


typedef uint8_t mcl_string_t;


// Calculates space required for string object with given length
static size_t string_size(uint16_t len) {
    return 4 + (size_t) len;
}


// Gets a string object's reference count
static uint8_t string_ref_count(const mcl_string_t* str) {
    return str[MCL_STRING_FIELD_REF_COUNT];
}


// Gets a string object's length
static uint16_t string_len(const mcl_string_t* str) {
    return unpack_u16(&str[MCL_STRING_FIELD_LENGTH]);
}


// Gets a string object's characters
static char* string_chars(mcl_string_t* str) {
    return (char*) &str[MCL_STRING_FIELD_CHARS];
}


// Emplaces string object in given allocation
static void string_emplace(mcl_t* ctx, mcl_string_t* str,
                                    uint16_t len) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(heap_contains(ctx, str));
    MCL_ASSERT(len <= MCL_MAX_STRING_LEN);

    // initialise fields
    str[MCL_STRING_FIELD_REF_COUNT] = 1;
    pack_u16(&str[MCL_STRING_FIELD_LENGTH], len);
    str[MCL_STRING_FIELD_CHARS + len] = '\0';
}



// Allocates new string object on heap
static mcl_string_t* string_alloc(mcl_t* ctx, uint16_t len) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(len <= MCL_MAX_STRING_LEN);

    // check for sufficient heap space
    size_t size = string_size(len);

    if ( size > heap_space(ctx) ) {
        except_throw(ctx, MCL_RESULT_OUT_OF_MEMORY); // no return
    }

    // allocate space for string object
    mcl_string_t* str = heap_alloc(ctx, size);

    // emplace string object in allocation
    string_emplace(ctx, str, len);

    return str;
}


// Increments a string's reference count
static mcl_string_t* string_ref(mcl_t* ctx, mcl_string_t* str) {
    MCL_ASSERT(ctx);
    MCL_ASSERT(heap_contains(ctx, str));

    // check incrementing reference count won't cause it to overflow
    if ( str[MCL_STRING_FIELD_REF_COUNT] == UINT8_MAX ) {
        // TODO: throw ref count overflow exception
        abort();
    }

    // increment reference count
    str[MCL_STRING_FIELD_REF_COUNT]++;

    return str;
}


// Decrements a string's reference count and deallocates the string if it
// reaches zero.
static void string_unref(mcl_t* ctx, mcl_string_t* str) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(heap_contains(ctx, str));

    // decrement ref count or free string object if decrementing count would
    // make it zero
    if ( str[MCL_STRING_FIELD_REF_COUNT] == 1 ) {
        heap_free(ctx, str, string_size(string_len(str)));
    } else {
        str[MCL_STRING_FIELD_REF_COUNT]--;
    }
}


// Expands a string object's capacity
static void string_grow(mcl_t* ctx, mcl_string_t* str, uint16_t new_len) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(heap_contains(ctx, str));
    MCL_ASSERT(new_len <= MCL_MAX_STRING_LEN);
    MCL_ASSERT(new_len > string_len(str));

    // calculate difference between current length and new length
    size_t current_len = string_len(str);
    size_t delta = new_len - current_len;

    // check there is space to expand string
    if ( heap_space(ctx) < delta ) {
        except_throw(ctx, MCL_RESULT_OUT_OF_MEMORY); // no return
    }

    // grow string object's allocation
    heap_grow(ctx, str, string_size(current_len),
              string_size(new_len));

    // update string object's length field
    str[MCL_STRING_FIELD_LENGTH] = new_len;

    // null-terminate
    string_chars(str)[new_len] = '\0';
}


// Reduces a string object's capacity
static void string_shrink(mcl_t* ctx, mcl_string_t* str, uint16_t new_len) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(heap_contains(ctx, str));
    MCL_ASSERT(new_len <= MCL_MAX_STRING_LEN);
    MCL_ASSERT(new_len < string_len(str));

    // calculate difference between current length and new length
    size_t current_len = string_len(str);

    // shrink string object's allocation
    heap_shrink(ctx, str, string_size(current_len),
                string_size(new_len));

    // update string object's length field
    str[MCL_STRING_FIELD_LENGTH] = new_len;

    // null-terminate
    string_chars(str)[new_len] = '\0';
}


// creates new string with given contents and length
static mcl_string_t* string_new_with_len(mcl_t* ctx, const char* content,
                                         uint16_t len) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(content || len == 0);
    MCL_ASSERT(len <= MCL_MAX_STRING_LEN);

    // allocate string object
    mcl_string_t* str = string_alloc(ctx, len);

    // copy contents to string object
    memcpy(string_chars(str), content, len);

    return str;
}


// creates a string with given null-terminated contents
static mcl_string_t* string_new(mcl_t* ctx, const char* contents) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(contents);

    // find length of content
    size_t len = strlen(contents);

    // check length does not exceed limit
    if ( len > MCL_MAX_STRING_LEN ) {
        // TODO: throw string too long exception
        abort();
    }

    // create string object
    return string_new_with_len(ctx, contents, (uint16_t) len);
}


static int string_compare(uint8_t* a, uint8_t* b) {
    MCL_ASSERT(a);
    MCL_ASSERT(b);

    uint16_t len_a = string_len(a);
    uint16_t len_b = string_len(b);
    const char* chars_a = string_chars(a);
    const char* chars_b = string_chars(b);

    for (size_t i = 0; i < len_a && i < len_b; i++) {
        if ( chars_a[i] < chars_b[i] ) {
            return -1;
        } else if ( chars_a[i] > chars_b[i] ) {
            return 1;
        }
    }

    if (len_a < len_b) {
        return -1;
    } else if ( len_a > len_b ) {
        return 1;
    } else {
        return 0;
    }
}


// ======== EXCEPTION HANDLING ========

typedef void (* mcl_try_func_t)(mcl_t* ctx);

// Sets up try block and invokes protected function, returns throw result or
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

    // save existing long jump
    saved_jb = ctx->jb;

    // save frame state
    saved_stack_ptr = ctx->stack_ptr;
    saved_frame_ptr = ctx->frame_ptr;

    // setup new long jump
    ctx->jb = &jb;
    result = (mcl_result_t) setjmp(jb);

    // invoke protected function, handle exception if thrown
    if (result == MCL_RESULT_OK) {
        func(ctx);
    } else {
        // unwind stack
        // TODO: Move error message to restored top

        while(ctx->stack_ptr < saved_stack_ptr) {
            void* ptr = stack_pop(ctx);

            if (heap_contains(ctx, ptr)) {
                string_unref(ctx, ptr);
            }
        }

        // restore frame pointer
        ctx->frame_ptr = saved_frame_ptr;
    }

    // restore previous long jump
    ctx->jb = saved_jb;

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


// ======== FRAME PRIMITIVES ========

// Pushes next frame onto stack and updates frame pointer
static void frame_push(mcl_t* ctx) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(stack_space(ctx) >= 2);

    // check for adequate space on stack for new frame
    if ( stack_space(ctx) < 2 ) {
        except_throw(ctx, MCL_RESULT_OUT_OF_MEMORY); // no return
    }

    // build new frame
    stack_push(ctx, ctx->frame_ptr);
    ctx->frame_ptr = ctx->stack_ptr - 1;
    stack_push(ctx, ctx->frame_ptr);
}


// Unwinds current frame from stack and restores frame pointer to target
// previous frame.
static void frame_pop(mcl_t* ctx) {
    MCL_ASSERT_CTX(ctx);
    MCL_ASSERT(stack_height(ctx) >= 2);

    while(ctx->stack_ptr <= ctx->frame_ptr) {
        void* ptr = stack_pop(ctx);

        if ( heap_contains(ctx, ptr) ) {
            string_unref(ctx, ptr);
        }
    }

    // restore previous frame
    ctx->frame_ptr = stack_pop(ctx);
}


// Finds address of frame at given level. If level is positive, the frame is
// located from the top down (zero being the current frame). If level is
// negative, the frame is located from the bottom up, -1 being the bottom most
// frame. Returns NULL if level is out-of-range.
static mcl_heap_type_t* frame_seek(mcl_t* ctx, int level) {
    mcl_heap_type_t* frame = ctx->frame_ptr;

    // determine search direction
    if ( level > 0 ) {
        // traverse frames top down to specified level
        do {
            frame = frame[1];

            if ( frame == ctx->stack_end ) {
                return NULL;
            }

            level--;
        } while(level > 0);
    } else if ( level < 0 ) {
        mcl_heap_type_t* saved_stack_ptr = ctx->stack_ptr;

        // traverse frames top to bottom, pushing their positions onto the
        // stack to form a frame poiner list
        do {
            // check there is space on stack for frame position
            if ( stack_space(ctx) < 1 ) {
                except_throw(ctx, MCL_RESULT_OUT_OF_MEMORY); // no return
            }

            // push frame position, advance to next frame
            stack_push(ctx, frame);
            frame = frame[1];
        } while(frame < ctx->stack_end);

        // get frame pointer at requested level from list
        level = -1 - level;

        if ( level >= (saved_stack_ptr - ctx->stack_ptr) ) {
            frame = NULL;
        } else {
            frame = ctx->stack_ptr[level];
        }

        // discard list
        ctx->stack_ptr = saved_stack_ptr;
    }

    return frame;
}


// ======== DEBUG FUNCTIONS ========

#ifdef MCL_DEBUG

#include <stdio.h>


// writes canonical hex dump of given data to stdout
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


// writes stack and heap state to stdout
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

#define MCL_CTX_N_INITIAL_FRAMES        2


void ctx_construct(mcl_t* ctx) {
    // create procedure table frame
    frame_push(ctx);

    // create global table frame
    frame_push(ctx);
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
