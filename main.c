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
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <ucl.h>

#include "global.h"
#include "server.h"
#include "controller.h"

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
