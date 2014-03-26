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
#include <errno.h>
#include <time.h>
#include <termios.h>
#include <ucl.h>

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

// Uncomment this to enable verbose debug output
//#define DEBUG

#ifdef DEBUG
#define DEBUG_LOG(fmt, ...) fprintf(stderr, fmt, __VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...)
#endif

int checktty(struct termios *p, int term_fd)
{
  struct termios ck;
  return
    tcgetattr(term_fd, &ck) == 0 &&
    (p->c_lflag == ck.c_lflag) &&
    (p->c_cc[VMIN] == ck.c_cc[VMIN]) &&
    (p->c_cc[VTIME] == ck.c_cc[VMIN]);
}


unsigned char keypress(int term_fd)
{
  unsigned char ch;
  if (read(term_fd, &ch, sizeof ch) != 1)
    return 0;
  return ch;
}

int flush_term(int term_fd, struct termios *p)
{
   struct termios newterm;
   errno = 0;
   tcgetattr(term_fd, p);

   newterm = *p;
   newterm.c_lflag &= ~(ECHO | ICANON);
   newterm.c_cc[VMIN] = 0;
   newterm.c_cc[VTIME] = 0;

  return
    tcgetattr(term_fd, p) == 0 &&
    tcsetattr(term_fd, TCSAFLUSH, &newterm) == 0 &&
    checktty(&newterm, term_fd) != 0;
}

long timer_now()
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
    fprintf(stderr, "ERROR: Failed to get monotonic clock, weird things may happen!");
    return -1;
  }
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int is_timeout(long *timer, long period)
{
  if (*timer < 0)
    return 0;

  long now = timer_now();
  if (now - *timer > period) {
    *timer = now;
    return 1;
  }

  return 0;
}

/**
 * Sends a command to the server and parses the response. The output
 * response buffer will be allocated by this method and must be freed
 * by the caller. In case of an error, the output buffer will be NULL.
 *
 * @param client_fd Connection to server file descriptor
 * @param command Command string to send
 * @param response Output buffer where the response will be stored
 * @return True on success, false when some error has ocurred
 */
bool controller_send_device_command(int client_fd, const char *command, char **response)
{
  // Initialize response buffer
  *response = NULL;

  DEBUG_LOG("DEBUG: Sending command: %s", command);

  // Request status data from the device
  if (write(client_fd, command, strlen(command)) < 0) {
    fprintf(stderr, "ERROR: Failed to send command to server!\n");
    fprintf(stderr, "ERROR: %s (%d)!\n", strerror(errno), errno);
    return false;
  }

  DEBUG_LOG("DEBUG: Waiting for response header from server.\n");

  // Parse response from device
  char buffer[4096];
  memset(buffer, 0, sizeof(buffer));
  if (read(client_fd, buffer, 8) < 0) {
    fprintf(stderr, "ERROR: Failed to read response header from server!\n");
    return false;
  } else if (strcmp(buffer, "#START\r\n") != 0) {
    fprintf(stderr, "ERROR: Failed to parse response header from server!\n");
    fprintf(stderr, "ERROR: Expected '#START', received: %s", buffer);
    return false;
  }

#define MAX_RESPONSE_LINES 128
  int line;
  char *buffer_p = (char*) buffer;
  size_t buffer_size = 0;
  size_t response_size = 0;
  memset(buffer, 0, sizeof(buffer));
  for (line = 0; line < MAX_RESPONSE_LINES;) {
    if (buffer_size >= sizeof(buffer)) {
      fprintf(stderr, "ERROR: Response line longer than %ld bytes!\n", sizeof(buffer));

      free(*response);
      *response = NULL;
      return false;
    }

    if (read(client_fd, buffer_p + buffer_size, 1) < 0) {
      fprintf(stderr, "ERROR: Failed to read from server!\n");
      fprintf(stderr, "ERROR: %s (%d)!\n", strerror(errno), errno);

      free(*response);
      *response = NULL;
      return false;
    } else {
      buffer_size++;
    }

    char last = buffer[buffer_size - 1];
    if (last == '\r') {
      buffer[buffer_size - 1] = 0;
      buffer_size--;
      continue;
    } else if (last != '\n') {
      continue;
    }

    line++;

    DEBUG_LOG("DEBUG: Got response line: %s", buffer);

    if (strncmp(buffer, "#STOP\n", sizeof(buffer)) == 0) {
      DEBUG_LOG("DEBUG: Detected stop message.\n");
      break;
    }

    size_t offset = response_size;
    response_size += buffer_size;
    *response = realloc(*response, response_size + 1);
    memcpy(*response + offset, buffer, buffer_size);
    (*response)[response_size] = 0;

    memset(buffer, 0, buffer_size);
    buffer_size = 0;
  }

  return true;
}

