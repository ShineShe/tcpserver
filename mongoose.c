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

#define _XOPEN_SOURCE 600     // For flockfile() on Li

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>


#include <time.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>

#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdint.h>
#include <inttypes.h>
#include <netdb.h>

#include <pwd.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>

#define closesocket(a) close(a)
#define mg_sleep(x) usleep((x) * 1000)
#define ERRNO errno
#define INVALID_SOCKET (-1)
typedef int SOCKET;

#include "mongoose.h"

#define DEBUG

//#define MONGOOSE_VERSION "3.8"
#define BITBOX_VERSION "0.1"
#define PASSWORDS_FILE_NAME ".htpasswd"
#define CGI_ENVIRONMENT_SIZE 4096
#define MAX_CGI_ENVIR_VARS 64
#define MG_BUF_LEN 8192
#define MAX_REQUEST_SIZE 16384
#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))


#define DEBUG_she(x) do { \
  flockfile(stdout); \
  printf("*** %lu.%p.%s.%d: ", \
         (unsigned long) time(NULL), (void *) pthread_self(), \
         __func__, __LINE__); \
  printf x; \
  putchar('\n'); \
  fflush(stdout); \
  funlockfile(stdout); \
} while (0)

#define DEBUG_PRINTF(x) do { \
  flockfile(stdout); \
  printf x; \
  fflush(stdout); \
  funlockfile(stdout); \
} while (0)



#ifdef DEBUG_TRACE
#undef DEBUG_TRACE
#define DEBUG_TRACE(x)
#else
#if defined(DEBUG)
#define DEBUG_TRACE(x) do { \
  flockfile(stdout); \
  printf("*** %lu.%p.%s.%d: ", \
         (unsigned long) time(NULL), (void *) pthread_self(), \
         __func__, __LINE__); \
  printf x; \
  putchar('\n'); \
  fflush(stdout); \
  funlockfile(stdout); \
} while (0)
#else
#define DEBUG_TRACE(x)
#endif // DEBUG
#endif // DEBUG_TRACE


#define IP_ADDR_STR_LEN 50  // IPv6 hex string is 46 chars

#if !defined(SOMAXCONN)
#define SOMAXCONN 100
#endif


// Unified socket address. For IPv6 support, add IPv6 address structure
// in the union u.
union usa {
  struct sockaddr sa;
  struct sockaddr_in sin;
#if defined(USE_IPV6)
  struct sockaddr_in6 sin6;
#endif
};

// Describes a string (chunk of memory).
struct vec {
  const char *ptr;
  size_t len;
};


// Describes listening socket, or socket which was accept()-ed by the master
// thread and queued for future handling by the worker thread.
struct socket {
  SOCKET sock;          // Listening socket
  union usa lsa;        // Local socket address
  union usa rsa;        // Remote socket address
  unsigned is_ssl:1;    // Is port SSL-ed
  unsigned ssl_redir:1; // Is port supposed to redirect everything to SSL port
};



struct mg_context {
  volatile int stop_flag;         // Should we stop event loop
  struct socket *listening_sockets;
  int num_listening_sockets;

  volatile int num_threads;  // Number of threads
  pthread_mutex_t mutex;     // Protects (max|num)_threads
  pthread_cond_t  cond;      // Condvar for tracking workers terminations

  struct socket queue[20];   // Accepted sockets
  volatile int sq_head;      // Head of the socket queue
  volatile int sq_tail;      // Tail of the socket queue
  pthread_cond_t sq_full;    // Signaled when socket is produced
  pthread_cond_t sq_empty;   // Signaled when socket is consumed
};

struct mg_connection {
//  struct mg_request_info request_info;
  struct mg_context *ctx;
  //SSL *ssl;                   // SSL descriptor
 // SSL_CTX *client_ssl_ctx;    // SSL context for client connections
  struct socket client;       // Connected client
  time_t birth_time;          // Time when request was received
  int64_t num_bytes_sent;     // Total bytes sent to client
  int64_t content_len;        // Content-Length header value
  int64_t consumed_content;   // How many bytes of content have been read
//  char *buf;                  // Buffer for received data
  char *path_info;            // PATH_INFO part of the URL
  int must_close;             // 1 if connection must be closed
  int buf_size;               // Buffer size
  int request_len;            // Size of the request + headers in a buffer
  int data_len;               // Total size of data in a buffer
  int status_code;            // HTTP reply status code, e.g. 200
  int throttle;               // Throttling, bytes/sec. <= 0 means no throttle
  time_t last_throttle_time;  // Last time throttled data was sent
  int64_t last_throttle_bytes;// Bytes sent this second
};


