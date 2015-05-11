#include <cstdlib>
#include <stdint.h>
#include <stddef.h>
#include <unwind.h>
#include <dlfcn.h> // dladdr

#include "llvm/Support/LEB128.h" // for {U,S}LEB128 decoding

#include "core/util.h"               // Timer
#include "core/stats.h"              // StatCounter
#include "core/types.h"              // for ExcInfo
#include "codegen/ast_interpreter.h" // interpreter_instr_addr
#include "runtime/generator.h"       // generatorEntry
#include "codegen/unwinding.h"       // getCFForAddress

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#define PYSTON_CUSTOM_UNWINDER 1 // set to 0 to use C++ unwinder

// %rax is used to store a pointer to the exception in-between various ABI calls. We don't use it, since the exception
// %is always in a known location, so we put a poison value there instead.
#define RAX_POISON_VALUE 0xdeadbeef

#define NORETURN __attribute__((__noreturn__))

// An action of 0 in the LSDA action table indicates cleanup.
#define CLEANUP_ACTION 0

// Dwarf encoding modes.
#define DW_EH_PE_absptr         0x00
#define DW_EH_PE_omit           0xff

#define DW_EH_PE_uleb128        0x01
#define DW_EH_PE_udata2         0x02
#define DW_EH_PE_udata4         0x03
#define DW_EH_PE_udata8         0x04
#define DW_EH_PE_sleb128        0x09
#define DW_EH_PE_sdata2         0x0A
#define DW_EH_PE_sdata4         0x0B
#define DW_EH_PE_sdata8         0x0C
#define DW_EH_PE_signed         0x08

#define DW_EH_PE_pcrel          0x10
#define DW_EH_PE_textrel        0x20
#define DW_EH_PE_datarel        0x30
#define DW_EH_PE_funcrel        0x40
#define DW_EH_PE_aligned        0x50

#define DW_EH_PE_indirect	0x80

// TODO: given that we're reimplementing a bunch of the C++ ABI, what happens if `new' fails, for example?
// we're assuming we only ever throw Python/Pyston exceptions, but that might be a bad assumption!

// The parts of the C++ unwinding things that I can't fuck with, because they get hard-coded into the emitted program:
//
// - on resuming an exception handler or cleanup code via a landing pad:
//       %rax contains the exception pointer
//       %rdx contains the switch value indicating which handler was invoked (or 0, for cleanup)
//
//   %rax is treated opaquely, and is only passed to __cxa_begin_catch, _Unwind_Resume & co
//
// - the landingpad code will call __cxa_begin_catch, __cxa_end_catch, and _Unwind_Resume
//   as appropriate. (How do I know they won't get inlined? That would be bad.)
//
//   it will also (depending on your compiler version! TODO: document) call __cxa_get_exception_ptr, if the exception is
//   caught by-value rather than by-reference (which we always do)
//
// TODO: clearer picture of all the steps here and what order they happen in so reader can follow this code

extern "C" void __gxx_personality_v0(); // wrong type signature, but that's ok, it's extern "C"

// check(EXPR) is like assert((EXPR) == 0), but evaluates EXPR even in debug mode.
template <typename T> static inline void check(T x) {
    assert(x == 0);
}

// TODO: This file uses uint64_t liberally. These should probably be changed to uintptr_t, for 32-bit compatibility. The
// exception is dereferences of uint64_t*s to read stuff out of an LSDA - we don't use that as of this writing, but we
// might in future to support DW_EH_PE_udata8.


namespace pyston {

thread_local ExcInfo exception_ferry(nullptr, nullptr, nullptr);

// Timer that auto-logs.
struct LogTimer {
    StatCounter& counter;
    Timer timer;

