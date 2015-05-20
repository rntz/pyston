# The Pyston Unwinder

Pyston uses a custom exception unwinder, replacing the general-purpose C++ unwinder provided by `libstdc++` and `libgcc`. We do this for two reasons:

1. **Efficiency**. The default clang/gcc C++ unwinder is slow, because it needs to support features we don't (such as two-phase unwinding, and having multiple exception types) and because it isn't optimized for speed (C++ assumes exceptions are uncommon).

2. **Customizability**. For example, Python handles backtraces differently than C++ does; with a custom unwinder, we can support Python-style backtraces more easily.

The custom unwinder is in `src/runtime/cxx_unwind.cpp`.

# How normal C++ unwinding works

The big picture is that when an exception is thrown, we walk the stack *twice*:

1. In the first phase, we look for a `catch`-block whose type matches the thrown exception. If we don't find one, we terminate the process.

2. In the second phase, we unwind up to the `catch`-block we found; along the way we run any intervening `finally` blocks or RAII destructors.

The purpose of the two-phase search is to make sure that *exceptions that won't be caught terminate the process immediately with a full stack-trace*. In Pyston we don't care about this --- stack traces work differently for us anyway.

## How normal C++ unwinding works, in detail

[This excellent blog post](https://monoinfinito.wordpress.com/series/exception-handling-in-c/) covers the dirty details. What follows is a summary:

### Throwing

C++ `throw` statements are translated into a pair of method calls:

1. A call to `void *__cxxabiv1::__cxa_allocate_exception(size_t)` allocates space for an exception of the given size.

2. A call to `void __cxxabiv1::__cxa_throw(void *exc_obj, std::type_info *type_info, void (*dtor)(void*))` invokes the stack unwinder. `exc_obj` is the exception to be thrown; `type_info` is the RTTI for the exception's class, and `dtor` is a callback that (I think) is called to destroy the exception object.

These methods (and others in the `__cxxabiv1` namespace) are defined in `libstdc++`. `__cxa_throw` invokes the generic (non-C++-specific) unwinder by calling `_Unwind_RaiseException()`. This function (and others prefixed with `_Unwind`) are defined in `libgcc`. The details of the libgcc unwinder's interface are less important, and I omit them here.

### Unwinding and .eh_frame

The libgcc unwinder walks the call frame stack, looking up debug information about each function it unwinds through. It finds the debug information by searching for the instruction pointer that would be returned-to in a list of tables; one table for each loaded object (in the linker-and-loader sense of "object", i.e. executable file or shared library). For a given object, the debug info is in a section called `.eh_frame`. See [this blog post](http://www.airs.com/blog/archives/460) for more on the format of `.eh_frame`.

In particular, the unwinder checks whether the function has an associated "personality function", and calls it if it does. If there's no personality function, unwinding continues as normal. C functions do not have personality functions. C++ functions have the personality function `__gxx_personality_v0`, or (if they don't involve exceptions or RAII at all) no personality function.

The job of the personality function is to:

1. Determine what action, if any, needs to happen when unwinding this exception through this frame.

2. If we are in Phase 1, or if there is no action to be taken, report this information to the caller.

3. If we are in Phase 2, actually take the relevant action: jump into the relevant cleanup code, `finally`, or `catch` block. In this case, the personality function does not return.

### The LSDA: how the personality function works

The personality function determines what to do by comparing the instruction pointer being unwound through against C++-specific unwinding information. This is contained in an area of `.eh_frame` called the LSDA (Language-Specific Data Area). See [this blog post](http://www.airs.com/blog/archives/464) for a detailed run-down.

### Landing pads and resuming from the personality function

TODO

# How our unwinder is different

We use `libunwind` to deal with a lot of the tedious gruntwork (restoring register state, etc.) of unwinding.

First, we dispense with two-phase unwinding. It's slow and Python tracebacks work differently anyway. (Currently we grab tracebacks before we start unwinding; in the future, we ought to generate them incrementally *as* we unwind.)

Second, we allocate exceptions using a thread-local variable, rather than `malloc()`. By ensuring that only one exception is ever active on a given thread at a given time, this lets us be more efficient. However, we have not measured the performance improvement here; it may be negligible.

Third, when unwinding, we only check whether a function *has* a personality function. If it does, we assert that it is `__gxx_personality_v0`, but we *do not call it*. Instead, we run our own custom dispatch code. We do this because:

1. One argument to the personality function is the current unwind context, in a `libgcc`-specific format. libunwind uses a different format, so we *can't* call it.

2. It avoids an unnecessary indirect call.

3. The personality function checks the exception's type against `catch`-block types. All Pyston exceptions have the same type, so this is unnecessary.

## Functions we override
- `std::terminate`
- `__gxx_personality_v0`: stubbed out, should never be called
- `_Unwind_Resume`
- `__cxxabiv1::__cxa_allocate_exception`
- `__cxxabiv1::__cxa_begin_catch`
- `__cxxabiv1::__cxa_end_catch`
- `__cxxabiv1::__cxa_throw`
- `__cxxabiv1::__cxa_rethrow`: stubbed out, we never rethrow directly
- `__cxxabiv1::__cxa_get_exception_ptr`