static void sockaddr_to_string(char *buf, size_t len,
                                     const union usa *usa) {
  buf[0] = '\0';
#if defined(USE_IPV6)
  inet_ntop(usa->sa.sa_family, usa->sa.sa_family == AF_INET ?
            (void *) &usa->sin.sin_addr :
            (void *) &usa->sin6.sin6_addr, buf, len);
#elif defined(_WIN32)
  // Only Windoze Vista (and newer) have inet_ntop()
  strncpy(buf, inet_ntoa(usa->sin.sin_addr), len);
#else
  inet_ntop(usa->sa.sa_family, (void *) &usa->sin.sin_addr, buf, len);
#endif
}

static void cry(struct mg_connection *conn,
                PRINTF_FORMAT_STRING(const char *fmt), ...) PRINTF_ARGS(2, 3);

// Print error message to the opened error log stream.
static void cry(struct mg_connection *conn, const char *fmt, ...) {
  char buf[MG_BUF_LEN], src_addr[IP_ADDR_STR_LEN];
  va_list ap;
  FILE *fp;
  time_t timestamp;

  va_start(ap, fmt);
  (void) vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  // Do not lock when getting the callback value, here and below.
  // I suppose this is fine, since function cannot disappear in the
  // same way string option can.
    fp = (FILE *)conn->ctx ;

    if (fp != NULL) {
      flockfile(fp);
      timestamp = time(NULL);

      sockaddr_to_string(src_addr, sizeof(src_addr), &conn->client.rsa);
      fprintf(fp, "[%010lu] [error] [client %s] ", (unsigned long) timestamp,
              src_addr);

      fprintf(fp, "%s", buf);
      fputc('\n', fp);
      funlockfile(fp);
      fclose(fp);
    }

}

// Return fake connection structure. Used for logging, if connection
// is not applicable at the moment of logging.
static struct mg_connection *fc(struct mg_context *ctx) {
  static struct mg_connection fake_connection;
  fake_connection.ctx = ctx;
  return &fake_connection;
}

const char *mg_version(void) {
  return BITBOX_VERSION;
}

static void set_close_on_exec(int fd) {
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}

int mg_start_thread(mg_thread_func_t func, void *param) {
  pthread_t thread_id;
  pthread_attr_t attr;
  int result;

  (void) pthread_attr_init(&attr);
  (void) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  // TODO(lsm): figure out why mongoose dies on Linux if next line is enabled
  // (void) pthread_attr_setstacksize(&attr, sizeof(struct mg_connection) * 5);

  result = pthread_create(&thread_id, &attr, func, param);
  pthread_attr_destroy(&attr);

  return result;
}

static int set_non_blocking_mode(SOCKET sock) {
  int flags;

  flags = fcntl(sock, F_GETFL, 0);
  (void) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  return 0;
}



// Parsed Authorization header

static void close_all_listening_sockets(struct mg_context *ctx) {
  int i;
  for (i = 0; i < ctx->num_listening_sockets; i++) {
    closesocket(ctx->listening_sockets[i].sock);
  }
  free(ctx->listening_sockets);
}

// Valid listening port specification is: [ip_address:]port[s]
// Examples: 80, 443s, 127.0.0.1:3128, 1.2.3.4:8080s
// TODO(lsm): add parsing of the IPv6 address
static int set_port(struct socket *so) {
	memset(so, 0, sizeof(*so));
	so->lsa.sin.sin_family = AF_INET;
	so->lsa.sin.sin_port = htons(9735);
	return 1;
}