    LogTimer(const char *desc, StatCounter& ctr, long min_usec = -1) : counter(ctr), timer(desc, min_usec) {}
    ~LogTimer() { counter.log(timer.end()); }
};

static StatCounter us_unwind_loop("us_unwind_loop");
static StatCounter us_unwind_resume_catch("us_unwind_resume_catch");
static StatCounter us_unwind_cleanup("us_unwind_cleanup");
static StatCounter us_unwind_get_proc_info("us_unwind_get_proc_info");
static StatCounter us_unwind_step("us_unwind_step");                   // TODO
static StatCounter us_unwind_find_call_site_entry("us_unwind_find_call_site_entry");

// do these need to be separate timers? might as well
static thread_local Timer per_thread_resume_catch_timer(-1);
static thread_local Timer per_thread_cleanup_timer(-1);
#ifndef NDEBUG
static __thread bool in_cleanup_code = false;
#endif

extern "C" {

static NORETURN void panic(void) {
    RELEASE_ASSERT(0, "pyston::panic() called!");
}

// Highly useful resource: http://www.airs.com/blog/archives/464
// talks about DWARF LSDA parsing with respect to C++ exception-handling
//
// TODO: document this structure & the things it points to
struct lsda_info_t {
    // base which landing pad offsets are relative to
    const uint8_t *landing_pad_base;
    const uint8_t *type_table;
    const uint8_t *call_site_table;
    const uint8_t *action_table;
    uint8_t type_table_entry_encoding;      // a DW_EH_PE_xxx value
    uint8_t call_site_table_entry_encoding; // a DW_EH_PE_xxx value
};

struct call_site_entry_t {
    const uint8_t *instrs_start;
    size_t instrs_len_bytes;
    const uint8_t *landing_pad;    // may be NULL if no landing pad
    // "plus one" so that 0 can mean "no action". offset is in bytes.
    uint64_t action_offset_plus_one;
};


// ---------- Parsing stuff ----------
static inline void parse_lsda_header(const unw_proc_info_t* pip, lsda_info_t* info) {
    const uint8_t *ptr = (const uint8_t*) pip->lsda;

    // 1. Read the landing pad base pointer.
    uint8_t landing_pad_base_encoding = *ptr++;
    if (landing_pad_base_encoding == DW_EH_PE_omit) {
        // The common case is to omit. Then the landing pad base is _Unwind_GetRegion(context), which is the start of
        // the function.
        info->landing_pad_base = (const uint8_t*) pip->start_ip;
    } else {
        RELEASE_ASSERT(0, "we only support omitting the landing pad base");
    }

    // 2. Read the type table encoding & base pointer.
    info->type_table_entry_encoding = *ptr++;
    if (info->type_table_entry_encoding != DW_EH_PE_omit) {
        // read ULEB128-formatted byte offset from THIS FIELD (?) to the start of the types table.
        unsigned uleb_size;
        uint64_t offset = llvm::decodeULEB128(ptr, &uleb_size);
        // We don't use the type table, and I'm not sure this calculation is correct - it might be an offset from a
        // different base, I should use gdb to check it against libgcc. So I've set it to nullptr instead.
        info->type_table = nullptr;
        // info->type_table = ptr + offset; // <- The calculation I'm not sure of.
        ptr += uleb_size;
    } else {                    // type table omitted
        info->type_table = nullptr;
    }

    // 3. Read the call-site encoding & base pointer.
    info->call_site_table_entry_encoding = *ptr++;
    unsigned uleb_size;
    uint64_t call_site_table_nbytes = llvm::decodeULEB128(ptr, &uleb_size);
    ptr += uleb_size;

    // The call site table follows immediately after the header.
    info->call_site_table = ptr;
    // The action table follows immediately after the call site table.
    info->action_table = ptr + call_site_table_nbytes;
}

__attribute__((__always_inline__))
static inline const uint8_t *parse_call_site_entry(const uint8_t *ptr, const lsda_info_t *info,
                                                   call_site_entry_t *entry) {
    uint64_t instrs_start_offset, instrs_len_bytes, landing_pad_offset, action_offset_plus_one;

    // TODO: think about how this whole file should work on 32-bit platforms!
    // g++ recently changed from always doing udata4 here to using uleb128
    unsigned uleb_size;
    if (DW_EH_PE_uleb128 == info->call_site_table_entry_encoding) {
        instrs_start_offset = llvm::decodeULEB128(ptr, &uleb_size);     ptr += uleb_size;
        instrs_len_bytes = llvm::decodeULEB128(ptr, &uleb_size);        ptr += uleb_size;
        landing_pad_offset = llvm::decodeULEB128(ptr, &uleb_size);      ptr += uleb_size;
    } else if (DW_EH_PE_udata4 == info->call_site_table_entry_encoding) {
        // offsets are from landing pad base
        instrs_start_offset = (uint64_t) *(const uint32_t*) ptr;
        instrs_len_bytes = (uint64_t) *(const uint32_t*) (ptr + 4);
        landing_pad_offset = (uint64_t) *(const uint32_t*) (ptr + 8);
        ptr += 12;
    } else {
        RELEASE_ASSERT(0, "expected call site table entries to use DW_EH_PE_udata4 or DW_EH_PE_uleb128");
    }

    // action offset (plus one) is always a ULEB128
    action_offset_plus_one = llvm::decodeULEB128(ptr, &uleb_size);
    ptr += uleb_size;

    entry->instrs_start = info->landing_pad_base + instrs_start_offset;
    entry->instrs_len_bytes = instrs_len_bytes;
    if (0 == landing_pad_offset) {
        // An offset of 0 is special and indicates "no landing pad", i.e. this call site does not handle exceptions or
        // perform any cleanup. (The call site entry is still necessary to indicate that it is *expected* that an
        // exception could be thrown here, and that unwinding should proceed; if the entry were absent, we'd call
        // std::terminate().)
        entry->landing_pad = nullptr;
    } else {
        entry->landing_pad = info->landing_pad_base + landing_pad_offset;
    }
    entry->action_offset_plus_one = action_offset_plus_one;

    return ptr;
}

static inline const uint8_t *first_action(const lsda_info_t* info, const call_site_entry_t *entry) {
    if (!entry->action_offset_plus_one)
        return nullptr;
    return info->action_table + entry->action_offset_plus_one - 1;
}

static inline int64_t next_action(const uint8_t** pp) {
    const uint8_t *p = *pp;
    unsigned leb_size;
    int64_t type_filter = llvm::decodeSLEB128(p, &leb_size);
    p += leb_size;
    int64_t offset_to_next_entry = llvm::decodeSLEB128(p);
    // an offset of 0 ends the action-chain.
    *pp = offset_to_next_entry ? p + offset_to_next_entry : nullptr;
    return type_filter;
}


// ---------- Printing things for debugging purposes ----------
static void print_lsda(const lsda_info_t *info) {
    uint64_t action_table_min_len_bytes = 0;

    // print call site table
    // the call site table ends where the action table begins
    printf("Call site table:\n");
    const uint8_t *p = info->call_site_table;
    while (p < info->action_table) {
        call_site_entry_t entry;
        p = parse_call_site_entry(p, info, &entry);
        printf("  start %p end %p landingpad %p action-plus-one %lx\n",
               entry.instrs_start, entry.instrs_start + entry.instrs_len_bytes,
               entry.landing_pad, entry.action_offset_plus_one);

        // Follow the action chain.
        for (const uint8_t *action_ptr = first_action(info, &entry); action_ptr;) {
            RELEASE_ASSERT(action_ptr >= info->action_table, "malformed LSDA");
            ptrdiff_t offset = action_ptr - info->action_table;
            // add one to indicate that there is an entry here. (consider the case of an empty table, for example.)
            // would be nicer to set action_table_min_len_bytes to the end of the entry, but that involves uleb-size
            // arithmetic.
            if (offset + 1 > action_table_min_len_bytes)
                action_table_min_len_bytes = offset + 1;

            int64_t type_filter = next_action(&action_ptr);
            if (action_ptr)
                printf("    %ld: filter %ld  next %ld\n", offset, type_filter, action_ptr - info->action_table);
            else
                printf("    %ld: filter %ld  end\n", offset, type_filter);
        }
    }

    // Print the action table.
    printf("Action table:\n");
    RELEASE_ASSERT(p == info->action_table, "malformed LSDA");
    while (p < info->action_table + action_table_min_len_bytes) {
        ptrdiff_t offset = p - info->action_table;
        int64_t type_filter = next_action(&p);

        if (p)
            printf("  %ld: filter %ld  next %ld\n", offset, type_filter, p - info->action_table);
        else
            printf("  %ld: filter %ld  end\n", offset, type_filter);
    }
}

// FIXME: duplicated from unwinding.cpp
static
unw_word_t getFunctionEnd(unw_word_t ip) {
    unw_proc_info_t pip;
    // where is the documentation for unw_get_proc_info_by_ip, anyway?
    int ret = unw_get_proc_info_by_ip(unw_local_addr_space, ip, &pip, NULL);
    RELEASE_ASSERT(ret == 0 && pip.end_ip, "");
    return pip.end_ip;
}

static
void print_frame(unw_cursor_t* cursor, const unw_proc_info_t* pip) {
    // FIXME: code duplication with PythonFrameIter::incr
    static unw_word_t interpreter_instr_end = getFunctionEnd((unw_word_t)interpreter_instr_addr);
    static unw_word_t generator_entry_end = getFunctionEnd((unw_word_t)generatorEntry);

    unw_word_t ip, bp;
    check(unw_get_reg(cursor, UNW_REG_IP, &ip));
    check(unw_get_reg(cursor, UNW_TDEP_BP, &bp));

    // NB. unw_get_proc_name appears to be MUCH slower than dl_addr for getting the names of functions!
    // but it also gets the names of more functions, so we use it for now.

    {
        char name[500];
        unw_word_t off;
        int err = unw_get_proc_name(cursor, name, 500, &off);
        // ENOMEM means name didn't fit in buffer, so it was truncated. We're okay with that.
        RELEASE_ASSERT(!err || err == -UNW_ENOMEM || err == -UNW_ENOINFO, "unw_get_proc_name errored");
        if (err != -UNW_ENOINFO) {
            printf(strnlen(name, 500) < 50 ? "  %-50s" : "  %s\n", name);
        } else {
            printf("  %-50s", "? (no info)");
        }
    }

    // {
    //     Dl_info dl_info;
    //     if (dladdr((void*)ip, &dl_info)) { // returns non-zero on success, wtf?
    //         if (!dl_info.dli_sname || strlen(dl_info.dli_sname) < 50)
    //             printf("  %-50s", dl_info.dli_sname ? dl_info.dli_sname : "(unnamed)");
    //         else
    //             printf("  %s\n", dl_info.dli_sname);
    //     } else {
    //         printf("  %-50s", "? (no dl info)");
    //     }
    // }

    CompiledFunction *cf = getCFForAddress(ip);
    AST_stmt *cur_stmt = nullptr;
    enum { COMPILED, INTERPRETED, GENERATOR, OTHER } frame_type;
    if (cf) {
        // compiled frame
        frame_type = COMPILED;
        printf("      ip %12lx  bp %lx    JITTED\n", ip, bp);
        // TODO: get current statement
    } else if ((unw_word_t) interpreter_instr_addr <= ip && ip < interpreter_instr_end) {
        // interpreted frame
        frame_type = INTERPRETED;
        printf("      ip %12lx  bp %lx    interpreted\n", ip, bp);
        // sometimes this assert()s!
        // cf = getCFForInterpretedFrame((void*)bp);
        // cur_stmt = getCurrentStatementForInterpretedFrame((void*) bp);
    } else if ((unw_word_t) generatorEntry <= ip && ip < generator_entry_end) {
        // generator return frame
        frame_type = GENERATOR;
        printf("      ip %12lx  bp %lx    generator\n", ip, bp);
    } else {
        // generic frame, probably C/C++
        frame_type = OTHER;
        printf("      ip %12lx  bp %lx\n", ip, bp);
    }

    if (frame_type == INTERPRETED && cf && cur_stmt) {
        auto source = cf->clfunc->source.get();
        // FIXME: dup'ed from lineInfoForFrame
        LineInfo line(cur_stmt->lineno, cur_stmt->col_offset, source->fn, source->getName());
        printf("      File \"%s\", line %d, in %s\n", line.file.c_str(), line.line, line.func.c_str());
    }
}


// ---------- Helpers for unwind_loop ----------
__attribute__((__always_inline__))
static inline
bool find_call_site_entry(const lsda_info_t* info, const uint8_t *ip, call_site_entry_t* entry) {
    const uint8_t *p = info->call_site_table;
    // The call site table ends where the action table begins.
    while (p < info->action_table) {
        p = parse_call_site_entry(p, info, entry);

        if (VERBOSITY("cxx_unwind") >= 3) {
            printf("    start %p end %p landingpad %p action %lx\n",
                   entry->instrs_start, entry->instrs_start + entry->instrs_len_bytes,
                   entry->landing_pad, entry->action_offset_plus_one);
        }

        // If our IP is in the given range, we found the right entry!
        if (entry->instrs_start <= ip && ip < entry->instrs_start + entry->instrs_len_bytes)
            return true;

        // The call-site table is in sorted order by start IP. If we've passed our current IP, we won't find an entry.
        if (ip < entry->instrs_start + entry->instrs_len_bytes)
            break;
    }

    // If p actually overran *into* info.action_table, we have a malformed LSDA.
    RELEASE_ASSERT(!(p > info->action_table), "Malformed LSDA; call site entry overlaps action table!");
    return false;
}

static inline NORETURN
void resume(unw_cursor_t* cursor, const uint8_t *landing_pad, int64_t switch_value, const ExcInfo *exc_info) {
    assert(landing_pad);
    if (VERBOSITY("cxx_unwind") >= 2)
        printf("  * RESUMED: ip %p  switch_value %ld\n", (const void*)landing_pad, (long)switch_value);

    if (0 != switch_value) {
        // The exception handler will call __cxa_begin_catch, which stops this timer and logs it.
        per_thread_resume_catch_timer.restart("resume_catch", 20);
    } else {
        // The cleanup code will call _Unwind_Resume, which will stop this timer and log it.
        // TODO: am I sure cleanup code can't raise exceptions? maybe have an assert!
        per_thread_cleanup_timer.restart("cleanup", 20);
#ifndef NDEBUG
        in_cleanup_code = true;
#endif
    }

    // set rax to POISON_VALUE, because we don't use it.
    // set rdx to the switch_value (0 for cleanup, otherwise an index indicating which exception handler to use)
    //
    // TODO: assumes x86-64!
    // maybe I should use __builtin_eh_return_data_regno() here?
    // but then, need to translate into UNW_* values somehow. not clear how.
    check(unw_set_reg(cursor, UNW_X86_64_RAX, RAX_POISON_VALUE));
    check(unw_set_reg(cursor, UNW_X86_64_RDX, switch_value));

    // resume!
    // NOTE: according to shachaf, setcontext() - which is what libunwind uses for context-switching under the hood -
    // does a system call per context switch, to set the signal mask. TODO: verify and measure.
    check(unw_set_reg(cursor, UNW_REG_IP, (unw_word_t)landing_pad));
    unw_resume(cursor);
    RELEASE_ASSERT(0, "unw_resume returned!");
}

// Determines whether to dispatch to cleanup code or an exception handler based on the action table.
// Doesn't need exception info b/c in Pyston we assume all handlers catch all exceptions.
//
// Returns the switch value to be passed into the landing pad, which selects which handler gets run in the case of
// multiple `catch' blocks, or is 0 to run cleanup code.
static inline
int64_t determine_action(const lsda_info_t* info, const call_site_entry_t *entry) {
    // No action means there are destructors/cleanup to run, but no exception handlers.
    const uint8_t *p = first_action(info, entry);
    if (!p)
        return CLEANUP_ACTION;

    // Read a chain of actions.
    if (VERBOSITY("cxx_unwind") >= 3) {
        printf("      reading action chain\n");
    }

    // When we see a cleanup action, we *don't* immediately take it. Rather, we remember that we should clean up if none
    // of the other actions matched.
    bool saw_cleanup = false;
    do {
        ASSERT(p >= info->action_table, "malformed LSDA");
        ptrdiff_t offset = p - info->action_table;
        int64_t type_filter = next_action(&p);
        if (VERBOSITY("cxx_unwind") >= 3) {
            if (p)
                printf("      %ld: filter %ld  next %ld\n", offset, type_filter, p - info->action_table);
            else
                printf("      %ld: filter %ld  end\n", offset, type_filter);
        }

        if (0 == type_filter) {
            // A type_filter of 0 indicates a cleanup.
            saw_cleanup = true;
        } else {
            // Otherwise, the type_filter is supposed to be interpreted by looking up information in the types table and
            // comparing it against the type of the exception thrown. In Pyston, however, every exception handler
            // handles all exceptions, so we ignore the type information entirely and just run the handler.
            //
            // I don't fully understand negative type filters. For now we don't implement them. See
            // http://www.airs.com/blog/archives/464 for some information.
            RELEASE_ASSERT(type_filter > 0, "negative type filters unimplemented");
            return type_filter;
        }
    } while (p);

    if (saw_cleanup)
        return CLEANUP_ACTION;

    // We ran through the whole action chain and none applied, *and* there was no cleanup indicated. What do we do?
    // This can't happen currently, but I think the answer is probably panic().
    RELEASE_ASSERT(0, "action chain exhausted and no cleanup indicated");
}

static inline int step(unw_cursor_t *cp) {
    LogTimer t("unw_step", us_unwind_step, 5);
    return unw_step(cp);
}

// The stack-unwinding loop.
// TODO: integrate incremental traceback generation into this function
static inline
void unwind_loop(const ExcInfo *exc_info) {
    Timer t("unwind_loop", 50);

    // NB. https://monoinfinito.wordpress.com/series/exception-handling-in-c/ is a very useful resource
    // as are http://www.airs.com/blog/archives/460 and http://www.airs.com/blog/archives/464
    unw_cursor_t cursor;
    {
        unw_context_t uc;       // exists only to initialize cursor
        unw_getcontext(&uc);
        unw_init_local(&cursor, &uc);
    }

    // TODO?: need to handle unwinding through generator frames?
    while (step(&cursor) > 0) {
        unw_proc_info_t pip;
        {
            // as it turns out, unw_get_proc_info is REALLY SLOW
            // things to try: registering JITted procs as local_table_info instead of remote?
            // but it seems like mostly it's just slow and there's no good way around it :(
            // should figure out what in particular is slow
            LogTimer t_procinfo("get_proc_info", us_unwind_get_proc_info, 10);
            check(unw_get_proc_info(&cursor, &pip));
        }
        assert((pip.lsda == 0) == (pip.handler == 0));
        assert(pip.flags == 0);

        // TODO: get line info for this frame!
        // TODO: should I use PythonFrameIter for this purpose?
        // no, it'll skip C++ frames that might need unwinding.
        if (VERBOSITY("cxx_unwind") >= 2) {
            print_frame(&cursor, &pip);
        }

        // Skip frames without handlers
        if (pip.handler == 0) {
            continue;
        }

        RELEASE_ASSERT(pip.handler == (uintptr_t)__gxx_personality_v0,
                       "personality function other than __gxx_personality_v0; "
                       "don't know how to unwind through non-C++ functions");

        // Ignore its actual personality and perform dispatch ourselves.
        // 1. parse LSDA header
        lsda_info_t info;
        parse_lsda_header(&pip, &info);

        call_site_entry_t entry;
        {
            LogTimer t_call_site("find_call_site_entry", us_unwind_find_call_site_entry, 10);

            // 2. Find our current IP in the call site table.
            unw_word_t ip;
            unw_get_reg(&cursor, UNW_REG_IP, &ip);
            // ip points to the instruction *after* the instruction that caused the error - which is generally (always?) a call
            // instruction - UNLESS we're in a signal frame, in which case it points at the instruction that caused the error.
            // For now, we assume we're never in a signal frame. So, we decrement it by one.
            //
            // TODO: can this code ever get called on a signal frame?
            --ip;

            bool found = find_call_site_entry(&info, (const uint8_t*)ip, &entry);
            // If we didn't find an entry, an exception happened somewhere exceptions should never happen; terminate
            // immediately.
            if (!found) {
                panic();
            }
        }

        // 3. Figure out what to do based on the call site entry.
        if (!entry.landing_pad) {
            // No landing pad means no exception handling or cleanup; keep unwinding!
            continue;
        }
        // After this point we are guaranteed to resume something rather than unwinding further.

        if (VERBOSITY("cxx_unwind") >= 3) {
            print_lsda(&info);
        }

        int64_t switch_value = determine_action(&info, &entry);
        us_unwind_loop.log(t.end());
        resume(&cursor, entry.landing_pad, switch_value, exc_info);
    }

    us_unwind_loop.log(t.end());
    // Hit end of stack! return & let unwindException determine what to do.
}


// The unwinder entry-point.
static
void unwind(void) {
    assert(exception_ferry.type && exception_ferry.value && exception_ferry.traceback);
    unwind_loop(&exception_ferry);
    // unwind_loop returned, couldn't find any handler. ruh-roh.
    panic();
}

} // extern "C"
} // namespace pyston


