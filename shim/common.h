#pragma once

#include <dlfcn.h>
#include <base/assert.h>

#define NOTSELF(name, ...)                                                     \
        if (unlikely(!__self)) {                                               \
                static typeof(name) *fn;                                       \
                if (!fn) {                                                     \
                        fn = dlsym(RTLD_NEXT, #name);                          \
                        BUG_ON(!fn);                                           \
                }                                                              \
                return fn(__VA_ARGS__);                                        \
        }
