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
#include "global.h"
#include "controller.h"
#include "util.h"

#include <termios.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

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
