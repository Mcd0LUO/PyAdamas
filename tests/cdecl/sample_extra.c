#include "sample_extra.h"

int extra_twice(int x) { return SAMPLE_SCALE(x); }

int extra_call(sample_cb cb, int x) { return cb(x, "hi"); }

static int extra_dummy;

opaque_handle *extra_open(void) { return (opaque_handle *)&extra_dummy; }

named_pair extra_make_pair(int a, int b) {
    named_pair p = {a, b};
    return p;
}
