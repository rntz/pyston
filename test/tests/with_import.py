import sys

def f():
    # originally this exposed a bug in our irgen phase, so even `with None`
    # failed here; the bug happened before actual execution. Just to test more
    # things, though, we use an actual contextmanager here.
    with open('/dev/null'):
        import noexiste

try:
    f()
except ImportError as e:
    print e

def f2():
    # originally this exposed a bug in our irgen phase, so even `with None`
    # failed here; the bug happened before actual execution. Just to test more
    # things, though, we use an actual contextmanager here.
    with open('/dev/null'):
        from sys import nonesuch

try:
    f2()
except ImportError as e:
    print e