static int set_ports_option(struct mg_context *ctx) {
  int on = 1, success = 1;
  struct vec vec;
  struct socket so, *ptr;


    if (!set_port(&so)) {
      cry(fc(ctx), "%s: %.*s: invalid port spec. Expecting list of: %s",
          __func__, (int) vec.len, vec.ptr, "[IP_ADDRESS:]PORT[s|p]");
      success = 0;
    }  else if ((so.sock = socket(so.lsa.sa.sa_family, SOCK_STREAM, 6)) ==
               INVALID_SOCKET ||
               // On Windows, SO_REUSEADDR is recommended only for
               // broadcast UDP sockets
               setsockopt(so.sock, SOL_SOCKET, SO_REUSEADDR,
                          (void *) &on, sizeof(on)) != 0 ||
#if defined(USE_IPV6)
               setsockopt(so.sock, IPPROTO_IPV6, IPV6_V6ONLY, (void *) &off,
                          sizeof(off)) != 0 ||
#endif
               bind(so.sock, &so.lsa.sa, sizeof(so.lsa)) != 0 ||
               listen(so.sock, SOMAXCONN) != 0) {
      cry(fc(ctx), "%s: cannot bind to %.*d: %s", __func__,
          4, 9735, strerror(ERRNO));
      closesocket(so.sock);
      success = 0;
    } else if ((ptr = realloc(ctx->listening_sockets,
                              (ctx->num_listening_sockets + 1) *
                              sizeof(ctx->listening_sockets[0]))) == NULL) {
      closesocket(so.sock);
      success = 0;
    } else {
      set_close_on_exec(so.sock);
      ctx->listening_sockets = ptr;
      ctx->listening_sockets[ctx->num_listening_sockets] = so;
      ctx->num_listening_sockets++;
    }


  printf("[%s] success is %d \n",__func__,success);
  if (!success) {
    close_all_listening_sockets(ctx);
  }

  return success;
}

static void close_socket_gracefully(struct mg_connection *conn) {
  struct linger linger;

  // Set linger option to avoid socket hanging out after close. This prevent
  // ephemeral port exhaust problem under high QPS.
  linger.l_onoff = 1;
  linger.l_linger = 1;
  setsockopt(conn->client.sock, SOL_SOCKET, SO_LINGER,
             (char *) &linger, sizeof(linger));

  // Send FIN to the client
  shutdown(conn->client.sock, SHUT_WR);
  set_non_blocking_mode(conn->client.sock);

  // Now we know that our FIN is ACK-ed, safe to close
  closesocket(conn->client.sock);
}

static void close_connection(struct mg_connection *conn) {
  conn->must_close = 1;

  if (conn->client.sock != INVALID_SOCKET) {
    close_socket_gracefully(conn);
    conn->client.sock = INVALID_SOCKET;
  }
}

void mg_close_connection(struct mg_connection *conn) {
  close_connection(conn);
  free(conn);
}


typedef enum playsrc_type{
	WIFI=0,
	USB=1,
	AUX=2,
	AUX1 = 3,
	OPT = 4,
	COA = 5,
	BL = 6,
	HDMI = 10,
	SHORTCUT = 11,
	SD= 9
}playsrc_t;	

typedef enum networkType_t{
	TCP_SWITCHING,
	TCP_AP,
	TCP_STA,	
	TCP_NONE,
}networkType_t;	

typedef struct msg_header{
	int cmdType;
	playsrc_t audioInputSource;
	networkType_t  networkType;
	unsigned short totalPage;/*total page */	
	unsigned short playindex;/*current play index */	
	unsigned short pageIndex;/*current paging Index*/
	unsigned short curplayPage;/*curplayPage  */	
	char ssid[20];
	char key[20];
	char playListId[64];
	int playlistlen;
	int playmode;
}msg_header_t;


static int data_recv(int fd, void *buf, int len)
{
	/*********************************/
	int n=0;
	int nrecved=0;
	/*********************************/

	/** 1s delay
	 *	when the client doesn't shut down,
	 *	and waiting for the data,recv
	 *	will block.
	 struct timeval tv_out;  
	 tv_out.tv_sec = 4;
	tv_out.tv_usec = 0;

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_out, sizeof(tv_out));
	**/

	while((n=recv(fd,buf+nrecved,len-nrecved,0))> 0) {
		nrecved += n;
//		printf(RED"[debug]nread is %d,n is %d\r\n"NONE,nrecved,n);
		if(nrecved == len) {
			break;/*break for recv block*/
		}
	}
	return nrecved;
}


