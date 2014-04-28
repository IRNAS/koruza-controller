/*
 * Simple KORUZA controller.
 *
 * Copyright (C) 2014 by Jernej Kos <kostko@irnas.eu>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <termios.h>
#include <ucl.h>

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "global.h"
#include "server.h"
#include "util.h"

struct command_queue_t {
  /// Connection that posted the command
  struct connection_context_t *connection;
  /// Queued command
  char *command;
  /// Command lengtgh
  size_t cmd_length;
  /// Next command in queue
  struct command_queue_t *next;
};

struct server_context_t {
  /// Event base
  struct event_base *base;
  /// Request timeout event
  struct event *timeout_event;
  /// Currently active connection (can be NULL)
  struct connection_context_t *active_connection;
  /// Command queue start
  struct command_queue_t *cmd_queue_start;
  /// Command queue tail
  struct command_queue_t *cmd_queue_tail;
  /// Serial device inode path
  const char *serial_device;
  /// Serial device buffer
  struct bufferevent *serial_bev;
  /// Serial port configuration
  struct termios serial_tio;
  /// Current response buffer
  char *response;
  /// Response length
  size_t rsp_length;
  /// Device reset hook
  const char *hook_device_reset;
};

struct connection_context_t {
  /// Server context
  struct server_context_t *server;
  /// Connection buffer
  struct bufferevent *conn_bev;
  /// Currently parsed command
  char command[64];
  /// Current command length
  size_t cmd_length;
};

// Forward declarations
void server_serial_start_response_timer(struct server_context_t *server);
void server_serial_read_cb(struct bufferevent *bev, void *ctx);
void server_serial_event_cb(struct bufferevent *bev, short events, void *ctx);
void server_serial_send_command(struct server_context_t *server, const char *command, size_t length);

/**
 * Creates a new connection context.
 *
 * @param server Parent server context
 * @return Newly created connection context
 */
struct connection_context_t *connection_context_new(struct server_context_t *server)
{
  struct connection_context_t *ctx = (struct connection_context_t*) malloc(sizeof(struct connection_context_t));
  if (!ctx)
    return NULL;

  ctx->server = server;
  memset(ctx->command, 0, sizeof(ctx->command));
  ctx->cmd_length = 0;
  return ctx;
}

/**
 * Frees the connection context.
 *
 * @param ctx Connection context
 */
void connection_context_free(struct connection_context_t *ctx)
{
  if (!ctx)
    return;

  if (ctx->server->active_connection == ctx) {
    // The connection that we are freeing is a currently active connection
    ctx->server->active_connection = NULL;
  }

  bufferevent_free(ctx->conn_bev);
  free(ctx);
}

/**
 * Sends a command to the serial device. If another command is
 * currently being processed, the command is queued for later
 * transmission.
 *
 * @param connection Connection context
 * @param command Command to send
 * @param size Length of command string
 * @return True on success, false if something went wrong
 */
bool server_send_command(struct connection_context_t *connection, const char *command, size_t size)
{
  struct server_context_t *server = connection->server;

  if (server->active_connection != NULL) {
    // Queue command
    struct command_queue_t *cmd = (struct command_queue_t*) malloc(sizeof(struct command_queue_t));
    if (!cmd) {
      syslog(LOG_ERR, "Failed to allocate command context, dropping connection.");
      connection_context_free(connection);
      return false;
    }
    cmd->connection = connection;
    cmd->command = strdup(command);
    cmd->cmd_length = size;
    cmd->next = NULL;

    if (server->cmd_queue_tail == NULL) {
      server->cmd_queue_start = cmd;
    } else {
      server->cmd_queue_tail->next = cmd;
    }

    server->cmd_queue_tail = cmd;

    DEBUG_LOG("DEBUG: Command queued.\n");
  } else {
    // Write command immediately
    server->active_connection = connection;
    server_serial_send_command(server, command, size);
  }

  return true;
}

/**
 * Callback for connection read events.
 *
 * @param bev Buffer event
 * @param ctx Connection context
 */
void server_connection_read_cb(struct bufferevent *bev, void *ctx)
{
  struct connection_context_t *connection = (struct connection_context_t*) ctx;
  int n = bufferevent_read(bev, connection->command + connection->cmd_length, 64 - connection->cmd_length);
  if (n <= 0)
    return;

  connection->cmd_length += n;
  if (connection->cmd_length >= 64) {
    syslog(LOG_ERR, "Protocol error, command too long.");

    // Close the connection
    connection_context_free(connection);
    return;
  } else if (connection->command[connection->cmd_length - 1] == '\n') {
    DEBUG_LOG("DEBUG: Got command: %s", connection->command);

    // Command has been parsed, send (or queue)
    if (!server_send_command(connection, connection->command, connection->cmd_length))
      return;

    memset(connection->command, 0, sizeof(connection->command));
    connection->cmd_length = 0;
  }
}

