import sys

class A(Exception):
    def __str__(self):
        return 'A(%s)' % self.message

class B(Exception):
    def __str__(self):
        return 'B(%s)' % self.message

raise A(2)
# def f():
#     try: raise A, 2
#     except A as e:
#         print sys.exc_info()
#         print e

#     try: raise A(2)
#     except A as e:
#         print sys.exc_info()
#         print e

# f()