static void process_new_connection(struct mg_connection *conn) {
  conn->data_len = 0;
  DEBUG_TRACE(("[%s] is start \n", __func__));
  do {
 #if 1 
	 char buf[sizeof(msg_header_t)];
	 int num;
	 
	 num = data_recv(conn->client.sock,(void *)buf,sizeof(msg_header_t));
	 msg_header_t msgRecved;
	 memcpy(&msgRecved,buf,sizeof(msg_header_t));
	 
	 DEBUG_TRACE(("[%s] num:%d msgRecved.cmdType is %d \n", __func__,num,msgRecved.cmdType));


 #else
    if (!getreq(conn, ebuf, sizeof(ebuf))) {
      send_http_error(conn, 500, "Server Error", "%s", ebuf);
      conn->must_close = 1;
    } else if (!is_valid_uri(conn->request_info.uri)) {
      snprintf(ebuf, sizeof(ebuf), "Invalid URI: [%s]", ri->uri);
      send_http_error(conn, 400, "Bad Request", "%s", ebuf);
    } else if (strcmp(ri->http_version, "1.0") &&
               strcmp(ri->http_version, "1.1")) {
      snprintf(ebuf, sizeof(ebuf), "Bad HTTP version: [%s]", ri->http_version);
      send_http_error(conn, 505, "Bad HTTP version", "%s", ebuf);
    }

    if (ebuf[0] == '\0') {
      handle_request(conn);
      if (conn->ctx->callbacks.end_request != NULL) {
        conn->ctx->callbacks.end_request(conn, conn->status_code);
      }
      log_access(conn);
    }
    if (ri->remote_user != NULL) {
      free((void *) ri->remote_user);
      // Important! When having connections with and without auth
      // would cause double free and then crash
      ri->remote_user = NULL;
    }

    // NOTE(lsm): order is important here. should_keep_alive() call
    // is using parsed request, which will be invalid after memmove's below.
    // Therefore, memorize should_keep_alive() result now for later use
    // in loop exit condition.
    keep_alive = conn->ctx->stop_flag == 0 && keep_alive_enabled &&
      conn->content_len >= 0 && should_keep_alive(conn);

    // Discard all buffered data for this request
    discard_len = conn->content_len >= 0 && conn->request_len > 0 &&
      conn->request_len + conn->content_len < (int64_t) conn->data_len ?
      (int) (conn->request_len + conn->content_len) : conn->data_len;
    assert(discard_len >= 0);
    memmove(conn->buf, conn->buf + discard_len, conn->data_len - discard_len);
    conn->data_len -= discard_len;
    assert(conn->data_len >= 0);
    assert(conn->data_len <= conn->buf_size);
#endif
    
  } while (0);
}

// Worker threads take accepted socket from the queue
static int consume_socket(struct mg_context *ctx, struct socket *sp) {
  (void) pthread_mutex_lock(&ctx->mutex);
  DEBUG_TRACE(("going idle"));

  // If the queue is empty, wait. We're idle at this point.
  while (ctx->sq_head == ctx->sq_tail && ctx->stop_flag == 0) {
    pthread_cond_wait(&ctx->sq_full, &ctx->mutex);
  }

  // If we're stopping, sq_head may be equal to sq_tail.
  if (ctx->sq_head > ctx->sq_tail) {
    // Copy socket from the queue and increment tail
    *sp = ctx->queue[ctx->sq_tail % ARRAY_SIZE(ctx->queue)];
    ctx->sq_tail++;
    DEBUG_TRACE(("grabbed socket %d, going busy", sp->sock));

    // Wrap pointers if needed
    while (ctx->sq_tail > (int) ARRAY_SIZE(ctx->queue)) {
      ctx->sq_tail -= ARRAY_SIZE(ctx->queue);
      ctx->sq_head -= ARRAY_SIZE(ctx->queue);
    }
  }

  (void) pthread_cond_signal(&ctx->sq_empty);
  (void) pthread_mutex_unlock(&ctx->mutex);

  return !ctx->stop_flag;
}

