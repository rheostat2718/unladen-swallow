#! /usr/bin/python2.5

"""Wrapper script for testing the performance of the Spitfire template system.

This is intended to support Unladen Swallow's perf.py

This will have Spitfire generate a 1000x1000 table as many times as you
specify (via the -n flag). The raw times to generate the template will be
dumped to stdout. This is more convenient for Unladen Swallow's uses: it
allows us to keep all our stats in perf.py.
"""

__author__ = "collinwinter@google.com (Collin Winter)"

# Python imports
import optparse
import sys
import time

# Spitfire imports
import spitfire
import spitfire.compiler.analyzer
import spitfire.compiler.util


SPITFIRE_SRC = """<table xmlns:py="http://spitfire/">
#for $row in $table
<tr>
#for $column in $row
<td>$column</td>
#end for
</tr>
#end for
</table>
"""

def test_spitfire(count):
    # Activate the most aggressive Spitfire optimizations. While it might
    # conceivably be interesting to stress Spitfire's lower optimization
    # levels, we assume no-one will be running a production system with those
    # settings.
    spitfire_tmpl_o4 = spitfire.compiler.util.load_template(
        SPITFIRE_SRC,
        "spitfire_tmpl_o4",
        spitfire.compiler.analyzer.o4_options,
        {"enable_filters": False})

    table = [xrange(1000) for _ in xrange(1000)]

    # Warm up Psyco.
    spitfire_tmpl_o4(search_list=[{"table": table}]).main()

    times = []
    for _ in xrange(count):
        t0 = time.time()
        data = spitfire_tmpl_o4(search_list=[{"table": table}]).main()
        t1 = time.time()
        times.append(t1 - t0)
    return times


def test_spitfire_without_psyco(count):
    class FakePsyco(object):
        def bind(self, *args, **kwargs):
            pass
    sys.modules["psyco"] = FakePsyco()

    return test_spitfire(count)


if __name__ == "__main__":
    parser = optparse.OptionParser(
        usage="%prog [options]",
        description=("Test the performance of Spitfire."))
    parser.add_option("-n", action="store", type="int", default=100,
                      dest="num_runs", help="Number of times to run the test.")
    parser.add_option("--disable_psyco", action="store_true",
                      help="Turn off Psyco integration.")
    options, args = parser.parse_args()

    benchmark = test_spitfire
    if options.disable_psyco:
        benchmark = test_spitfire_without_psyco

    for t in benchmark(options.num_runs):
        print t