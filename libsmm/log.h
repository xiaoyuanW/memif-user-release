/* xzl: this comes from the mutilate project
 * https://github.com/leverich/mutilate
 *
 *
 Copyright (c) 2012, Jacob Leverich, Stanford University
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of Stanford University nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL STANFORD UNIVERSITY BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 * */
#ifndef LOG_H
#define LOG_H

typedef enum { DEBUG, VERBOSE, INFO, WARN, QUIET } log_level_t;
extern log_level_t log_level;

void log_file_line(log_level_t level, const char *file, int line,
                   const char* format, ...);
#define L(level, args...) log_file_line(level, __FILE__, __LINE__, args)

#define D(args...) L(DEBUG, args)
#define V(args...) L(VERBOSE, args)
#define I(args...) L(INFO, args)
#define W(args...) L(WARN, args)

#define DIE(args...) do { W(args); exit(-1); } while (0)

#define NOLOG(x)                                \
  do { log_level_t old = log_level;             \
  log_level = QUIET;                            \
  (x);                                          \
  log_level = old;                              \
  } while (0)

#endif // LOG_H