/**
 * Callback for connection exceptional events.
 *
 * @param bev Buffer event
 * @param events Event mask
 * @param ctx Connection context
 */
void server_connection_event_cb(struct bufferevent *bev, short events, void *ctx)
{
  struct connection_context_t *connection = (struct connection_context_t*) ctx;

  if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    syslog(LOG_INFO, "Connection closed.");
    connection_context_free(connection);
  }
}

/**
 * Callback for accepting new connections.
 *
 * @param listener Connection listener
 * @param fd Accepted connection file descriptor
 * @param address Remote address
 * @param ctx Server context
 */
void server_accept_conn_cb(struct evconnlistener *listener,
                           evutil_socket_t fd,
                           struct sockaddr *address,
                           int socklen,
                           void *ctx)
{
  struct server_context_t *server = (struct server_context_t*) ctx;

  struct connection_context_t *connection = connection_context_new(server);
  if (!connection) {
    syslog(LOG_ERR, "Failed to allocate connection context, dropping connection.");
    // TODO close connection?
    return;
  }

  struct event_base *base = evconnlistener_get_base(listener);
  connection->conn_bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_setcb(connection->conn_bev, server_connection_read_cb, NULL, server_connection_event_cb, connection);
  bufferevent_enable(connection->conn_bev, EV_READ | EV_WRITE);

  syslog(LOG_INFO, "Accepted new connection.");
}

void server_serial_command_done(struct server_context_t *server)
{
  server->rsp_length = 0;

  // Cancel response timeout timer
  if (server->timeout_event)
    evtimer_del(server->timeout_event);

  if (server->cmd_queue_start != NULL) {
    // Dequeue next message and send it to device
    struct command_queue_t *cmd = server->cmd_queue_start;
    server->active_connection = cmd->connection;
    server->cmd_queue_start = cmd->next;
    if (server->cmd_queue_start == NULL)
      server->cmd_queue_tail = NULL;

    server_serial_send_command(server, cmd->command, cmd->cmd_length);
    free(cmd->command);
    free(cmd);
  } else {
    server->active_connection = NULL;
  }
}

/**
 * Performs a serial port reset, aborting any pending commands.
 *
 * @param server Server context
 */
