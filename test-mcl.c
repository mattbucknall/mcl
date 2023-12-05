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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcl.c"


// ======== UNIT TEST MACROS/FUNCTIONS ========

#define UT_RNG_SEED           0x64D26934ul

#define UT_WRAP_STRINGIFY(x) #x
#define UT_STRINGIFY(x) UT_WRAP_STRINGIFY(x)

#define UT_ASSERT(expr) \
    do { if ( !(expr) ) { fprintf(stderr, \
    "%u: Assertion '" UT_STRINGIFY(expr) "' failed", \
    __LINE__); exit(EXIT_FAILURE); } } while(0)


static uint32_t m_ut_lfsr = UT_RNG_SEED;


static void ut_random_reset(void) {
    m_ut_lfsr = UT_RNG_SEED;
}


static uint32_t ut_random(void) {
    uint32_t value;

    value = 0;

    for (size_t i = 0; i < (sizeof(value) * 8); i++) {
        value <<= 1;

        if (m_ut_lfsr & 1) {
            m_ut_lfsr >>= 1;
            m_ut_lfsr ^= 0xB4BCD35Cul;
            value |= 1;
        } else {
            m_ut_lfsr >>= 1;
        }
    }

    return value;
}


// ======== SELF TESTS ========

static void test_ut_random(void) {
    int32_t bins[64];

    memset(bins, 0, sizeof(bins));

    for (int32_t i = 0; i < 65536; i++) {
        bins[ut_random() & 0x3F]++;
    }

    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 64; j++) {
            int32_t delta = bins[i] - bins[j];
            UT_ASSERT(delta >= -250 && delta <= 250);
        }
    }
}


// ======== TEST FIXTURE ========

#ifndef TEST_HEAP_SIZE
#define TEST_HEAP_SIZE          (MCL_MAX_STRING_LEN * 2)
#endif // TEST_HEAP_SIZE

#define TEST_DUMMY_DATA_SIZE    256


#define TEST_GUARD_1            0x914A55Eul
#define TEST_GUARD_2            0xE1F7FC6ul
#define TEST_GUARD_3            0x334BB23Eul


static unsigned int m_setup_count;
static unsigned int m_teardown_count;
static uint32_t m_guard_1 = TEST_GUARD_1;
static mcl_heap_type_t m_heap[TEST_HEAP_SIZE];
static uint32_t m_guard_2 = TEST_GUARD_2;
static mcl_t m_ctx;
static uint32_t m_guard_3 = TEST_GUARD_3;
static uint32_t m_user_data;
static char m_dummy_data[TEST_DUMMY_DATA_SIZE];


static void setup(void) {
    memset(&m_ctx, 0, sizeof(m_ctx));
    memset(m_heap, 0, sizeof(m_heap));
    m_user_data = 0;

    for (size_t i = 0; i < TEST_DUMMY_DATA_SIZE; i++) {
        m_dummy_data[i] = ("012345679ABCDEFGHIJKLMNOPQRSTUVWXYZ")
        [ut_random() % 36];
    }

    UT_ASSERT(mcl_init(&m_ctx, m_heap, TEST_HEAP_SIZE,
                       &m_user_data) == MCL_RESULT_OK);

    m_setup_count++;
}


static void teardown(void) {
    UT_ASSERT(m_guard_1 = TEST_GUARD_1);
    UT_ASSERT(m_guard_2 = TEST_GUARD_2);
    UT_ASSERT(m_guard_3 = TEST_GUARD_3);

    m_teardown_count++;
}


// ======== INITIALISATION TESTS ========

static void test_init(void) {
    setup();
    teardown();
}


static void test_init_user_data(void) {
    setup();
    UT_ASSERT(mcl_user_data(&m_ctx) == &m_user_data);
    teardown();
}


// ======== STACK PRIMITIVE TESTS ========

static void test_stack_round_up_ptr(void) {
    setup();

    for (uintptr_t i = 0; i < 256; i++) {
        uintptr_t expected;

        expected = (i + sizeof(void*) - 1) / sizeof(void*);
        expected *= sizeof(void*);

        UT_ASSERT(stack_round_up_ptr((void*) i) == (void*) expected);
    }

    teardown();
}


static void test_stack_space(void) {
    for (int i = 0; i < 100; i++) {
        setup();

        size_t initial_space = stack_space(&m_ctx);
        uint32_t n = ut_random() % initial_space;

        for (uint32_t j = 0; j < n; j++) {
            stack_push(&m_ctx, NULL);
        }

        UT_ASSERT(stack_space(&m_ctx) == (initial_space - n));

        teardown();
    }
}


