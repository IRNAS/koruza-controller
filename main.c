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

bool start_controller(ucl_object_t *commands, FILE *serial)
{
  struct timespec tsp = {0, 500};
  struct termios attr;
  struct termios *p = &attr;
  int term_fd = fileno(stdin);

  fflush(stdout);
  if (!flush_term(term_fd, p))
   return false;

  for (;;) {
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

      if (fprintf(serial, "%s", action) < 0) {
        fprintf(stderr, "ERROR: Failed to send command to device!\n");
        continue;
      }
    }
  }

  if (tcsetattr(term_fd, TCSADRAIN, p) == -1 && tcsetattr(term_fd, TCSADRAIN, p) == -1 )
    return false;

  return true;
}

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
  FILE *serial = NULL;
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
  serial = fopen(device, "r+");
  if (!serial) {
    fprintf(stderr, "ERROR: Failed to open the serial device '%s'!\n", device);
    ret_value = 2;
    goto cleanup_exit;
  }

  // Setup the serial device for non-blocking mode
  int serial_fd = fileno(serial);
  if (fcntl(serial_fd, F_SETFL, O_NONBLOCK) < 0) {
    fprintf(stderr, "ERROR: Failed to setup the serial device!\n");
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
  start_controller(obj, serial);

  fprintf(stderr, "INFO: Closing controller.\n");

cleanup_exit:
  // Cleanup and exit
  if (config)
    ucl_object_free(config);
  if (parser)
    ucl_parser_free(parser);
  if (serial)
    fclose(serial);
  return ret_value;
}