bool server_serial_reset(struct server_context_t *server, bool fail_active)
{
  // Fail the currently active command
  if (fail_active && server->active_connection) {
    bufferevent_write(server->active_connection->conn_bev, "#ERROR\r\n#STOP\r\n", 15);
  }

  // Close serial port
  if (server->serial_bev) {
    bufferevent_free(server->serial_bev);
    server->serial_bev = NULL;
  }

  // Call external script to perform device reset
  if (server->hook_device_reset != NULL) {
    pid_t pid = fork();
    if (pid == 0) {
      execl(server->hook_device_reset, server->hook_device_reset, (char*) NULL);
      exit(1);
    } else {
      int status;
      waitpid(pid, &status, 0);
    }
  }

  int serial_fd = open(server->serial_device, O_RDWR);
  if (serial_fd == -1) {
    syslog(LOG_ERR, "Failed to reopen serial device '%s'!", server->serial_device);
    server_serial_start_response_timer(server);
    return false;
  }

  if (fcntl(serial_fd, F_SETFL, O_NONBLOCK) < 0) {
    syslog(LOG_ERR, "Failed to reconfigure serial port.");
    close(serial_fd);
    server_serial_start_response_timer(server);
    return false;
  }

  if (tcsetattr(serial_fd, TCSAFLUSH, &server->serial_tio) < 0) {
    syslog(LOG_ERR, "Failed to reconfigure serial port!");
    server_serial_start_response_timer(server);
    return false;
  }

  // Listen for serial port I/O
  server->serial_bev = bufferevent_socket_new(server->base, serial_fd, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_setcb(server->serial_bev, server_serial_read_cb, NULL, server_serial_event_cb, server);
  bufferevent_enable(server->serial_bev, EV_READ | EV_WRITE);

  // Process next command in queue (if any)
  if (fail_active) {
    server_serial_command_done(server);
  }

  return true;
}

/**
 * Response timeout callback.
 *
 * @param fd Serial socket
 * @param events Event mask
 * @param ctx Server context
 */
void server_serial_read_response_timeout_cb(evutil_socket_t fd, short events, void *ctx)
{
  struct server_context_t *server = (struct server_context_t*) ctx;
  syslog(LOG_ERR, "Read from serial port timed out, resetting port.");
  server_serial_reset(server, true);
}

/**
 * Starts the response timeout timer.
 *
 * @param server Server context
 */
void server_serial_start_response_timer(struct server_context_t *server)
{
  struct timeval one_sec = { 1, 0 };
  if (!server->timeout_event)
    server->timeout_event = evtimer_new(server->base, server_serial_read_response_timeout_cb, server);
  evtimer_add(server->timeout_event, &one_sec);
  DEBUG_LOG("DEBUG: Scheduled serial read timeout event.\n");
}

/**
 * Sends a command to the underlying serial device.
 *
 * @param server Server context
 * @param command Command to send
 * @param length Command length
 */
void server_serial_send_command(struct server_context_t *server, const char *command, size_t length)
{
  server_serial_start_response_timer(server);

  if (!server->serial_bev && !server_serial_reset(server, false)) {
    syslog(LOG_ERR, "Failed to reset serial port before command, returning error!");

    if (server->active_connection)
      bufferevent_write(server->active_connection->conn_bev, "#ERROR\r\n#STOP\r\n", 15);
  } else {
    bufferevent_write(server->serial_bev, command, length);
    DEBUG_LOG("DEBUG: Next command sent to device: %s", command);
  }
}

/**
 * Callback for serial port read events.
 *
 * @param bev Buffer event
 * @param ctx Server context
 */
void server_serial_read_cb(struct bufferevent *bev, void *ctx)
{
  struct server_context_t *server = (struct server_context_t*) ctx;

  if (server->active_connection == NULL) {
    // Ignore messages that were not requested
    syslog(LOG_WARNING, "Message received but not requested!");

    struct evbuffer *buf = evbuffer_new();
    bufferevent_read_buffer(bev, buf);
    evbuffer_free(buf);
    return;
  }

  char buffer[128];
  for (;;) {
    int n = bufferevent_read(bev, buffer, sizeof(buffer));
    if (n <= 0)
      break;

    DEBUG_LOG("DEBUG: Received: %.*s\n", n, buffer);

    size_t offset = server->rsp_length;
    server->rsp_length += n;
    server->response = realloc(server->response, server->rsp_length + 1);
    memcpy(server->response + offset, buffer, n);
    server->response[server->rsp_length] = 0;

    // Simply pipe the output to the currently active connection
    bufferevent_write(server->active_connection->conn_bev, buffer, n);
  }

  // Detect the end of message
  if (strncmp(server->response + server->rsp_length - 9, "\r\n#STOP\r\n", 9) == 0) {
    DEBUG_LOG("DEBUG: Received end of message from device.\n");
    server_serial_command_done(server);
  }
}

/**
 * Callback for serial port exceptional events.
 *
 * @param bev Buffer event
 * @param events Event mask
 * @param ctx Connection context
 */
void server_serial_event_cb(struct bufferevent *bev, short events, void *ctx)
{
  struct server_context_t *server = (struct server_context_t*) ctx;

  if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    syslog(LOG_ERR, "Error event detected on serial port, resetting port!");
    server_serial_reset(server, true);
  }
}

/**
 * Starts the server.
 *
 * @param config Root configuration object
 * @param log_option Syslog flags
 * @return True on success, false if something went wrong
 */
