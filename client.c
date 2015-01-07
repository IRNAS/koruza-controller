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
#include "client.h"
#include "util.h"

#include <termios.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>

/**
 * Establishes a connection with the control server.
 *
 * @param cfg_server Server configuration object
 * @return Socket file descriptor
 */
int client_connect(ucl_object_t *cfg_server)
{
  // Install signal handlers
  signal(SIGPIPE, SIG_IGN);

  // Setup the UNIX socket
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;

  ucl_object_t *obj = ucl_object_find_key(cfg_server, "socket");
  const char *socket_path;
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'socket' in configuration file!\n");
    return -1;
  } else if (!ucl_object_tostring_safe(obj, &socket_path)) {
    fprintf(stderr, "ERROR: Socket path must be a string!\n");
    return -1;
  }

  strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);

  int client_fd;
  if ((client_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    fprintf(stderr, "ERROR: Unable to create UNIX socket!\n");
    return -1;
  }

  if (connect(client_fd, (struct sockaddr*) &address, sizeof(address)) == -1) {
    fprintf(stderr, "ERROR: Unable to connect with server!\n");
    return -1;
  }

  return client_fd;
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
bool client_send_device_command(int client_fd, const char *command, char **response)
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

  DEBUG_LOG("DEBUG: Waiting for response from server.\n");

  // Parse response from device
#define MAX_RESPONSE_LINES 128
  bool result = true;
  char buffer[4096];
  int line;
  char *buffer_p = (char*) buffer;
  size_t buffer_size = 0;
  size_t response_size = 0;
  bool received_header = false;
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

    if (strncmp(buffer, "#START\n", sizeof(buffer)) == 0) {
      DEBUG_LOG("DEBUG: Detected start message.\n");
      memset(buffer, 0, buffer_size);
      buffer_size = 0;
      received_header = true;
      continue;
    } else if (strncmp(buffer, "#ERROR\n", sizeof(buffer)) == 0) {
      DEBUG_LOG("DEBUG: Detected error message.\n");
      memset(buffer, 0, buffer_size);
      buffer_size = 0;
      received_header = true;
      result = false;
      continue;
    } else if (strncmp(buffer, "#STOP\n", sizeof(buffer)) == 0) {
      DEBUG_LOG("DEBUG: Detected stop message.\n");
      break;
    }

    if (!received_header) {
      fprintf(stderr, "WARNING: Received response line before header start:\n");
      fprintf(stderr, "WARNING: %s", buffer);
      memset(buffer, 0, buffer_size);
      buffer_size = 0;
      continue;
    }

    size_t offset = response_size;
    response_size += buffer_size;
    *response = realloc(*response, response_size + 1);
    memcpy(*response + offset, buffer, buffer_size);
    (*response)[response_size] = 0;

    memset(buffer, 0, buffer_size);
    buffer_size = 0;
  }

  // Prevent NULL responses from being propagated
  if (result && *response == NULL)
    result = false;

  return result;
}

/**
 * Requests device state and prints the response to stdout.
 *
 * @param client_fd Connection to server file descriptor
 * @param format Should the output contain beginning/end formatting
 * @return True on success, false when some error has ocurred
 */
bool client_request_device_state(int client_fd, const char *command, bool format)
{
  char *response;
  if (!client_send_device_command(client_fd, command, &response))
    return false;

  if (response) {
    if (format)
      fprintf(stdout, "--- Current KORUZA State ---\n");

    fprintf(stdout, "%s", response);

    if (format)
      fprintf(stdout, "----------------------------\n");
  }

  free(response);
  return true;
}
