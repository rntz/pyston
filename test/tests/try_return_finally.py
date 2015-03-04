# fail-if: ('-n' in jit_args) or ('-O' in jit_args)
# this is triggering an internal assert
def f():
    try:
        # Looks like this returns from the function, but it needs to go to the finally block
        return
    finally:
        pass
f()
