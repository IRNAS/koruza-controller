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
#include <errno.h>
#include <time.h>
#include <termios.h>
#include <ucl.h>

// Uncomment this to enable verbose debug output
//#define DEBUG

void show_help(const char *app)
{
  fprintf(stderr, "usage: %s [options]\n", app);
  fprintf(stderr,
    "       -h         this text\n"
    "       -c config  configuration file\n"
  );
}

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

time_t timer_now()
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
    fprintf(stderr, "ERROR: Failed to get monotonic clock, weird things may happen!");
    return -1;
  }
  return ts.tv_sec;
}

int is_timeout(time_t *timer, time_t period)
{
  if (*timer < 0)
    return 0;

  time_t now = timer_now();
  if (now - *timer > period) {
    *timer = now;
    return 1;
  }

  return 0;
}

/**
 * Sends a command to the device and parses the response. The output
 * response buffer will be allocated by this method and must be freed
 * by the caller. In case of an error, the output buffer will be NULL.
 *
 * @param serial_fd Serial port file descriptor
 * @param command Command string to send
 * @param response Output buffer where the response will be stored
 * @return True on success, false when some error has ocurred
 */
bool send_device_command(int serial_fd, const char *command, char **response)
{
  // Initialize response buffer
  *response = NULL;

#ifdef DEBUG
  fprintf(stderr, "DEBUG: Sending command: %s", command);
#endif

  // Request status data from the device
  if (write(serial_fd, command, strlen(command)) < 0) {
    fprintf(stderr, "ERROR: Failed to send command to device!\n");
    fprintf(stderr, "ERROR: %s (%d)!\n", strerror(errno), errno);
    return false;
  }

#ifdef DEBUG
  fprintf(stderr, "DEBUG: Waiting for response header from device.\n");
#endif

  // Parse response from device
  char buffer[4096];
  memset(buffer, 0, sizeof(buffer));
  if (read(serial_fd, buffer, 8) < 0) {
    fprintf(stderr, "ERROR: Failed to read response header from device!\n");
    return false;
  } else if (strcmp(buffer, "#START\r\n") != 0) {
    fprintf(stderr, "ERROR: Failed to parse response header from device!\n");
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

    if (read(serial_fd, buffer_p + buffer_size, 1) < 0) {
      fprintf(stderr, "ERROR: Failed to read from device!\n");
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

#ifdef DEBUG
    fprintf(stderr, "DEBUG: Got response line: %s", buffer);
#endif

    if (strncmp(buffer, "#STOP\n", sizeof(buffer)) == 0) {
#ifdef DEBUG
      fprintf(stderr, "DEBUG: Detected stop message.\n");
#endif
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
 * @param serial_fd Serial port file descriptor
 * @return True on success, false when some error has ocurred
 */
bool request_device_state(int serial_fd)
{
  char *response;
  if (!send_device_command(serial_fd, "A 4\n", &response))
    return false;

  if (response) {
    fprintf(stderr, "--- Current KORUZA State ---\n");
    fprintf(stderr, "%s", response);
    fprintf(stderr, "----------------------------\n");
  }
  return true;
}

/**
 * Starts the device controller that accepts keyboard input on
 * stdin and transmits commands based on the configuration file.
 *
 * @param commands Commands configuration option
 * @param serial_fd Serial port file descriptor
 * @return True on success, false when some error has ocurred
 */
bool start_controller(ucl_object_t *commands, int serial_fd)
{
  struct timespec tsp = {0, 500};
  struct termios attr;
  struct termios *p = &attr;
  int term_fd = fileno(stdin);
  bool ret_flag = true;
  time_t timer_refresh_controller = timer_now();

  fflush(stdout);
  if (!flush_term(term_fd, p))
    return false;

  for (;;) {
    // Periodically request device state
    if (is_timeout(&timer_refresh_controller, 1)) {
      if (!request_device_state(serial_fd)) {
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
      if (!send_device_command(serial_fd, action, &response))
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
 * Entry point.
 */
int main(int argc, char **argv)
{
  // Parse program options
  char *config_file = NULL;

  char c;
  while ((c = getopt(argc, argv, "hc:")) != EOF) {
    switch (c) {
      case 'h': {
        show_help(argv[0]);
        return 1;
      }
      case 'c': config_file = strdup(optarg); break;
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
  int serial_fd = -1;
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

  const char *device;
  int64_t baudrate;

  obj = ucl_object_find_key(config, "device");
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'device' in configuration file!\n");
    ret_value = 2;
    goto cleanup_exit;
  } else if (!ucl_object_tostring_safe(obj, &device)) {
    fprintf(stderr, "ERROR: Device must be a string!\n");
    ret_value = 2;
    goto cleanup_exit;
  }

  obj = ucl_object_find_key(config, "baudrate");
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'baudrate' in configuration file!\n");
    ret_value = 2;
    goto cleanup_exit;
  } else if (!ucl_object_toint_safe(obj, &baudrate)) {
    fprintf(stderr, "ERROR: Baudrate must be an integer!\n");
    ret_value = 2;
    goto cleanup_exit;
  }

  // Open the serial device
  serial_fd = open(device, O_RDWR);
  if (serial_fd == -1) {
    fprintf(stderr, "ERROR: Failed to open the serial device '%s'!\n", device);
    ret_value = 2;
    goto cleanup_exit;
  }

  // Configure the serial device
  struct termios tio;
  if (tcgetattr(serial_fd, &tio) < 0) {
    fprintf(stderr, "ERROR: Failed to configure the serial device!\n");
    ret_value = 2;
    goto cleanup_exit;
  }

  // Configure for RAW I/O and setup baudrate
  cfmakeraw(&tio);
  cfsetispeed(&tio, baudrate);
  cfsetospeed(&tio, baudrate);

  if (tcsetattr(serial_fd, TCSAFLUSH, &tio) < 0) {
    fprintf(stderr, "ERROR: Failed to configure the serial device!\n");
    ret_value = 2;
    goto cleanup_exit;
  }

  fprintf(stderr, "INFO: Controller ready and accepting commands.\n");
  fprintf(stderr, "INFO: Press 'esc' to quit.\n");

  obj = ucl_object_find_key(config, "commands");
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'commands' in configuration file!\n");
    ret_value = 2;
    goto cleanup_exit;
  }
  start_controller(obj, serial_fd);

  fprintf(stderr, "INFO: Closing controller.\n");

cleanup_exit:
  // Cleanup and exit
  if (config)
    ucl_object_free(config);
  if (parser)
    ucl_parser_free(parser);
  if (serial_fd != -1)
    close(serial_fd);
  return ret_value;
}