bool start_server(ucl_object_t *config, int log_option)
{
  int64_t baudrate;
  ucl_object_t *obj = NULL;
  bool ret_value = false;
  int serial_fd = -1;

  // Create the server context
  struct server_context_t ctx;
  ctx.timeout_event = NULL;
  ctx.active_connection = NULL;
  ctx.cmd_queue_start = NULL;
  ctx.cmd_queue_tail = NULL;
  ctx.response = NULL;
  ctx.rsp_length = 0;
  ctx.hook_device_reset = NULL;

  obj = ucl_object_find_key(config, "device");
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'device' in configuration file!\n");
    goto cleanup_exit;
  } else if (!ucl_object_tostring_safe(obj, &ctx.serial_device)) {
    fprintf(stderr, "ERROR: Device must be a string!\n");
    goto cleanup_exit;
  }

  obj = ucl_object_find_key(config, "baudrate");
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'baudrate' in configuration file!\n");
    goto cleanup_exit;
  } else if (!ucl_object_toint_safe(obj, &baudrate)) {
    fprintf(stderr, "ERROR: Baudrate must be an integer!\n");
    goto cleanup_exit;
  }

  // Open the serial device
  serial_fd = open(ctx.serial_device, O_RDWR);
  if (serial_fd == -1) {
    fprintf(stderr, "ERROR: Failed to open the serial device '%s'!\n", ctx.serial_device);
    goto cleanup_exit;
  }

  if (fcntl(serial_fd, F_SETFL, O_NONBLOCK) < 0) {
    fprintf(stderr, "ERROR: Failed to setup the serial device!\n");
    goto cleanup_exit;
  }

  // Configure the serial device
  if (tcgetattr(serial_fd, &ctx.serial_tio) < 0) {
    fprintf(stderr, "ERROR: Failed to configure the serial device!\n");
    goto cleanup_exit;
  }

  // Configure for RAW I/O and setup baudrate
  speed_t speed;
  switch (baudrate) {
    case 50: speed = B50; break;
    case 75: speed = B75; break;
    case 110: speed = B110; break;
    case 134: speed = B134; break;
    case 150: speed = B150; break;
    case 200: speed = B200; break;
    case 300: speed = B300; break;
    case 600: speed = B600; break;
    case 1200: speed = B1200; break;
    case 1800: speed = B1800; break;
    case 2400: speed = B2400; break;
    case 4800: speed = B4800; break;
    case 9600: speed = B9600; break;
    case 19200: speed = B19200; break;
    case 38400: speed = B38400; break;
    case 57600: speed = B57600; break;
    case 115200: speed = B115200; break;
    case 230400: speed = B230400; break;
    default: {
      fprintf(stderr, "ERROR: Invalid baudrate specified!\n");
      goto cleanup_exit;
    }
  }

  cfmakeraw(&ctx.serial_tio);
  cfsetispeed(&ctx.serial_tio, speed);
  cfsetospeed(&ctx.serial_tio, speed);

  if (tcsetattr(serial_fd, TCSAFLUSH, &ctx.serial_tio) < 0) {
    fprintf(stderr, "ERROR: Failed to configure the serial device!\n");
    goto cleanup_exit;
  }

  // Configure hooks
  ucl_object_t *hooks = ucl_object_find_key(config, "hooks");
  if (obj) {
    // Device reset hook
    obj = ucl_object_find_key(hooks, "reset");
    if (obj && !ucl_object_tostring_safe(obj, &ctx.hook_device_reset)) {
      fprintf(stderr, "ERROR: Hook 'reset' must be a string!\n");
      goto cleanup_exit;
    }
  }

  // Open the syslog facility
  openlog("koruza-control", log_option, LOG_DAEMON);
  syslog(LOG_INFO, "KORUZA control daemon starting up.");
  syslog(LOG_INFO, "Connected to device '%s'.", ctx.serial_device);

  if (ctx.hook_device_reset) {
    syslog(LOG_INFO, "Device reset hook configured: %s", ctx.hook_device_reset);
  }

  // Setup the event loop
  struct event_base *base = event_base_new();
  ctx.base = base;

  // Setup the UNIX socket
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;

  obj = ucl_object_find_key(config, "socket");
  const char *socket_path;
  if (!obj) {
    syslog(LOG_ERR, "Missing 'socket' in configuration file!");
    goto cleanup_ev_exit;
  } else if (!ucl_object_tostring_safe(obj, &socket_path)) {
    syslog(LOG_ERR, "Socket path must be a string!");
    goto cleanup_ev_exit;
  }

  strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
  unlink(socket_path);

  struct evconnlistener *listener = evconnlistener_new_bind(
    base, server_accept_conn_cb, &ctx, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
    (struct sockaddr *) &address, sizeof(address)
  );
  if (!listener) {
    syslog(LOG_ERR, "Could not create socket listener!");
    goto cleanup_ev_exit;
  }

  // Listen for serial port I/O
  ctx.serial_bev = bufferevent_socket_new(base, serial_fd, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_setcb(ctx.serial_bev, server_serial_read_cb, NULL, server_serial_event_cb, &ctx);
  bufferevent_enable(ctx.serial_bev, EV_READ | EV_WRITE);

  syslog(LOG_INFO, "Entering dispatch loop.");

  // Enter the event loop
  event_base_dispatch(base);

cleanup_ev_exit:
  event_base_free(base);
cleanup_exit:
  if (serial_fd != -1)
    close(serial_fd);
  return ret_value;
}