static void *worker_thread(void *thread_func_param) {
  struct mg_context *ctx = thread_func_param;
  struct mg_connection *conn;

  conn = (struct mg_connection *) calloc(1, sizeof(*conn)/* + MAX_REQUEST_SIZE*/);
  if (conn == NULL) {
    cry(fc(ctx), "%s", "Cannot create new connection struct, OOM");
  } else {
//    conn->buf_size = MAX_REQUEST_SIZE;
//    conn->buf = (char *) (conn + 1);
    conn->ctx = ctx;
//    conn->request_info.user_data = ctx->user_data;

    // Call consume_socket() even when ctx->stop_flag > 0, to let it signal
    // sq_empty condvar to wake up the master waiting in produce_socket()
    while (consume_socket(ctx, &conn->client)) {
      conn->birth_time = time(NULL);

      // Fill in IP, port info early so even if SSL setup below fails,
      // error handler would have the corresponding info.
      // Thanks to Johannes Winkelmann for the patch.
      // TODO(lsm): Fix IPv6 case
//      conn->request_info.remote_port = ntohs(conn->client.rsa.sin.sin_port);
//      memcpy(&conn->request_info.remote_ip,
//             &conn->client.rsa.sin.sin_addr.s_addr, 4);
//      conn->request_info.remote_ip = ntohl(conn->request_info.remote_ip);
      process_new_connection(conn);

      close_connection(conn);
    }
    free(conn);
  }

  // Signal master that we're done with connection and exiting
  (void) pthread_mutex_lock(&ctx->mutex);
  ctx->num_threads--;
  (void) pthread_cond_signal(&ctx->cond);
  assert(ctx->num_threads >= 0);
  (void) pthread_mutex_unlock(&ctx->mutex);

  DEBUG_TRACE(("exiting"));
  return NULL;
}

// Master thread adds accepted socket to a queue
static void produce_socket(struct mg_context *ctx, const struct socket *sp) {
  (void) pthread_mutex_lock(&ctx->mutex);

  // If the queue is full, wait
  while (ctx->stop_flag == 0 &&
         ctx->sq_head - ctx->sq_tail >= (int) ARRAY_SIZE(ctx->queue)) {
    (void) pthread_cond_wait(&ctx->sq_empty, &ctx->mutex);
  }

  if (ctx->sq_head - ctx->sq_tail < (int) ARRAY_SIZE(ctx->queue)) {
    // Copy socket to the queue and increment head
    ctx->queue[ctx->sq_head % ARRAY_SIZE(ctx->queue)] = *sp;
    ctx->sq_head++;
    DEBUG_TRACE(("queued socket %d", sp->sock));
  }

  (void) pthread_cond_signal(&ctx->sq_full);
  (void) pthread_mutex_unlock(&ctx->mutex);
}

static int set_sock_timeout(SOCKET sock, int milliseconds) {
#ifdef _WIN32
  DWORD t = milliseconds;
#else
  struct timeval t;
  t.tv_sec = milliseconds / 1000;
  t.tv_usec = (milliseconds * 1000) % 1000000;
#endif
  return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void *) &t, sizeof(t)) ||
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (void *) &t, sizeof(t));
}

static void accept_new_connection(const struct socket *listener,
                                  struct mg_context *ctx) {
  struct socket so;
  socklen_t len = sizeof(so.rsa);
  int on = 1;

  if ((so.sock = accept(listener->sock, &so.rsa.sa, &len)) == INVALID_SOCKET) {
  }  else {
    // Put so socket structure into the queue
    DEBUG_TRACE(("Accepted socket %d", (int) so.sock));

    getsockname(so.sock, &so.lsa.sa, &len);
    // Set TCP keep-alive. This is needed because if HTTP-level keep-alive
    // is enabled, and client resets the connection, server won't get
    // TCP FIN or RST and will keep the connection open forever. With TCP
    // keep-alive, next keep-alive handshake will figure out that the client
    // is down and will close the server end.
    // Thanks to Igor Klopov who suggested the patch.
    setsockopt(so.sock, SOL_SOCKET, SO_KEEPALIVE, (void *) &on, sizeof(on));
    set_sock_timeout(so.sock, 4000);
    DEBUG_TRACE(("produce_socket %d", (int) so.sock));    
    produce_socket(ctx, &so);
  }
}

