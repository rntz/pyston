// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <cstdarg>
#include <dlfcn.h>

#include "codegen/unwinding.h"
#include "core/options.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/traceback.h"
#include "runtime/types.h"
#include "runtime/util.h"

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#define TOSTR(e) (str((e))->s.str().c_str()) // this is wrong, because of lifetime issues

namespace pyston {

// from http://www.nongnu.org/libunwind/man/libunwind(3).html
void showBacktrace() {
    unw_cursor_t cursor;
    unw_context_t uc;
    unw_word_t ip, sp;

    unw_getcontext(&uc);
    unw_init_local(&cursor, &uc);

    while (unw_step(&cursor) > 0) {
        unw_get_reg(&cursor, UNW_REG_IP, &ip);
        unw_get_reg(&cursor, UNW_REG_SP, &sp);
        printf("ip = %lx, sp = %lx\n", (long)ip, (long)sp);
    }
}

// https://monoinfinito.wordpress.com/series/exception-handling-in-c/ is a very useful resource

// auto handler_fn = (int (*)(int, int, uint64_t, void*, void*))pip.handler;
////handler_fn(1, 1 /* _UA_SEARCH_PHASE */, 0 /* exc_class */, NULL, NULL);
// handler_fn(2, 2 /* _UA_CLEANUP_PHASE */, 0 /* exc_class */, NULL, NULL);
// unw_set_reg(&cursor, UNW_REG_IP, 1);

// TODO testing:
// unw_resume(&cursor);

// PROBLEM: if I resume any function, I run the risk of clobbering the stack frame for unwindExc!
// so I'll need to find some way to store any information I need to resume unwinding.
// this is probably the reason why C++ malloc()s up an exception record for every throw

// what benefits are we gaining by reimplementing this much C++ implementation behavior?
// - can avoid "search phase"
//   not clear how much time this gains us.
//
// - incremental traceback generation
//   could we do this with RAII somehow?
//   have a destructor that, if we're unwinding inside it, adds a line to the traceback?
//   (would need to be guaranteed not to throw an exception.)
//
// - does throwing exceptions work across generators?

// what are our other options?
// - use return-code exceptions
// - some hack with RAII to do incremental tracebacks
// - avoid C++ exceptions entirely & use something like longjmp()/setjmp()

// What do I need in order to deal with ICs?
//
// TODO: what the hell happens if an exception occurs inside an inline cache?
// TODO: how even do inline caches work?

void raiseRaw(const ExcInfo& e) __attribute__((__noreturn__));
void raiseRaw(const ExcInfo& e) {
    // Should set these to None rather than null before getting here:
    assert(e.type && e.value && e.traceback);

    if (VERBOSITY("stacktrace")) {
        try {
            BoxedString* st = str(e.type);
            BoxedString* sv = str(e.value);
            printf("---- raiseRaw() called with %s: %s\n", st->s.str().c_str(), sv->s.str().c_str());
        } catch (ExcInfo e) {
            printf("---- raiseRaw() called and WTFed\n");
        }
    }

    // {
    //     static bool flag = 0;   // recursion flag
    //     if (!flag) {
    //         flag = 1;
    //         if (PyObject_HasAttrString(e.value, "magic_break")) {
    //             printf("MAGIC BREAK\n");
    //         }
    //         flag = 0;
    //     }
    // }

    throw e;
}

void raiseExc(Box* exc_obj) {
    raiseRaw(ExcInfo(exc_obj->cls, exc_obj, getTraceback()));
}

// Have a special helper function for syntax errors, since we want to include the location
// of the syntax error in the traceback, even though it is not part of the execution:
void raiseSyntaxError(const char* msg, int lineno, int col_offset, const std::string& file, const std::string& func) {
    Box* exc = runtimeCall(SyntaxError, ArgPassSpec(1), boxStrConstant(msg), NULL, NULL, NULL, NULL);

    auto tb = getTraceback();
    std::vector<const LineInfo*> entries = tb->lines;
    entries.push_back(new LineInfo(lineno, col_offset, file, func));
    raiseRaw(ExcInfo(exc->cls, exc, new BoxedTraceback(std::move(entries))));
}

void raiseSyntaxErrorHelper(const std::string& file, const std::string& func, AST* node_at, const char* msg, ...) {
    va_list ap;
    va_start(ap, msg);

    char buf[1024];
    vsnprintf(buf, sizeof(buf), msg, ap);


    // TODO I'm not sure that it's safe to raise an exception here, since I think
    // there will be things that end up not getting cleaned up.
    // Then again, there are a huge number of things that don't get cleaned up even
    // if an exception doesn't get thrown...

    // TODO output is still a little wrong, should be, for example
    //
    //  File "../test/tests/future_non_existent.py", line 1
    //    from __future__ import rvalue_references # should cause syntax error
    //
    // but instead it is
    //
    // Traceback (most recent call last):
    //  File "../test/tests/future_non_existent.py", line -1, in :
    //    from __future__ import rvalue_references # should cause syntax error
    raiseSyntaxError(buf, node_at->lineno, node_at->col_offset, file, "");
}

void _printStacktrace() {
    printTraceback(getTraceback());
}

// where should this go...
extern "C" void abort() {
    static void (*libc_abort)() = (void (*)())dlsym(RTLD_NEXT, "abort");

    // In case something calls abort down the line:
    static bool recursive = false;
    // If object_cls is NULL, then we somehow died early on, and won't be able to display a traceback.
    if (!recursive && object_cls) {
        recursive = true;

        fprintf(stderr, "Someone called abort!\n");

        // If we call abort(), things may be seriously wrong.  Set an alarm() to
        // try to handle cases that we would just hang.
        // (Ex if we abort() from a static constructor, and _printStackTrace uses
        // that object, _printStackTrace will hang waiting for the first construction
        // to finish.)
        alarm(1);
        try {
            _printStacktrace();
        } catch (ExcInfo) {
            fprintf(stderr, "error printing stack trace during abort()");
        }

        // Cancel the alarm.
        // This is helpful for when running in a debugger, since the debugger will catch the
        // abort and let you investigate, but the alarm will still come back to kill the program.
        alarm(0);
    }

    if (PAUSE_AT_ABORT) {
        printf("PID %d about to call libc abort; pausing for a debugger...\n", getpid());
        while (true) {
            sleep(1);
        }
    }
    libc_abort();
    __builtin_unreachable();
}

extern "C" void exit(int code) {
    static void (*libc_exit)(int) = (void (*)(int))dlsym(RTLD_NEXT, "exit");

    if (code == 0) {
        libc_exit(0);
        __builtin_unreachable();
    }

    fprintf(stderr, "Someone called exit with code=%d!\n", code);

    // In case something calls exit down the line:
    static bool recursive = false;
    if (!recursive) {
        recursive = true;

        _printStacktrace();
    }

    libc_exit(code);
    __builtin_unreachable();
}

extern "C" void raise0() {
    ExcInfo* exc_info = getFrameExcInfo();
    assert(exc_info->type);

    // TODO need to clean up when we call normalize, do_raise, etc
    if (exc_info->type == None)
        raiseExcHelper(TypeError, "exceptions must be old-style classes or derived from BaseException, not NoneType");

    raiseRaw(*exc_info);
}

#ifndef NDEBUG
ExcInfo::ExcInfo(Box* type, Box* value, Box* traceback) : type(type), value(value), traceback(traceback) {
    if (this->type && this->type != None)
        RELEASE_ASSERT(isSubclass(this->type->cls, type_cls), "throwing old-style objects not supported yet (%s)",
                       getTypeName(this->type));
}
#endif

void ExcInfo::printExcAndTraceback() const {
    PyErr_Display(type, value, traceback);
}

bool ExcInfo::matches(BoxedClass* cls) const {
    assert(this->type);
    RELEASE_ASSERT(isSubclass(this->type->cls, type_cls), "throwing old-style objects not supported yet (%s)",
                   getTypeName(this->type));
    return isSubclass(static_cast<BoxedClass*>(this->type), cls);
}

// void raise3_(Box* exc, Box* val, Box* tb) {
//     PyErr_NormalizeException(&exc, &val, &tb);
//     if (tb == None)
//         tb = getTraceback();
//     raiseRaw(ExcInfo(exc, val, tb));
// }

// takes the three arguments of a `raise' and produces the ExcInfo to throw
ExcInfo excInfoForRaise(Box* exc_cls, Box* exc_val, Box* exc_tb) {
    assert(exc_cls && exc_val && exc_tb); // use None for default behavior, not nullptr
    // TODO switch this to PyErr_Normalize

    // printf("orig: (%s, %s, %s)\n", TOSTR(exc_cls), TOSTR(exc_val), TOSTR(exc_tb));
    // TODO: theoretically, PyErr_NormalizeException could itself throw an exception
    // but it would do this by setting the error, CPython-style, not by throwing it C++-style
    // PyErr_NormalizeException(&exc_cls, &exc_val, &exc_tb);
    // printf("normalized: (%s, %s, %s)\n", TOSTR(exc_cls), TOSTR(exc_val), TOSTR(exc_tb));

    if (exc_tb == None)
        exc_tb = getTraceback();

    // now exc_cls is the type, exc_val the value, and exc_tb the traceback

    if (isSubclass(exc_cls->cls, type_cls)) {
        // printf("  NEW STYLE\n");
        BoxedClass* c = static_cast<BoxedClass*>(exc_cls);
        if (isSubclass(c, BaseException)) {
            Box* exc_obj;

            if (isSubclass(exc_val->cls, BaseException)) {
                exc_obj = exc_val;
                c = exc_obj->cls;
            } else if (exc_val != None) {
                exc_obj = runtimeCall(c, ArgPassSpec(1), exc_val, NULL, NULL, NULL, NULL);
            } else {
                exc_obj = runtimeCall(c, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
            }

            // printf("ExcInfo(%s, %s, %s)\n", TOSTR(c), TOSTR(exc_obj), TOSTR(exc_tb));
            return ExcInfo(c, exc_obj, exc_tb);
        }
    }

    if (isSubclass(exc_cls->cls, BaseException)) {
        // printf("  OLD STYLE\n");
        if (exc_val != None)
            raiseExcHelper(TypeError, "instance exception may not have a separate value");

        // printf("ExcInfo(%s, %s, %s)\n", TOSTR(exc_cls->cls), TOSTR(exc_cls), TOSTR(exc_tb));
        return ExcInfo(exc_cls->cls, exc_cls, exc_tb);
    }

    raiseExcHelper(TypeError, "exceptions must be old-style classes or derived from BaseException, not %s",
                   getTypeName(exc_cls));
}

extern "C" void raise3(Box* arg0, Box* arg1, Box* arg2) {
    raiseRaw(excInfoForRaise(arg0, arg1, arg2));
}

void raiseExcHelper(BoxedClass* cls, Box* arg) {
    Box* exc_obj = runtimeCall(cls, ArgPassSpec(1), arg, NULL, NULL, NULL, NULL);
    raiseExc(exc_obj);
}

void raiseExcHelper(BoxedClass* cls, const char* msg, ...) {
    if (msg != NULL) {
        va_list ap;
        va_start(ap, msg);

        // printf("Raising: ");
        // vprintf(msg, ap);
        // printf("\n");
        // va_start(ap, msg);

        char buf[1024];
        vsnprintf(buf, sizeof(buf), msg, ap);

        va_end(ap);

        BoxedString* message = boxStrConstant(buf);
        Box* exc_obj = runtimeCall(cls, ArgPassSpec(1), message, NULL, NULL, NULL, NULL);
        raiseExc(exc_obj);
    } else {
        Box* exc_obj = runtimeCall(cls, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        raiseExc(exc_obj);
    }
}
}
