/* SPDX-License-Identifier: LGPL-3.0-only
 * Run each scenario in a fresh process: production failure latch has no reset. */
#include "entropy.h"
#include <bsp_api.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

static unsigned init_calls, read_calls, fail_read;
static int fail_init;
fsp_err_t HW_SCE_McuSpecificInit(void) {
    ++init_calls;
    return fail_init ? FSP_ERR_INTERNAL : FSP_SUCCESS;
}
fsp_err_t HW_SCE_RNG_Read(uint32_t *out) {
    assert((uintptr_t)out % sizeof(uint32_t) == 0);
    ++read_calls;
    /* Write even on error, exercising rejection of partial vendor output. */
    uint8_t *bytes = (uint8_t *)out;
    for (unsigned i = 0; i < 16; ++i) bytes[i] = (uint8_t)(16*read_calls + i);
    return read_calls == fail_read ? FSP_ERR_INTERNAL : FSP_SUCCESS;
}
static void check_failure(void) {
    uint8_t storage[35];
    memset(storage, 0xa5, sizeof(storage));
    assert(hdt_ra4m1_entropy(storage+1, 33) == -1);
    assert(storage[0] == 0xa5 && storage[34] == 0xa5);
    for (unsigned i = 1; i < 34; ++i) assert(storage[i] == 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(hdt_ra4m1_entropy(NULL, 0) == 0);
    assert(init_calls == 0 && read_calls == 0);
    if (!strcmp(argv[1], "success")) {
        const size_t sizes[] = {1, 15, 16, 17, 31, 32, 33};
        for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
            uint8_t storage[35];
            memset(storage, 0xa5, sizeof(storage));
            unsigned first = read_calls + 1;
            assert(hdt_ra4m1_entropy(storage+1, sizes[i]) == 0);
            assert(init_calls == 1 && read_calls == first-1+(sizes[i]+15)/16);
            assert(storage[0] == 0xa5);
            for (size_t j = 0; j < sizes[i]; ++j)
                assert(storage[j+1] == (uint8_t)(16*first+j));
            for (size_t j = sizes[i]+1; j < sizeof(storage); ++j)
                assert(storage[j] == 0xa5);
        }
    } else if (!strcmp(argv[1], "null")) {
        assert(hdt_ra4m1_entropy(NULL, 1) == -1);
        assert(init_calls == 0 && read_calls == 0);
        uint8_t byte;
        assert(hdt_ra4m1_entropy(&byte, 1) == 0);
        assert(init_calls == 1 && read_calls == 1);
    } else {
        if (!strcmp(argv[1], "init_failure")) fail_init = 1;
        else if (!strcmp(argv[1], "first_read_failure")) fail_read = 1;
        else { assert(!strcmp(argv[1], "later_read_failure")); fail_read = 2; }
        check_failure();
        assert(init_calls == 1 && read_calls == fail_read);
        unsigned calls = read_calls;
        check_failure();
        assert(init_calls == 1 && read_calls == calls);
        assert(hdt_ra4m1_entropy(NULL, 0) == 0);
    }
    return 0;
}
