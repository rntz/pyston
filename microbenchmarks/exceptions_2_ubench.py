from __builtin__ import Exception

NUM_ITERS = 100 * 1000
RECURSE_DEPTH = 10

e = Exception()
counter = 0

def gtor():
    raise e
    yield 1

def raiser(n=RECURSE_DEPTH):
    global counter
    if n:
        try:
            raiser(n-1)
        finally:
            counter += 1
    else:
        g = gtor()
        g.next()

def f(niters):
    for i in xrange(niters):
        try:
            raiser()
        except Exception:
            pass

f(NUM_ITERS)
