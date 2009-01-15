# Minimal tests for dis module

from test.test_support import run_unittest
import unittest
import sys
import dis
import StringIO


def _f(a):
    print a
    return 1

dis_f = """\
 %-4d         0 LOAD_GLOBAL              0 (#@print_stmt)
              2 LOAD_FAST                0 (a)
              4 CALL_FUNCTION            1
              6 POP_TOP

 %-4d         7 LOAD_CONST               1 (1)
              9 RETURN_VALUE
"""%(_f.func_code.co_firstlineno + 1,
     _f.func_code.co_firstlineno + 2)


def _supertest(a):
    return a + 1

dis_supertest = """\
 %-4d         0 LOAD_FAST                0 (a)
              2 CBINARY_ADD:
                  LOAD_CONST               1 (1)
                  BINARY_ADD
              4 RETURN_VALUE
"""%(_supertest.func_code.co_firstlineno + 1,)


def _2arg_supertest(a):
    return (a, 1, 2)

dis_2arg_supertest = """\
 %-4d         0 LOAD_FAST                0 (a)
              2 CC:
                  LOAD_CONST               1 (1)
                  LOAD_CONST               2 (2)
              5 BUILD_TUPLE              3
              7 RETURN_VALUE
"""%(_2arg_supertest.func_code.co_firstlineno + 1,)


def bug708901():
    for res in range(1,
                     10):
        pass

dis_bug708901 = """\
 %-4d         0 SETUP_LOOP              15 (to 17)
              2 LOAD_GLOBAL              0 (range)

 %-4d         4 CC:
                  LOAD_CONST               1 (1)
                  LOAD_CONST               2 (10)
              7 CALL_FUNCTION            2
              9 GET_ITER
        >>   10 FOR_ITER                 4 (to 16)
             12 STORE_FAST               0 (res)

 %-4d        14 JUMP_ABSOLUTE           10
        >>   16 POP_BLOCK
        >>   17 LOAD_CONST               0 (None)
             19 RETURN_VALUE
"""%(bug708901.func_code.co_firstlineno + 1,
     bug708901.func_code.co_firstlineno + 2,
     bug708901.func_code.co_firstlineno + 3)


def bug1333982(x=[]):
    assert 0, ([s for s in x] +
              1)
    pass

dis_bug1333982 = """\
 %-4d         0 LOAD_CONST               1 (0)
              2 JUMP_IF_TRUE            27 (to 31)
              4 POP_TOP
              5 LOAD_GLOBAL              0 (AssertionError)
              7 BUILD_LIST               0
              9 DUP_TOP
             10 STORE_FAST               1 (_[1])
             12 LOAD_FAST                0 (x)
             14 GET_ITER
        >>   15 FOR_ITER                 9 (to 26)
             17 STORE_FAST               2 (s)
             19 LOAD_FAST                1 (_[1])
             21 LOAD_FAST                2 (s)
             23 LIST_APPEND
             24 JUMP_ABSOLUTE           15
        >>   26 DELETE_FAST              1 (_[1])

 %-4d        28 CBINARY_ADD:
                  LOAD_CONST               2 (1)
                  BINARY_ADD
             30 RAISE_VARARGS_TWO
        >>   31 POP_TOP

 %-4d        32 LOAD_CONST               0 (None)
             34 RETURN_VALUE
"""%(bug1333982.func_code.co_firstlineno + 1,
     bug1333982.func_code.co_firstlineno + 2,
     bug1333982.func_code.co_firstlineno + 3)

_BIG_LINENO_FORMAT = """\
%3d           0 LOAD_GLOBAL              0 (spam)
              2 POP_TOP
              3 LOAD_CONST               0 (None)
              5 RETURN_VALUE
"""

class DisTests(unittest.TestCase):
    def do_disassembly_test(self, func, expected):
        s = StringIO.StringIO()
        save_stdout = sys.stdout
        sys.stdout = s
        dis.dis(func)
        sys.stdout = save_stdout
        got = s.getvalue()
        # Trim trailing blanks (if any).
        lines = got.split('\n')
        lines = [line.rstrip() for line in lines]
        expected = expected.split("\n")
        import difflib
        if expected != lines:
            self.fail(
                "events did not match expectation:\n" +
                "\n".join(difflib.ndiff(expected,
                                        lines)))

    def test_opmap(self):
        self.assertEqual(dis.opmap["LOAD_CONST"] in dis.hasconst, True)
        self.assertEqual(dis.opmap["STORE_NAME"] in dis.hasname, True)

    def test_opname(self):
        self.assertEqual(dis.opname[dis.opmap["LOAD_FAST"]], "LOAD_FAST")

    def test_dis(self):
        self.do_disassembly_test(_f, dis_f)

    def test_dis_super(self):
        self.do_disassembly_test(_supertest, dis_supertest)

    def test_dis_2_arg_super(self):
        self.do_disassembly_test(_2arg_supertest, dis_2arg_supertest)

    def test_bug_708901(self):
        self.do_disassembly_test(bug708901, dis_bug708901)

    def test_bug_1333982(self):
        # This one is checking bytecodes generated for an `assert` statement,
        # so fails if the tests are run with -O.  Skip this test then.
        if __debug__:
            self.do_disassembly_test(bug1333982, dis_bug1333982)

    def test_big_linenos(self):
        def func(count):
            namespace = {}
            func = "def foo():\n " + "".join(["\n "] * count + ["spam\n"])
            exec func in namespace
            return namespace['foo']

        # Test all small ranges
        for i in xrange(1, 300):
            expected = _BIG_LINENO_FORMAT % (i + 2)
            self.do_disassembly_test(func(i), expected)

        # Test some larger ranges too
        for i in xrange(300, 5000, 10):
            expected = _BIG_LINENO_FORMAT % (i + 2)
            self.do_disassembly_test(func(i), expected)

def test_main():
    run_unittest(DisTests)


if __name__ == "__main__":
    test_main()
