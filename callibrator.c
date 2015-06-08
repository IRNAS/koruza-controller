/*
 * Simple KORUZA controller.
 *
 * Copyright (C) 2015 by Jernej Kos <kostko@irnas.eu>
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

#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <syslog.h>

bool fetch_callibration_data(const char *host, char *response, size_t length)
{
  // Resolve hostname.
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0;
  hints.ai_protocol = 0;

  if (getaddrinfo(host, "80", &hints, &result) != 0) {
    return false;
  }

  int fd;
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      continue;

    // Configure socket timeouts.
    struct timespec timeout = {5, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
      close(fd);
      continue;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
      close(fd);
      continue;
    }

    // Connect to remote host.
    if (connect(fd, rp->ai_addr, rp->ai_addrlen) != -1)
      break;

    close(fd);
  }

  if (rp == NULL)
    return false;

  // Send HTTP request.
  const char *request = "GET /koruza/last_state HTTP/1.0\r\nConnection: close\r\n\r\n";
  if (write(fd, request, strlen(request)) != strlen(request)) {
    close(fd);
    return false;
  }

  // Callibration data should not be more than 1024 bytes together with HTTP headers.
  char buffer[1024] = {0,};
  size_t offset = 0;
  for (;;) {
    int bytes = read(fd, buffer + offset, sizeof(buffer) - offset);
    if (bytes <= 0)
      break;

    offset += bytes;
    if (offset >= sizeof(buffer))
      break;
  }

  // Get the callibration value.
  const char *body = strstr(buffer, "\r\n\r\n");
  if (body == NULL) {
    close(fd);
    return false;
  }

  strncpy(response, body + 4, length);

  close(fd);
  return true;
}

/**
 * Starts the callibrator.
 *
 * @param config Root configuration object
 * @param log_option Syslog flags
 * @return True on success, false when some error has ocurred
 */
bool start_callibrator(ucl_object_t *config, int log_option)
{
  const ucl_object_t *cfg_server = ucl_object_find_key(config, "server");
  if (!cfg_server) {
    fprintf(stderr, "ERROR: Missing server configuration!\n");
    return false;
  }

  const ucl_object_t *cfg_callibrator = ucl_object_find_key(config, "callibrator");
  if (!cfg_callibrator) {
    fprintf(stderr, "ERROR: Missing callibrator configuration!\n");
    return false;
  }

  utimer_t timer_recallibrate = timer_now();
  double interval_sec;
  utimer_t interval_msec;

  const ucl_object_t *interval = ucl_object_find_key(cfg_callibrator, "interval");
  if (!interval) {
    fprintf(stderr, "ERROR: Missing 'interval' in configuration file!\n");
    return false;
  } else if (!ucl_object_todouble_safe(interval, &interval_sec)) {
    fprintf(stderr, "ERROR: Interval must be an integer or double!\n");
    return false;
  }

  interval_msec = (long) (interval_sec * 1000);

  const ucl_object_t *cfg_tokens = ucl_object_find_key(cfg_callibrator, "tokens");
  if (!cfg_tokens) {
    fprintf(stderr, "ERROR: Missing 'tokens' in configuration file!\n");
    return false;
  }

  const char *host;
  const ucl_object_t *cfg_host = ucl_object_find_key(cfg_callibrator, "host");
  if (!cfg_host) {
    fprintf(stderr, "ERROR: Missing 'host' in configuration file!\n");
    return false;
  } else if (!ucl_object_tostring_safe(cfg_host, &host)) {
    fprintf(stderr, "ERROR: Callibration host must be a string!\n");
    return false;
  }

  int client_fd = client_connect(cfg_server);

  // Open the syslog facility
  openlog("koruza-callibrator", 0, LOG_DAEMON);
  syslog(LOG_INFO, "KORUZA callibrator daemon starting up.");

  struct timespec tsp = {0, 10000000};
  size_t cmd_failures = 0;

  for (;;) {
    nanosleep(&tsp, NULL);

    // Periodically perform recallibration
    if (is_timeout(&timer_recallibrate, interval_msec)) {
      char value[1024] = {0,};
      // Request callibration information from given host.
      if (!fetch_callibration_data(host, value, sizeof(value))) {
        syslog(LOG_ERR, "Failed to fetch callibration data from '%s'.", host);
        continue;
      }

      // Tokenize string by spaces.
      char *token = NULL;
      char *tmp;
      int index;
      for (index = 1, tmp = value; ; tmp = NULL, index++) {
        token = strtok(tmp, " ");
        if (token == NULL)
          break;

        // Check if this token is configured to execute any command.
        char key[64] = {0,};
        snprintf(key, sizeof(key), "%d", index);

        const char *callibration_command;
        const ucl_object_t *cfg_callibration_command = ucl_object_find_key(cfg_tokens, key);
        if (!cfg_callibration_command) {
          continue;
        } else if (!ucl_object_tostring_safe(cfg_callibration_command, &callibration_command)) {
          syslog(LOG_ERR, "Callibration command for token %d must be a string!", index);
          continue;
        }

        // Execute callibration command locally.
        char *response;
        char command[256] = {0,};
        snprintf(command, sizeof(command), callibration_command, token);
        if (!client_send_device_command(client_fd, command, &response)) {
          syslog(LOG_WARNING, "Failed to communicate with the control daeamon!");

          if (++cmd_failures > 5) {
            syslog(LOG_ERR, "Multiple failures while recallibrating, reconnecting...");
            close(client_fd);
            client_fd = client_connect(cfg_server);
            cmd_failures = 0;
          }
          continue;
        }

        free(response);
      }
    }
  }
}
