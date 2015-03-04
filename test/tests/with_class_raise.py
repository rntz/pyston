# fail-if: '-n' in EXTRA_JIT_ARGS or '-O' in EXTRA_JIT_ARGS
class ContextManager(object):
    def __enter__(self):
        print 'entered'
    def __exit__(self, exc_type, exc_value, exc_tback):
        print 'exited'

def f():
    with ContextManager():
        class C(object):
            raise Exception('an exception')

try:
    f()
except Exception as e:
    print e