// Standard library / runtime functions we override
#if PYSTON_CUSTOM_UNWINDER

void std::terminate() noexcept {
    // The default std::terminate assumes things about the C++ exception state which aren't true for our custom
    // unwinder.
    RELEASE_ASSERT(0, "std::terminate() called!");
}

// wrong type signature, but that's okay, it's extern "C"
extern "C"
void __gxx_personality_v0() {
    RELEASE_ASSERT(0, "__gxx_personality_v0 should never get called");
}

extern "C"
void _Unwind_Resume(struct _Unwind_Exception *_exc) {
    assert(pyston::in_cleanup_code);    // is this right?
#ifndef NDEBUG
    pyston::in_cleanup_code = false;
#endif
    pyston::us_unwind_cleanup.log(pyston::per_thread_cleanup_timer.end());

    if (VERBOSITY("cxx_unwind"))
        printf("***** _Unwind_Resume() *****\n");
    // we give `_exc' type `struct _Unwind_Exception*' because unwind.h demands it; it's not actually accurate
    assert((uintptr_t)_exc == RAX_POISON_VALUE); // double-check
    pyston::unwind();
}

// C++ ABI functionality
namespace __cxxabiv1 {

// TODO?: maybe we should actually use the `exc_obj' pointer passed through all these procedures instead of poisoning it
// and using cur_thread_state every time?

extern "C"
void *__cxa_allocate_exception(size_t size) noexcept {
    // we should only ever be throwing ExcInfos
    ASSERT(size == sizeof(pyston::ExcInfo), "allocating exception whose size doesn't match ExcInfo");

    // Instead of allocating memory for this exception, we return a pointer into a pre-allocated region in
    // cur_thread_state.
    //
    // This location is used legitimately *ONLY* in the following short timespans:
    // - between cxa_allocate_exception and cxa_throw
    //
    // - between cxa_begin_catch and when the result of cxa_begin_catch is copied out onto the stack (which it will be
    //   if you follow the Pyston internal requirement that all catches are by-value not by-reference).
    //
    // FIXME: ^this list is inaccurate, what about _Unwind_Resume?
    //
    // All of the code in these regions is generated by the compiler, so any reference to `pyston_exception_ferry'
    // outside of this file and the header that defines it is wrong!
    //
    // TODO: a document describing how not to fuck up exception handling in pyston
    //
    // Q: Why can't we just use curexc_{type,value,traceback}?
    //
    // A: Because that conflates the space used to store exceptions during C++ unwinding with the space used to store
    // them during C-API return-code based unwinding! This actually comes up in practice - the original version *did*
    // use curexc_{type,value,traceback}, and it had a bug.
    //
    // In particular, we need to unset the C API exception at an appropriate point so as not to make C-API functions
    // *think* an exception is being thrown when one isn't. The natural place is __cxa_begin_catch, BUT we need some way
    // to communicate the exception info to the inside of the catch block - and all we get is a single lousy pointer,
    // when we need three!
    //
    // You might think we could get away with only unsetting the C-API information in __cxa_end_catch, but you'd be
    // wrong! The problem is that __cxa_end_catch is always called when leaving a catch block, even if we're leaving it
    // by re-raising the exception. So if we store our exception info in curexc_*, and then unset these in
    // __cxa_end_catch, then we'll wipe our exception info during unwinding!
    //
    // There might be some clever way to signal to __cxa_end_catch that we're unwinding and shouldn't wipe the exception
    // info. This seems simpler. TODO: think about this.
    return (void*) &pyston::exception_ferry;
}

// This function is supposed to return a pointer to the exception value actually thrown. So if we threw an ExcInfo, this
// should return an ExcInfo*. And if we catch it like so:
//
//     catch(ExcInfo& c) { ... }
//
// Then within the { ... }, `c' refers to the contents of that ExcInfo*.
//
// Usually, the actual pointer points to some memory allocated by __cxa_allocate_exception. In our case, we point it at
// the internals of PyThreadState. This is rather hackish.
//
// TODO: document that you must always do `catch(ExcInfo e) { ... }' instead so that `e' gets copied locally and isn't
// volatile.
extern "C"
void *__cxa_begin_catch(void *exc_obj_in) noexcept {
    assert((uintptr_t)exc_obj_in == RAX_POISON_VALUE); // double-check

    pyston::us_unwind_resume_catch.log(pyston::per_thread_resume_catch_timer.end());

    if (VERBOSITY("cxx_unwind"))
        printf("***** __cxa_begin_catch() *****\n");

    // Hack: we pun a pointer into PyThreadState.pyston_exception_ferry as a pyston::ExcInfo*.
    pyston::ExcInfo *e = &pyston::exception_ferry;
    assert(e->type && e->value && e->traceback);
    return (void*) e;
}

extern "C"
void __cxa_end_catch() {
    if (VERBOSITY("cxx_unwind"))
        printf("***** __cxa_end_catch() *****\n");

    // We DO NOT clear the Python C API error or the pyston exception ferry here. This is because __cxa_end_catch is
    // *always* called, even when you re-throw inside a catch. For example:
    //
    //     catch (ExcInfo e) {
    //         if (some_condition)
    //             return; // error handled
    //         throw e;    // re-propagate
    //     }
    //
    //  Here, `throw e' will set the pyston exception ferry according to `e', then invoke the unwinder. The unwinder
    //  will discover that the function we're in has cleanup code to run. This cleanup code does nothing but invoke
    //  __cxa_end_catch() followed by _Unwind_Resume(); its sole purpose is to ensure that __cxa_end_catch() is *always*
    //  called on exiting a catch.
    //
    //  TODO: write a README on how to do exception-handling in the Pyston codebase without fucking up.
}

extern "C"
void __cxa_throw(void *exc_obj, std::type_info *tinfo, void (*dtor)(void*)) {
    assert(!pyston::in_cleanup_code);

    if (VERBOSITY("cxx_unwind"))
        printf("***** __cxa_throw() *****\n");

    ASSERT(exc_obj == (void*) &pyston::exception_ferry,
           "throwing exception not allocated on the Pyston exception ferry");

    pyston::unwind();
}

extern "C"
void *__cxa_get_exception_ptr(void *exc_obj_in) noexcept {
    assert((uintptr_t) exc_obj_in == RAX_POISON_VALUE);
    assert(pyston::exception_ferry.type
           && pyston::exception_ferry.value
           && pyston::exception_ferry.traceback);

    return (void*) &pyston::exception_ferry;
}

// We deliberately don't implement rethrowing because we can't implement it correctly with our current strategy for
// storing the exception info. Don't use bare `throw' from inside an exception handler! Instead, do:
//
//     try { ... }
//     catch(ExcInfo e) {   // copies the exception info received to the stack
//         ...
//         throw e;
//     }
//
extern "C"
void __cxa_rethrow() {
    RELEASE_ASSERT(0, "__cxa_rethrow() unimplemented; please don't use bare `throw' in Pyston!");
}

}

#endif  // PYSTON_CUSTOM_UNWINDER
