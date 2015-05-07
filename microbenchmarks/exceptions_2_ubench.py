from __builtin__ import Exception

# configuration
NUM_ITERS = 100 * 1000
WRAPPER_DEPTH = 10
RECURSE_DEPTH = 0
TRACEBACK_DEPTH = 0

# exception code
counter = 0
e = Exception("bad wrong")
e.magic_break = True

def gtor():
    yield 1
    raise e
    yield 2

def wrapper(n=WRAPPER_DEPTH):
    global counter
    if n:
        try:
            wrapper(n-1)
        finally:
            counter += 1
    else:
        for x in gtor():
            pass

def recurser(n=RECURSE_DEPTH):
    if n:
        return recurser(n-1)
    else:
        return wrapper()

def f(niters, traceback_depth=TRACEBACK_DEPTH):
    global counter
    if traceback_depth:
        f(niters, traceback_depth - 1)
    else:
        for i in xrange(niters):
            try:
                recurser()
            except Exception:
                counter = 0

# run the function
f(NUM_ITERS)
