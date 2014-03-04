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

void show_help(const char *app)
{
  fprintf(stderr, "usage: %s [options]\n", app);
  fprintf(stderr,
    "       -h         this text\n"
    "       -d device  serial device\n"
    "       -b rate    device baudrate (default = 115200)\n"
    "       -c config  configuration file\n"
  );
}

int main(int argc, char **argv)
{
  // Parse program options
  char *device = NULL;
  char *config_file = NULL;
  unsigned short timeout = 100;
  unsigned int baudrate = 115200;

  char c;
  while ((c = getopt(argc, argv, "hd:b:c:")) != EOF) {
    switch (c) {
      case 'h': {
        show_help(argv[0]);
        return 1;
      }
      case 'd': device = strdup(optarg); break;
      case 'b': baudrate = atoi(optarg); break;
      case 'c': config_file = strdup(optarg); break;
      default: {
        fprintf(stderr, "ERROR: Invalid option %c!\n", c);
        show_help(argv[0]);
        return 1;
      }
    }
  }

  if (device == NULL || config_file == NULL) {
    fprintf(stderr, "ERROR: Serial device and config file path are required!\n");
    return 1;
  }

  // Open the serial device
  FILE *serial = fopen(device, "r+");
  if (!serial) {
    fprintf(stderr, "ERROR: Failed to open the serial device '%s'!\n", device);
    return 2;
  }

  // Setup the serial device for non-blocking mode
  int serial_fd = fileno(serial);
  if (fcntl(serial_fd, F_SETFL, O_NONBLOCK) < 0) {
    fprintf(stderr, "ERROR: Failed to setup the serial device!\n");
    return 2;
  }

  // Configure the serial device
  struct termios tio;
  if (tcgetattr(serial_fd, &tio) < 0) {
    fprintf(stderr, "ERROR: Failed to configure the serial device!\n");
    return 2;
  }

  // Configure for RAW I/O and setup baudrate
  cfmakeraw(&tio);
  cfsetispeed(&tio, baudrate);
  cfsetospeed(&tio, baudrate);

  if (tcsetattr(serial_fd, TCSAFLUSH, &tio) < 0) {
    fprintf(stderr, "ERROR: Failed to configure the serial device!\n");
    return 2;
  }

  // TODO: controller implementation

  // Cleanup and exit
  fclose(serial);
  return 0;
}
