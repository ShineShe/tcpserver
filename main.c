// Copyright (c) 2004-2013 Sergey Lyubka
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#define _XOPEN_SOURCE 600  // For PATH_MAX on linux

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdarg.h>
#include <ctype.h>

#include "mongoose.h"


#include <sys/wait.h>
#include <unistd.h>

static int exit_flag;
static char server_name[40];        // Set by init_server_name()
static struct mg_context *ctx;      // Set by start_mongoose()


static void  signal_handler(int sig_num) {
  exit_flag = sig_num;
}

static void die(const char *fmt, ...) {
  va_list ap;
  char msg[200];

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  fprintf(stderr, "%s\n", msg);
  exit(EXIT_FAILURE);
}


static void init_server_name(void) {
//  snprintf(server_name, sizeof(server_name), "Mongoose web server v. %s",
//           mg_version());

snprintf(server_name, sizeof(server_name), "Bitbox Music server v. %s",
		 mg_version());

}


static void start_bitbox() {
  /* Setup signal handler: quit on Ctrl-C */
  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);

  
  ctx = mg_start();

  if (ctx == NULL) {
    die("%s", "Failed to start Mongoose.");
  }
}

int main() {
  init_server_name();
  start_bitbox();
  printf("%s started on port(s) %s \n",
         server_name, "9735");
  while (exit_flag == 0) {
    sleep(1);
  }
  printf("Exiting on signal %d, waiting for all threads to finish...",
         exit_flag);
  fflush(stdout);
  mg_stop(ctx);
  printf("%s", " done.\n");

  return EXIT_SUCCESS;
}