/**
 * Requests device state and prints the response to stdout.
 *
 * @param client_fd Connection to server file descriptor
 * @param format Should the output contain beginning/end formatting
 * @return True on success, false when some error has ocurred
 */
bool controller_request_device_state(int client_fd, bool format)
{
  char *response;
  if (!controller_send_device_command(client_fd, "A 4\n", &response))
    return false;

  if (response) {
    if (format)
      fprintf(stdout, "--- Current KORUZA State ---\n");

    fprintf(stdout, "%s", response);

    if (format)
      fprintf(stdout, "----------------------------\n");
  }
  return true;
}

/**
 * Starts the device controller that accepts keyboard input on
 * stdin and transmits commands based on the configuration file.
 *
 * @param commands Commands configuration option
 * @param client_fd Connection to server file descriptor
 * @return True on success, false when some error has ocurred
 */
bool start_manual_controller(ucl_object_t *config, int client_fd)
{
  struct timespec tsp = {0, 500};
  struct termios attr;
  struct termios *p = &attr;
  int term_fd = fileno(stdin);
  bool ret_flag = true;
  long timer_refresh_controller = timer_now();
  double status_refresh_interval_sec;
  long status_refresh_interval_msec;

  ucl_object_t *commands = ucl_object_find_key(config, "commands");
  if (!commands) {
    fprintf(stderr, "ERROR: Missing 'commands' in configuration file!\n");
    return false;
  }

  ucl_object_t *interval = ucl_object_find_key(config, "status_interval");
  if (!interval) {
    fprintf(stderr, "ERROR: Missing 'status_interval' in configuration file!\n");
    return false;
  } else if (!ucl_object_todouble_safe(interval, &status_refresh_interval_sec)) {
    fprintf(stderr, "ERROR: Status refresh interval must be an integer or double!\n");
    return false;
  }

  status_refresh_interval_msec = (long) (status_refresh_interval_sec * 1000);

  fflush(stdout);
  if (!flush_term(term_fd, p))
    return false;

  for (;;) {
    // Periodically request device state
    if (is_timeout(&timer_refresh_controller, status_refresh_interval_msec)) {
      if (!controller_request_device_state(client_fd, true)) {
        ret_flag = false;
        break;
      }
    }

    nanosleep(&tsp, NULL);
    unsigned char ch = keypress(term_fd);
    if (ch == 0)
      continue;

    char command_key[32] = {0,};
    if (ch == 0x1b) {
      // Special key
      unsigned char ch1 = keypress(term_fd);
      unsigned char ch2 = keypress(term_fd);
      if (ch1 == '[' && ch2 == 'A')
        strcpy(command_key, "up");
      else if (ch1 == '[' && ch2 == 'B')
        strcpy(command_key, "down");
      else if (ch1 == '[' && ch2 == 'C')
        strcpy(command_key, "right");
      else if (ch1 == '[' && ch2 == 'D')
        strcpy(command_key, "left");
      else if (ch1 == 0 && ch2 == 0)
        break;
      else {
        fprintf(stderr, "INFO: Unknown special command '%x%x' ignored.\n", ch1, ch2);
        continue;
      }
    } else if (ch == 10) {
      strcpy(command_key, "enter");
    } else {
      command_key[0] = ch;
    }

    const char *action;
    ucl_object_t *obj = ucl_object_find_key(commands, command_key);
    if (!obj) {
      fprintf(stderr, "WARNING: No binding for key '%s'.\n", command_key);
      continue;
    } else if (!ucl_object_tostring_safe(obj, &action)) {
      fprintf(stderr, "WARNING: Binding for key '%s' is not a valid string!\n", command_key);
    } else {
      int x = strlen(action);
      if (action[x - 1] != '\n')
        fprintf(stderr, "INFO: Sending command: %s\n", action);
      else
        fprintf(stderr, "INFO: Sending command: %s", action);

      char *response;
      if (!controller_send_device_command(client_fd, action, &response))
        continue;

      if (response) {
        // TODO: Output response for some commands
      }
    }
  }

  if (tcsetattr(term_fd, TCSADRAIN, p) == -1 && tcsetattr(term_fd, TCSADRAIN, p) == -1 )
    return false;

  return ret_flag;
}