static void test_stack_height(void) {
    for (int i = 0; i < 100; i++) {
        setup();

        uint32_t n = ut_random() % stack_space(&m_ctx);

        for (uint32_t j = 0; j < n; j++) {
            stack_push(&m_ctx, NULL);
        }

        UT_ASSERT(stack_height(&m_ctx) == n);

        teardown();
    }
}


static void test_stack_contains(void) {
    for (int i = 0; i < 100; i++) {
        setup();

        uint32_t space = stack_space(&m_ctx);
        uint32_t n = ut_random() % space;

        for (uint32_t j = 1; j <= n; j++) {
            stack_push(&m_ctx, NULL);
        }

        for (uint32_t j = 1; j <= space; j++) {
            mcl_heap_type_t* ptr = m_ctx.stack_end - j;
            UT_ASSERT(stack_contains(&m_ctx, ptr) == (j <= n));
        }

        teardown();
    }
}


static void test_stack_contains_null(void) {
    setup();
    UT_ASSERT(stack_contains(&m_ctx, NULL) == false);
    teardown();
}


static void test_stack_push_pop(void) {
    setup();

    size_t space = stack_space(&m_ctx);

    for (uintptr_t i = 0; i < space; i++) {
        stack_push(&m_ctx, (void*) i);
    }

    for (uintptr_t i = 0; i < space; i++) {
        UT_ASSERT(m_ctx.stack_ptr[0] == (void*) (space - i - 1));
        stack_pop(&m_ctx);
    }

    teardown();
}


static void test_stack_pop_n(void) {
    setup();

    for (int i = 0; i < 100; i++) {
        uint32_t n = ut_random() % stack_space(&m_ctx);

        for (uint32_t j = 0; j < n; j++) {
            stack_push(&m_ctx, NULL);
        }

        stack_pop_n(&m_ctx, n);
        UT_ASSERT(stack_height(&m_ctx) == 0);
    }

    teardown();
}


static void test_stack_swap(void) {
    setup();

    size_t half_space = stack_space(&m_ctx) / 2;
    size_t space = half_space * 2;

    for (uintptr_t i = 0; i < space; i++) {
        stack_push(&m_ctx, (void*) i);
    }

    for (uintptr_t i = 0; i < half_space; i++) {
        stack_swap(&m_ctx.stack_ptr[i],
                   &m_ctx.stack_ptr[space - i - 1]);
    }

    for (uintptr_t i = 0; i < space; i++) {
        UT_ASSERT(m_ctx.stack_ptr[i] == (void*) i);
    }

    teardown();
}


// ======== HEAP PRIMITIVE TESTS ========

static void test_heap_space_alloc(void) {
    for (int i = 0; i < 100; i++) {
        size_t space;
        size_t size;

        setup();

        space = heap_space(&m_ctx);
        size = ut_random() % space;

        heap_alloc(&m_ctx, size);

        UT_ASSERT(heap_space(&m_ctx) == (space - size));

        teardown();
    }
}


static void test_heap_grow(void) {
    for (int i = 0; i < 100; i++) {
        size_t size[2];
        size_t new_size;

        setup();

        for (int j = 0; j < 2; j++) {
            do {
                size[j] = ut_random() % (TEST_DUMMY_DATA_SIZE / 2);
            } while(size[j] == 0);

            stack_push(&m_ctx, heap_alloc(&m_ctx, size[j]));
            memcpy(m_ctx.stack_ptr[0], m_dummy_data, size[j]);
        }

        do {
            new_size = size[0] + (ut_random() % (TEST_DUMMY_DATA_SIZE / 2));
        } while(new_size == size[0]);

        heap_grow(&m_ctx, m_ctx.stack_ptr[1],
                  size[0], new_size);

        memcpy(m_ctx.stack_ptr[1], m_dummy_data, new_size);

        UT_ASSERT(memcmp(m_ctx.stack_ptr[0],
                         m_dummy_data, size[1]) == 0);

        teardown();
    }
}


static void test_heap_shrink(void) {
    for (int i = 0; i < 100; i++) {
        size_t size[2];
        size_t new_size;

        setup();

        for (int j = 0; j < 2; j++) {
            do {
                size[j] = ut_random() % (TEST_DUMMY_DATA_SIZE / 2);
            } while(size[j] < 2);

            stack_push(&m_ctx, heap_alloc(&m_ctx, size[j]));
            memcpy(m_ctx.stack_ptr[0], m_dummy_data, size[j]);
        }

        do {
            new_size = size[0] - (ut_random() % size[0]);
        } while(new_size == size[0]);

        heap_shrink(&m_ctx, m_ctx.stack_ptr[1],
                  size[0], new_size);

        UT_ASSERT(memcmp(m_ctx.stack_ptr[0],
                         m_dummy_data, size[1]) == 0);

        teardown();
    }
}


