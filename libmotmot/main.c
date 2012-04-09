#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <glib.h>
#include <msgpack.h>
#include <readline/readline.h>

#define SOCKDIR   "conn/"
#define MAXCONNS  100

#define err(cond, errstr)   \
  if (cond) {               \
    perror(errstr);         \
    exit(1);                \
  }

GMainLoop *gmain;

struct {
  int pid;
  GIOChannel *channel;
} conns[MAXCONNS];

/**
 * Create a local UNIX socket and wrap it in a GIOChannel.
 *
 * All our socket files are identified by conn/xxxxxx, where xxxxxx is the
 * pid of the listening process.  Passing pid > 0 to create_socket will
 * open a connection to the corresponding file; otherwise, a listening
 * socket file will be created with the pid of the calling process.
 */
GIOChannel *
create_socket_channel(int pid)
{
  int s;
  char *filename;
  struct sockaddr_un *saddr;
  GIOChannel *channel;
  GError *gerr;

  // 14 = sizeof(short) + "conn/000000".
  saddr = g_malloc0(14);
  saddr->sun_family = AF_UNIX;

  // "Six digits should be enough for a pid."
  sprintf(saddr->sun_path, SOCKDIR "%06d", (pid > 0) ? pid : getpid());

  // Open a socket.
  s = socket(PF_LOCAL, SOCK_STREAM, 0);
  err(s < 0, "socket");

  if (pid > 0) {
    // Connect to conn/pid.
    err(connect(s, (struct sockaddr *)saddr, 14) < 0, "connect");
  } else {
    // Listen on conn/pid.
    err(bind(s, (struct sockaddr *)saddr, 14) < 0, "bind");
    err(listen(s, 5) < 0, "listen");
  }

  gerr = NULL;
  channel = g_io_channel_unix_new(s);
  if (g_io_channel_set_encoding(channel, NULL, &gerr) == G_IO_STATUS_ERROR) {
    // TODO: error handling
    printf("create_socket_channel: Failed to set channel encoding.\n");
  }

  g_free(saddr);
  return channel;
}

int
socket_recv(GIOChannel *source, GIOCondition condition, void *data)
{
  char *buf;
  unsigned long len;
  msgpack_unpacked msg;
  GError *gerr;

  // Read a line from the socket.
  gerr = NULL;
  if (g_io_channel_read_line(source, &buf, &len, NULL, &gerr) ==
      G_IO_STATUS_ERROR) {
    // TODO: error handling
    printf("socket_recv: Could not read line from socket.\n");
  }

  // Print message to stdout.
  msgpack_unpacked_init(&msg);
  if (!msgpack_unpack_next(&msg, buf, len, NULL)) {
    // TODO: error handling
    printf("socket_recv: Could not unpack message.\n");
  }
  msgpack_object_print(stdout, msg.data);

  return TRUE;
}

int
socket_accept(GIOChannel *source, GIOCondition condition, void *data)
{
  int fd, newfd, len;
  struct sockaddr_un *saddr;
  GIOChannel *channel;
  GError *gerr;

  fd = g_io_channel_unix_get_fd(source);
  saddr = g_new0(struct sockaddr_un, 1);

  // Spin off a new socket.
  newfd = accept(fd, (struct sockaddr *)saddr, &len);
  err(newfd < 0, "accept");

  // Wrap it in a channel and watch it.
  channel = g_io_channel_unix_new(newfd);
  if (g_io_channel_set_encoding(channel, NULL, &gerr) == G_IO_STATUS_ERROR) {
    // TODO: error handling
    printf("socket_accept: Failed to set channel encoding.\n");
  }

  // TODO: pass in some data here
  g_io_add_watch(channel, G_IO_IN, socket_recv, NULL);

  g_free(saddr);
  return TRUE;
}

int
poll_conns(void *data)
{
  int i, pid;
  bool found;
  DIR *dp;
  struct dirent *ep;
  GIOChannel *channel;

  // Open up conn/.
  dp = opendir(SOCKDIR);
  err(dp == NULL, "opendir");

  // Search through all connections.
  while (ep = readdir(dp)) {
    // Ignore . and .. and any files that shouldn't exist.
    pid = atoi(ep->d_name);
    if (pid == 0 || pid == getpid()) {
      continue;
    }

    // Check whether we have already connected to the socket.
    found = false;
    for (i = 0; conns[i].pid != 0; ++i) {
      if (pid == conns[i].pid) {
        found = true;
        break;
      }
    }

    // Create a new socket channel for any new connections discovered.
    if (!found) {
      conns[i].pid = pid;
      conns[i].channel = create_socket_channel(pid);
    }
  }

  closedir(dp);
  return TRUE;
}

int
input_loop(void *data)
{
  int i, pid;
  char *msg;
  unsigned long len;
  msgpack_sbuffer *buf;
  msgpack_packer *pk;
  GError *gerr;

  // Read in a line and pack it.
  msg = readline("> ");

  buf = msgpack_sbuffer_new();
  pk = msgpack_packer_new(buf, msgpack_sbuffer_write);

  msgpack_pack_raw(pk, strlen(msg));
  msgpack_pack_raw_body(pk, msg, strlen(msg));

  // Broadcast message.
  for (i = 0, pid = getpid(); conns[i].pid != 0; ++i) {
    // Skip ourselves.
    if (pid == conns[i].pid) {
      continue;
    }

    // Send message over the wire.
    gerr = NULL;
    if (g_io_channel_write_chars(conns[i].channel, buf->data, buf->size,
                                 &len, &gerr) == G_IO_STATUS_ERROR) {
      // TODO: error handling
      printf("input_loop: Could not write message to socket.\n");
    }
  }

  // Free all the things.
  msgpack_packer_free(pk);
  msgpack_sbuffer_free(buf);
  free(msg);

  return TRUE;
}

int
main(int argc, char *argv[])
{
  GIOChannel *channel;

  // Set up the main event loop.
  gmain = g_main_loop_new(g_main_context_default(), FALSE);

  // Create our server / listening socket.
  channel = create_socket_channel(0);
  g_io_add_watch(channel, G_IO_IN, socket_accept, NULL);

  // Poll the directory for new sockets every second.
  g_timeout_add_seconds(1, poll_conns, NULL);

  // Add the input loop as an idle source.
  g_idle_add(input_loop, NULL);

  g_main_loop_run(gmain);
}
