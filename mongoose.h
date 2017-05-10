// Copyright (c) 2004-2012 Sergey Lyubka
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

#ifndef MONGOOSE_HEADER_INCLUDED
#define  MONGOOSE_HEADER_INCLUDED

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

struct mg_context;     // Handle for the HTTP service itself
struct mg_connection;  // Handle for the individual connection


// This structure contains information about the HTTP request.
struct mg_request_info {
  const char *request_method; // "GET", "POST", etc
  const char *uri;            // URL-decoded URI
  const char *http_version;   // E.g. "1.0", "1.1"
  const char *query_string;   // URL part after '?', not including '?', or NULL
  const char *remote_user;    // Authenticated user, or NULL if no auth used
  long remote_ip;             // Client's IP address
  int remote_port;            // Client's port
  int is_ssl;                 // 1 if SSL-ed, 0 if not
  void *user_data;            // User data pointer passed to mg_start()

  int num_headers;            // Number of HTTP headers
  struct mg_header {
    const char *name;         // HTTP header name
    const char *value;        // HTTP header value
  } http_headers[64];         // Maximum 64 headers
};


// This structure needs to be passed to mg_start(), to let mongoose know
// which callbacks to invoke. For detailed description, see
// https://github.com/valenok/mongoose/blob/master/UserManual.md
struct mg_callbacks {
  // Called when mongoose has received new HTTP request.
  // If callback returns non-zero,
  // callback must process the request by sending valid HTTP headers and body,
  // and mongoose will not do any further processing.
  // If callback returns 0, mongoose processes the request itself. In this case,
  // callback must not send any data to the client.
  int  (*begin_request)(struct mg_connection *);

  // Called when mongoose has finished processing request.
  void (*end_request)(const struct mg_connection *, int reply_status_code);

  // Called when mongoose is about to log a message. If callback returns
  // non-zero, mongoose does not log anything.
  int  (*log_message)(const struct mg_connection *, const char *message);

  // Called when mongoose initializes SSL library.
  int  (*init_ssl)(void *ssl_context, void *user_data);

  // Called when websocket request is received, before websocket handshake.
  // If callback returns 0, mongoose proceeds with handshake, otherwise
  // cinnection is closed immediately.
  int (*websocket_connect)(const struct mg_connection *);

  // Called when websocket handshake is successfully completed, and
  // connection is ready for data exchange.
  void (*websocket_ready)(struct mg_connection *);

  // Called when data frame has been received from the client.
  // Parameters:
  //    bits: first byte of the websocket frame, see websocket RFC at
  //          http://tools.ietf.org/html/rfc6455, section 5.2
  //    data, data_len: payload, with mask (if any) already applied.
  // Return value:
  //    non-0: keep this websocket connection opened.
  //    0:     close this websocket connection.
  int  (*websocket_data)(struct mg_connection *, int bits,
                         char *data, size_t data_len);

  // Called when mongoose tries to open a file. Used to intercept file open
  // calls, and serve file data from memory instead.
  // Parameters:
  //    path:     Full path to the file to open.
  //    data_len: Placeholder for the file size, if file is served from memory.
  // Return value:
  //    NULL: do not serve file from memory, proceed with normal file open.
  //    non-NULL: pointer to the file contents in memory. data_len must be
  //              initilized with the size of the memory block.
  const char * (*open_file)(const struct mg_connection *,
                             const char *path, size_t *data_len);

  // Called when mongoose is about to serve Lua server page (.lp file), if
  // Lua support is enabled.
  // Parameters:
  //   lua_context: "lua_State *" pointer.
  void (*init_lua)(struct mg_connection *, void *lua_context);

  // Called when mongoose has uploaded a file to a temporary directory as a
  // result of mg_upload() call.
  // Parameters:
  //    file_file: full path name to the uploaded file.
  void (*upload)(struct mg_connection *, const char *file_name);

  // Called when mongoose is about to send HTTP error to the client.
  // Implementing this callback allows to create custom error pages.
  // Parameters:
  //   status: HTTP error status code.
  int  (*http_error)(struct mg_connection *, int status);
};

// Start web server.
//
// Parameters:
//   callbacks: mg_callbacks structure with user-defined callbacks.
//   options: NULL terminated list of option_name, option_value pairs that
//            specify Mongoose configuration parameters.
//
// Side-effects: on UNIX, ignores SIGCHLD and SIGPIPE signals. If custom
//    processing is required for these, signal handlers must be set up
//    after calling mg_start().
//
//
// Example:
//   const char *options[] = {
//     "document_root", "/var/www",
//     "listening_ports", "80,443s",
//     NULL
//   };
//   struct mg_context *ctx = mg_start(&my_func, NULL, options);
//
// Refer to https://github.com/valenok/mongoose/blob/master/UserManual.md
// for the list of valid option and their possible values.
//
// Return:
//   web server context, or NULL on error.
struct mg_context *mg_start(void);


// Stop the web server.
//
// Must be called last, when an application wants to stop the web server and
// release all associated resources. This function blocks until all Mongoose
// threads are stopped. Context pointer becomes invalid.
void mg_stop(struct mg_context *);



// Opcodes, from http://tools.ietf.org/html/rfc6455
enum {
  WEBSOCKET_OPCODE_CONTINUATION = 0x0,
  WEBSOCKET_OPCODE_TEXT = 0x1,
  WEBSOCKET_OPCODE_BINARY = 0x2,
  WEBSOCKET_OPCODE_CONNECTION_CLOSE = 0x8,
  WEBSOCKET_OPCODE_PING = 0x9,
  WEBSOCKET_OPCODE_PONG = 0xa
};


// Macros for enabling compiler-specific checks for printf-like arguments.
#undef PRINTF_FORMAT_STRING
#if _MSC_VER >= 1400
#include <sal.h>
#if _MSC_VER > 1400
#define PRINTF_FORMAT_STRING(s) _Printf_format_string_ s
#else
#define PRINTF_FORMAT_STRING(s) __format_string s
#endif
#else
#define PRINTF_FORMAT_STRING(s) s
#endif

#ifdef __GNUC__
#define PRINTF_ARGS(x, y) __attribute__((format(printf, x, y)))
#else
#define PRINTF_ARGS(x, y)
#endif

// Send data to the client using printf() semantics.
//
// Works exactly like mg_write(), but allows to do message formatting.
int mg_printf(struct mg_connection *,
              PRINTF_FORMAT_STRING(const char *fmt), ...) PRINTF_ARGS(2, 3);


// Close the connection opened by mg_download().
void mg_close_connection(struct mg_connection *conn);




// Convenience function -- create detached thread.
// Return: 0 on success, non-0 on error.
typedef void * (*mg_thread_func_t)(void *);
int mg_start_thread(mg_thread_func_t f, void *p);



// Return Mongoose version.
const char *mg_version(void);


#ifdef __cplusplus
}
#endif // __cplusplus

#endif // MONGOOSE_HEADER_INCLUDED