static void test_heap_free(void) {
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 10; j++) {
            size_t size[10];
            size_t space;

            setup();

            for (int k = 0; k < 10; k++) {
                do {
                    size[k] = ut_random() % (TEST_DUMMY_DATA_SIZE / 10);
                } while (size[k] < 1);

                stack_push(&m_ctx,
                           heap_alloc(&m_ctx, size[k]));

                memcpy(m_ctx.stack_ptr[0], m_dummy_data, size[k]);
            }

            space = heap_space(&m_ctx);
            heap_free(&m_ctx, m_ctx.stack_ptr[j], size[9-j]);
            UT_ASSERT(space == (heap_space(&m_ctx) - size[9-j]));

            for (int k = 0; k < 10; k++) {
                if ( k == j ) {
                    continue;
                }

                UT_ASSERT(memcmp(m_ctx.stack_ptr[k],
                                 m_dummy_data, size[9-k]) == 0);
            }

            teardown();
        }
    }
}


// ======== EXCEPTION HANDLING TESTS ========

static void test_except_helper_no_throw(mcl_t* ctx) {
    (void) ctx;

    uint32_t* data = mcl_user_data(&m_ctx);
    *data = 1;
}


static void test_except_no_throw(void) {
    setup();

    uint32_t* data = mcl_user_data(&m_ctx);

    UT_ASSERT(except_try(&m_ctx, test_except_helper_no_throw) ==
            MCL_RESULT_OK);

    UT_ASSERT(*data == 1);

    teardown();
}


static void test_except_helper_throw_out_of_memory(mcl_t* ctx) {
    stack_push(&m_ctx, NULL);
    except_throw(&m_ctx, MCL_RESULT_OUT_OF_MEMORY);
}


static void test_except_throw_out_of_memory(void) {
    size_t height;

    setup();

    height = stack_height(&m_ctx);

    UT_ASSERT(except_try(&m_ctx,
                         test_except_helper_throw_out_of_memory) ==
                         MCL_RESULT_OUT_OF_MEMORY);

    UT_ASSERT(stack_height(&m_ctx) == height);

    teardown();
}


// ======== DATA PACKING TESTS ========

static void test_pack_unpack_u16(void) {
    setup();

    uint8_t bytes[3];

    for (uint32_t i = 0; i <= UINT16_MAX; i++) {
        pack_u16(&bytes[0], i);
        UT_ASSERT(unpack_u16(&bytes[0]) == i);
    }

    for (uint32_t i = 0; i <= UINT16_MAX; i++) {
        pack_u16(&bytes[1], i);
        UT_ASSERT(unpack_u16(&bytes[1]) == i);
    }

    teardown();
}


// ======== STRING PRIMITIVE TESTS ========

static void test_string_alloc(void) {
    for (uint32_t len = 0; len < MCL_MAX_STRING_LEN; len += 17) {
        setup();

        mcl_string_t* str = string_alloc(&m_ctx, len);
        UT_ASSERT(string_ref_count(str) == 1);
        UT_ASSERT(string_len(str) == len);
        UT_ASSERT(string_chars(str)[len] == '\0');

        teardown();
    }
}


static void test_string_ref_unref(void) {
    size_t space;
    mcl_string_t* str;

    setup();

    space = heap_space(&m_ctx);
    str = string_alloc(&m_ctx, 100);
    UT_ASSERT(heap_space(&m_ctx) < space);

    for (int i = 0; i < 254; i++) {
        UT_ASSERT(string_ref(&m_ctx, str) == str);
    }

    for (int i = 0; i < 255; i++) {
        string_unref(&m_ctx, str);
    }

    UT_ASSERT(heap_space(&m_ctx) == space);

    teardown();
}


// ======== TEST RUNNER ========

int main(int argc, char* argv[]) {
    (void) argc;
    (void) argv;

    test_ut_random();

    test_init();
    test_init_user_data();

    test_stack_round_up_ptr();
    test_stack_space();
    test_stack_height();
    test_stack_contains();
    test_stack_contains_null();
    test_stack_push_pop();
    test_stack_pop_n();
    test_stack_swap();

    test_heap_space_alloc();
    test_heap_grow();
    test_heap_shrink();
    test_heap_free();

    test_except_no_throw();
    test_except_throw_out_of_memory();

    test_pack_unpack_u16();

    test_string_alloc();
    test_string_ref_unref();

    UT_ASSERT(m_setup_count == m_teardown_count);

    printf("All tests passed\n");

    return EXIT_SUCCESS;
}
