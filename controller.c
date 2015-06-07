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
#include "client.h"
#include "util.h"

#include <termios.h>
#include <errno.h>

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
 * Starts the device controller that accepts keyboard input on
 * stdin and transmits commands based on the configuration file.
 *
 * @param config Controller configuration object
 * @param status_command Status command
 * @param client_fd Connection to server file descriptor
 * @return True on success, false when some error has ocurred
 */
bool start_manual_controller(ucl_object_t *config, const char *status_command, int client_fd)
{
  struct timespec tsp = {0, 500};
  struct termios attr;
  struct termios *p = &attr;
  int term_fd = fileno(stdin);
  bool ret_flag = true;
  utimer_t timer_refresh_controller = timer_now();
  double status_refresh_interval_sec;
  utimer_t status_refresh_interval_msec;

  const ucl_object_t *commands = ucl_object_find_key(config, "commands");
  if (!commands) {
    fprintf(stderr, "ERROR: Missing 'commands' in configuration file!\n");
    return false;
  }

  const ucl_object_t *interval = ucl_object_find_key(config, "status_interval");
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
      if (!client_request_device_state(client_fd, status_command, true)) {
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
      if (!client_send_device_command(client_fd, action, &response))
        continue;

      if (response) {
        // TODO: Output response for some commands
        free(response);
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
  const ucl_object_t *cfg_server = ucl_object_find_key(config, "server");
  if (!cfg_server) {
    fprintf(stderr, "ERROR: Missing server configuration!\n");
    return false;
  }

  const ucl_object_t *cfg_controller = ucl_object_find_key(config, "controller");
  if (!cfg_controller) {
    fprintf(stderr, "ERROR: Missing controller configuration!\n");
    return false;
  }

  const ucl_object_t *cfg_client = ucl_object_find_key(config, "client");
  if (!cfg_client) {
    fprintf(stderr, "ERROR: Missing client configuration!\n");
    return false;
  }

  const char *status_command;
  ucl_object_t *obj = ucl_object_find_key(cfg_client, "status_command");
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'status_command' in configuration file!\n");
    return false;
  } else if (!ucl_object_tostring_safe(obj, &status_command)) {
    fprintf(stderr, "ERROR: Status command must be a string!\n");
    return false;
  }

  int client_fd = client_connect(cfg_server);
  if (client_fd < 0)
    return false;

  if (status_only) {
    client_request_device_state(client_fd, status_command, false);
  } else {
    fprintf(stderr, "INFO: Controller ready and accepting commands.\n");
    fprintf(stderr, "INFO: Press 'esc' to quit.\n");

    start_manual_controller(cfg_controller, status_command, client_fd);

    fprintf(stderr, "INFO: Closing controller.\n");
  }
}
