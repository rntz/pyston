from __builtin__ import Exception

# configuration
NUM_ITERS = 100 * 1000
WRAPPER_DEPTH = 10
RECURSE_DEPTH = 0

# exception code
counter = 0
e = Exception("bad wrong")

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

def f(niters):
    for i in xrange(niters):
        try:
            recurser()
        except Exception, e:
            pass

# run the function
f(NUM_ITERS)
