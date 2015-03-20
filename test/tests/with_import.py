import sys

def f():
    with open('/dev/null'):
        import noexiste

try:
    f()
except ImportError as e:
    print e

def f2():
    with open('/dev/null'):
        from sys import nonesuch

try:
    f2()
except ImportError as e:
    print e