/**
 * Starts the controller.
 *
 * @param config Root configuration object
 * @param status_only True if only one time status was requested
 * @return True on success, false when some error has ocurred
 */
bool start_controller(ucl_object_t *config, bool status_only)
{
  ucl_object_t *cfg_server = ucl_object_find_key(config, "server");
  if (!cfg_server) {
    fprintf(stderr, "ERROR: Missing server configuration!\n");
    return false;
  }

  ucl_object_t *cfg_controller = ucl_object_find_key(config, "controller");
  if (!cfg_controller) {
    fprintf(stderr, "ERROR: Missing controller configuration!\n");
    return false;
  }

  // Setup the UNIX socket
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;

  ucl_object_t *obj = ucl_object_find_key(cfg_server, "socket");
  const char *socket_path;
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'socket' in configuration file!\n");
    return false;
  } else if (!ucl_object_tostring_safe(obj, &socket_path)) {
    fprintf(stderr, "ERROR: Socket path must be a string!\n");
    return false;
  }

  strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);

  int client_fd;
  if ((client_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    fprintf(stderr, "ERROR: Unable to create UNIX socket!\n");
    return false;
  }

  if (connect(client_fd, (struct sockaddr*) &address, sizeof(address)) == -1) {
    fprintf(stderr, "ERROR: Unable to connect with server!\n");
    return false;
  }

  if (status_only) {
    controller_request_device_state(client_fd, false);
  } else {
    fprintf(stderr, "INFO: Controller ready and accepting commands.\n");
    fprintf(stderr, "INFO: Press 'esc' to quit.\n");

    start_manual_controller(cfg_controller, client_fd);

    fprintf(stderr, "INFO: Closing controller.\n");
  }
}

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
  /// Currently active connection (can be NULL)
  struct connection_context_t *active_connection;
  /// Command queue start
  struct command_queue_t *cmd_queue_start;
  /// Command queue tail
  struct command_queue_t *cmd_queue_tail;
  /// Serial device buffer
  struct bufferevent *serial_bev;
  /// Current response buffer
  char *response;
  /// Response length
  size_t rsp_length;
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
    bufferevent_write(server->serial_bev, command, size);

    DEBUG_LOG("DEBUG: Command sent to device.\n");
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
    server->rsp_length = 0;

    if (server->cmd_queue_start != NULL) {
      // Dequeue next message and send it to device
      struct command_queue_t *cmd = server->cmd_queue_start;
      server->active_connection = cmd->connection;
      server->cmd_queue_start = cmd->next;
      if (server->cmd_queue_start == NULL)
        server->cmd_queue_tail = NULL;

      bufferevent_write(server->serial_bev, cmd->command, cmd->cmd_length);
      DEBUG_LOG("DEBUG: Next command sent to device: %s", cmd->command);
      free(cmd->command);
      free(cmd);
    } else {
      server->active_connection = NULL;
    }
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
  const char *device;
  int64_t baudrate;
  ucl_object_t *obj = NULL;
  bool ret_value = false;
  int serial_fd = -1;

  obj = ucl_object_find_key(config, "device");
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'device' in configuration file!\n");
    goto cleanup_exit;
  } else if (!ucl_object_tostring_safe(obj, &device)) {
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
  serial_fd = open(device, O_RDWR);
  if (serial_fd == -1) {
    fprintf(stderr, "ERROR: Failed to open the serial device '%s'!\n", device);
    goto cleanup_exit;
  }

  if (fcntl(serial_fd, F_SETFL, O_NONBLOCK) < 0) {
    fprintf(stderr, "ERROR: Failed to setup the serial device!\n");
    goto cleanup_exit;
  }

  // Configure the serial device
  struct termios tio;
  if (tcgetattr(serial_fd, &tio) < 0) {
    fprintf(stderr, "ERROR: Failed to configure the serial device!\n");
    goto cleanup_exit;
  }

  // Configure for RAW I/O and setup baudrate
  cfmakeraw(&tio);
  cfsetispeed(&tio, baudrate);
  cfsetospeed(&tio, baudrate);

  if (tcsetattr(serial_fd, TCSAFLUSH, &tio) < 0) {
    fprintf(stderr, "ERROR: Failed to configure the serial device!\n");
    goto cleanup_exit;
  }

  // Open the syslog facility
  openlog("koruza-control", log_option, LOG_DAEMON);
  syslog(LOG_INFO, "KORUZA control daemon starting up.");
  syslog(LOG_INFO, "Connected to device '%s'.", device);

  // Create the server context
  struct server_context_t ctx;
  ctx.active_connection = NULL;
  ctx.cmd_queue_start = NULL;
  ctx.cmd_queue_tail = NULL;
  ctx.response = NULL;
  ctx.rsp_length = 0;

  // Setup the event loop
  struct event_base *base = event_base_new();

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
  bufferevent_setcb(ctx.serial_bev, server_serial_read_cb, NULL, NULL, &ctx);
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

/**
 * Prints help text.
 */
void show_help(const char *app)
{
  fprintf(stderr, "usage: %s [options]\n", app);
  fprintf(stderr,
    "       -h         this text\n"
    "       -c config  configuration file\n"
    "       -s         request status and exit\n"
    "       -d         start server daemon\n"
    "       -f         run in foreground\n"
  );
}

/**
 * Entry point.
 */
int main(int argc, char **argv)
{
  // Parse program options
  char *config_file = NULL;
  bool server = false;
  bool status_only = false;
  int log_option = 0;

  char c;
  while ((c = getopt(argc, argv, "hc:sdf")) != EOF) {
    switch (c) {
      case 'h': {
        show_help(argv[0]);
        return 1;
      }
      case 'c': config_file = strdup(optarg); break;
      case 's': status_only = true; break;
      case 'd': server = true; break;
      case 'f': log_option |= LOG_PERROR; break;
      default: {
        fprintf(stderr, "ERROR: Invalid option %c!\n", c);
        show_help(argv[0]);
        return 1;
      }
    }
  }

  if (config_file == NULL) {
    fprintf(stderr, "ERROR: Config file path argument is required!\n");
    return 1;
  }

  // Load the configuration file
  struct ucl_parser *parser = ucl_parser_new(UCL_PARSER_KEY_LOWERCASE);
  ucl_object_t *config = NULL;
  ucl_object_t *obj = NULL;
  int ret_value = 0;
  if (!parser) {
    fprintf(stderr, "ERROR: Failed to initialize configuration parser!\n");
    return 2;
  }
  if (!ucl_parser_add_file(parser, config_file)) {
    fprintf(stderr, "ERROR: Failed to parse configuration file '%s'!\n", config_file);
    fprintf(stderr, "ERROR: %s\n", ucl_parser_get_error(parser));
    ret_value = 2;
    goto cleanup_exit;
  } else {
    config = ucl_parser_get_object(parser);
  }

  if (server) {
    obj = ucl_object_find_key(config, "server");
    if (!obj) {
      fprintf(stderr, "ERROR: Missing server configuration!\n");
      ret_value = 2;
      goto cleanup_exit;
    }

    // TODO: Fork into background

    start_server(obj, log_option);
  } else {
    start_controller(config, status_only);
  }

cleanup_exit:
  // Cleanup and exit
  if (config)
    ucl_object_free(config);
  if (parser)
    ucl_parser_free(parser);
  return ret_value;
}
