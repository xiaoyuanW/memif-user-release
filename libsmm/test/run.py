#!/usr/bin/env python

import errno, optparse, os, select, subprocess, sys, time, zlib

program="./test-mig"
args = [program]	# more args can follow
count = -1	# default: inf until something stops us from below (kernel oops?)

if __name__ == '__main__':
	i = 0
	while True:
		print
		print "********** test harness: run %d  ************" %i
		print
		try: 
			subprocess.check_call(args)
		except subprocess.CalledProcessError, e:
			print >> sys.stderr, 'program failed: %s' %e
			break
		if count != -1 and i == count:
			break
		i += 1
