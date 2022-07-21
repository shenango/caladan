#pragma once

#include <dlfcn.h>
#include <base/assert.h>

#define NOTSELF_NOARG(retType, name)                                           \
        if (unlikely(!__self)) {                                               \
                static retType (*fn)(void);                                    \
                if (!fn) {                                                     \
                        fn = dlsym(RTLD_NEXT, name);                           \
                        BUG_ON(!fn);                                           \
                }                                                              \
                return fn();                                                   \
        }

#define NOTSELF_1ARG(retType, name, arg)                                       \
        if (unlikely(!__self)) {                                               \
                static retType (*fn)(typeof(arg));                             \
                if (!fn) {                                                     \
                        fn = dlsym(RTLD_NEXT, name);                           \
                        BUG_ON(!fn);                                           \
                }                                                              \
                return fn(arg);                                                \
        }

#define NOTSELF_2ARG(retType, name, arg1, arg2)                                \
        if (unlikely(!__self)) {                                               \
                static retType (*fn)(typeof(arg1), typeof(arg2));              \
                if (!fn) {                                                     \
                        fn = dlsym(RTLD_NEXT, name);                           \
                        BUG_ON(!fn);                                           \
                }                                                              \
                return fn(arg1, arg2);                                         \
        }

#define NOTSELF_3ARG(retType, name, arg1, arg2, arg3)                          \
        if (unlikely(!__self)) {                                               \
                static retType (*fn)(typeof(arg1), typeof(arg2),               \
                                     typeof(arg3));                            \
                if (!fn) {                                                     \
                        fn = dlsym(RTLD_NEXT, name);                           \
                        BUG_ON(!fn);                                           \
                }                                                              \
                return fn(arg1, arg2, arg3);                                   \
        }

#define NOTSELF_4ARG(retType, name, arg1, arg2, arg3, arg4)                    \
        if (unlikely(!__self)) {                                               \
                static retType (*fn)(typeof(arg1), typeof(arg2),               \
                                     typeof(arg3), typeof(arg4));              \
                if (!fn) {                                                     \
                        fn = dlsym(RTLD_NEXT, name);                           \
                        BUG_ON(!fn);                                           \
                }                                                              \
                return fn(arg1, arg2, arg3, arg4);                             \
        }