static void *master_thread(void *thread_func_param) {
  struct mg_context *ctx = thread_func_param;
  struct pollfd *pfd;
  int i;

  pfd = calloc(ctx->num_listening_sockets, sizeof(pfd[0]));
  while (pfd != NULL && ctx->stop_flag == 0) {
    for (i = 0; i < ctx->num_listening_sockets; i++) {
      pfd[i].fd = ctx->listening_sockets[i].sock;
      pfd[i].events = POLLIN;
    }

    if (poll(pfd, ctx->num_listening_sockets, 200) > 0) {
      for (i = 0; i < ctx->num_listening_sockets; i++) {
        // NOTE(lsm): on QNX, poll() returns POLLRDNORM after the
        // successfull poll, and POLLIN is defined as (POLLRDNORM | POLLRDBAND)
        // Therefore, we're checking pfd[i].revents & POLLIN, not
        // pfd[i].revents == POLLIN.
        if (ctx->stop_flag == 0 && (pfd[i].revents & POLLIN)) {
          accept_new_connection(&ctx->listening_sockets[i], ctx);
        }
      }
    }
  }
  free(pfd);
  DEBUG_TRACE(("stopping workers"));

  // Stop signal received: somebody called mg_stop. Quit.
  close_all_listening_sockets(ctx);

  // Wakeup workers that are waiting for connections to handle.
  pthread_cond_broadcast(&ctx->sq_full);

  // Wait until all threads finish
  (void) pthread_mutex_lock(&ctx->mutex);
  while (ctx->num_threads > 0) {
    (void) pthread_cond_wait(&ctx->cond, &ctx->mutex);
  }
  (void) pthread_mutex_unlock(&ctx->mutex);

  // All threads exited, no sync is needed. Destroy mutex and condvars
  (void) pthread_mutex_destroy(&ctx->mutex);
  (void) pthread_cond_destroy(&ctx->cond);
  (void) pthread_cond_destroy(&ctx->sq_empty);
  (void) pthread_cond_destroy(&ctx->sq_full);

  DEBUG_TRACE(("exiting"));

  // Signal mg_stop() that we're done.
  // WARNING: This must be the very last thing this
  // thread does, as ctx becomes invalid after this line.
  ctx->stop_flag = 2;
  return NULL;
}

static void free_context(struct mg_context *ctx) {
  // Deallocate context itself
  free(ctx);
}

void mg_stop(struct mg_context *ctx) {
  ctx->stop_flag = 1;

  // Wait until mg_fini() stops
  while (ctx->stop_flag != 2) {
    (void) mg_sleep(10);
  }
  free_context(ctx);
}

struct mg_context *mg_start(void) {
  struct mg_context *ctx;
  int i;

  // Allocate context and initialize reasonable general case defaults.
  // TODO(lsm): do proper error handling here.
  if ((ctx = (struct mg_context *) calloc(1, sizeof(*ctx))) == NULL) {
    return NULL;
  }
  (void) pthread_mutex_init(&ctx->mutex, NULL);
  (void) pthread_cond_init(&ctx->cond, NULL);
  (void) pthread_cond_init(&ctx->sq_empty, NULL);
  (void) pthread_cond_init(&ctx->sq_full, NULL);

  // Start master (listening) thread

  if (!set_ports_option(ctx) )
	return NULL;

  mg_start_thread(master_thread, ctx);

  // Start worker threads
  for (i = 0; i < 20; i++) {
    if (mg_start_thread(worker_thread, ctx) != 0) {
      cry(fc(ctx), "Cannot start worker thread: %ld", (long) ERRNO);
    } else {
      ctx->num_threads++;
    }
  }

  return ctx;
}
