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

#include "uthash/uthash.h"

#include <termios.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <zlib.h>

struct collector_cfg_t {
  /// Name format string
  const char *of_name;
  /// Value format string
  const char *of_value;
};

struct log_item_t {
  /// Unique item key
  char *key;
  /// Unique item short key
  int key_short;
  /// Last stored value
  double last;
  /// Number of stored values
  size_t count;
  /// Sum of stored values
  double sum;
  /// Maximum of stored values
  double max;
  /// Minimum of stored values
  double min;

  UT_hash_handle hh;
};

double collector_get_time()
{
  struct timeval tv;
  if (!gettimeofday(&tv, NULL)) {
    return tv.tv_sec + ((double) tv.tv_usec / 1000000.0);
  }

  return (double) time(NULL);
}

void collector_parse_response(struct collector_cfg_t *cfg,
                              struct log_item_t **log_table,
                              const char *response,
                              gzFile log,
                              FILE *state,
                              FILE *last_state)
{
  // Do not attempt to parse NULL responses
  if (!response)
    return;

  char *rsp = strdup(response);
  char *rsp_tok = rsp;

  ftruncate(fileno(state), 0);
  rewind(state);
  if (last_state != NULL) {
    ftruncate(fileno(last_state), 0);
    rewind(last_state);
    fprintf(last_state, "%d", time(NULL));
  }

  // Each line in the form of <key>: <double> is a valid response
  for (;; rsp_tok = NULL) {
    char *line = strtok(rsp_tok, "\n");
    if (!line)
      break;

    char key[256] = {0,};
    int key_short = -1;
    char op[128] = {0,};
    char value_str[256] = {0,};
    double value;
    bool metadata = false;

    if (sscanf(line, "%[^:]%*c %[^:]%*c%lf", key, op, &value) == 3) {
      // Value line with operator specification
    } else if (sscanf(line, "%[^:]%*c%lf", key, &value) == 2) {
      // Value line specification, default to "avg" operator
      strcpy(op, "avg");
    } else if (sscanf(line, "%[^:]: %250[^\n]", key, value_str) == 2) {
      // Nodewatcher metadata line -- output unchanged line to state file
      metadata = true;
    } else {
      continue;
    }

    // Support shortened output format for names and values
    char *endptr = NULL;
    key_short = strtol(key, &endptr, 10);
    if (*endptr == 0) {
      char fmt_key[256] = {0,};
      if (metadata)
        snprintf(fmt_key, sizeof(fmt_key), cfg->of_name, key);
      else
        snprintf(fmt_key, sizeof(fmt_key), cfg->of_value, key);

      strncpy(key, fmt_key, sizeof(key));
    } else {
      key_short = -1;
    }

    if (metadata) {
      fprintf(state, "%s: %s\n", key, value_str);
      continue;
    }

    // Value line -- store into the log hash table
    struct log_item_t *item;
    HASH_FIND_STR(*log_table, key, item);
    if (!item) {
      // Create new item and store it
      item = (struct log_item_t*) malloc(sizeof(struct log_item_t));
      item->key = strdup(key);
      item->key_short = key_short;
      item->count = 0;
      item->sum = 0.0;
      item->min = value;
      item->max = value;

      HASH_ADD_KEYPTR(hh, *log_table, item->key, strlen(item->key), item);
    }

    item->last = value;
    item->count++;
    item->sum += value;
    if (value < item->min)
      item->min = value;
    if (value > item->max)
      item->max = value;

    // Calculate value based on selected operator
    double derived;
    if (strcmp(op, "min") == 0)
      derived = item->min;
    else if (strcmp(op, "max") == 0)
      derived = item->max;
    else if (strcmp(op, "sum") == 0)
      derived = item->sum;
    else if (strcmp(op, "avg") == 0)
      derived = item->sum / item->count;
    else
      derived = item->sum / item->count;

    fprintf(state, "%s: %f\n", item->key, derived);
    if (last_state != NULL) {
      fprintf(last_state, " %f", item->last);
    }
  }

  free(rsp);

  // Output current state and log last values
  struct log_item_t *item;

  gzprintf(log, "%f", collector_get_time());
  for (item = *log_table; item != NULL; item = item->hh.next) {
    if (item->key_short >= 0)
      gzprintf(log, "\t%d\t%f", item->key_short, item->last);
    else
      gzprintf(log, "\t%s\t%f", item->key, item->last);
  }
  gzprintf(log, "\n");

  fflush(state);
  if (last_state != NULL) {
    fprintf(last_state, "\n");
    fflush(last_state);
  }
  gzflush(log, Z_SYNC_FLUSH);
}

/**
 * Starts the collector.
 *
 * @param config Root configuration object
 * @param log_option Syslog flags
 * @return True on success, false when some error has ocurred
 */
bool start_collector(ucl_object_t *config, int log_option)
{
  ucl_object_t *cfg_server = ucl_object_find_key(config, "server");
  if (!cfg_server) {
    fprintf(stderr, "ERROR: Missing server configuration!\n");
    return false;
  }

  ucl_object_t *cfg_collector = ucl_object_find_key(config, "collector");
  if (!cfg_collector) {
    fprintf(stderr, "ERROR: Missing collector configuration!\n");
    return false;
  }

  ucl_object_t *cfg_client = ucl_object_find_key(config, "client");
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

  utimer_t timer_poll = timer_now();
  double poll_interval_sec;
  utimer_t poll_interval_msec;

  ucl_object_t *interval = ucl_object_find_key(cfg_collector, "poll_interval");
  if (!interval) {
    fprintf(stderr, "ERROR: Missing 'poll_interval' in configuration file!\n");
    return false;
  } else if (!ucl_object_todouble_safe(interval, &poll_interval_sec)) {
    fprintf(stderr, "ERROR: Poll interval must be an integer or double!\n");
    return false;
  }

  const char *log_filename;
  const char *state_filename;
  const char *last_state_filename = NULL;

  obj = ucl_object_find_key(cfg_collector, "log_file");
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'log_file' in configuration file!\n");
    return false;
  } else if (!ucl_object_tostring_safe(obj, &log_filename)) {
    fprintf(stderr, "ERROR: Log file path must be a string!\n");
    return false;
  }

  obj = ucl_object_find_key(cfg_collector, "state_file");
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'state_file' in configuration file!\n");
    return false;
  } else if (!ucl_object_tostring_safe(obj, &state_filename)) {
    fprintf(stderr, "ERROR: State file path must be a string!\n");
    return false;
  }

  obj = ucl_object_find_key(cfg_collector, "last_state_file");
  if (!obj) {
    last_state_filename = NULL;
  } else if (!ucl_object_tostring_safe(obj, &last_state_filename)) {
    fprintf(stderr, "ERROR: Last state file path must be a string!\n");
    return false;
  }

  struct collector_cfg_t cfg;

  obj = ucl_object_find_key(cfg_collector, "output_formatter");
  if (!obj) {
    fprintf(stderr, "ERROR: Missing 'output_formatter' section in configuration file!\n");
    return false;
  } else {
    ucl_object_t *of_obj = ucl_object_find_key(obj, "name");
    if (!of_obj) {
      fprintf(stderr, "ERROR: Missing 'output_formatter.name' in configuration file!\n");
      return false;
    } else if (!ucl_object_tostring_safe(of_obj, &cfg.of_name)) {
      fprintf(stderr, "ERROR: Name format must be a string!\n");
      return false;
    }

    of_obj = ucl_object_find_key(obj, "value");
    if (!of_obj) {
      fprintf(stderr, "ERROR: Missing 'key_formatter.value' in configuration file!\n");
      return false;
    } else if (!ucl_object_tostring_safe(of_obj, &cfg.of_value)) {
      fprintf(stderr, "ERROR: Value format must be a string!\n");
      return false;
    }
  }

  FILE *log_file = fopen(log_filename, "w");
  if (!log_file) {
    fprintf(stderr, "ERROR: Unable to open log file.\n");
    return false;
  }
  FILE *state_file = fopen(state_filename, "w");
  if (!state_file) {
    fprintf(stderr, "ERROR: Unable to open state file.\n");
    return false;
  }
  FILE *last_state_file = NULL;
  if (last_state_filename) {
    last_state_file = fopen(last_state_filename, "w");
    if (!last_state_file) {
      fprintf(stderr, "ERROR: Unable to open state file.\n");
      return false;
    }
  }

  gzFile log_file_gz = gzdopen(fileno(log_file), "a");
  poll_interval_msec = (long) (poll_interval_sec * 1000);

  int client_fd = client_connect(cfg_server);

  // Open the syslog facility
  openlog("koruza-collector", 0, LOG_DAEMON);
  syslog(LOG_INFO, "KORUZA collector daemon starting up.");

  struct log_item_t *log_table = NULL;
  size_t state_file_size = 0;
  size_t log_file_size = 0;
  struct timespec tsp = {0, 10000000};
  size_t cmd_failures = 0;

  for (;;) {
    nanosleep(&tsp, NULL);

    // Periodically request data
    if (is_timeout(&timer_poll, poll_interval_msec)) {
      char *response;
      DEBUG_LOG("Requesting data from server.\n");
      if (!client_send_device_command(client_fd, status_command, &response)) {
        syslog(LOG_WARNING, "Failed to receive data from control daeamon!");

        if (++cmd_failures > 5) {
          syslog(LOG_ERR, "Multiple failures while requesting data, reconnecting...");
          close(client_fd);
          client_fd = client_connect(cfg_server);
          cmd_failures = 0;
        }
        continue;
      }

      // Check for state file truncation -- in this case reset all state
      struct stat stats;
      stats.st_size = 0;
      if (fstat(fileno(state_file), &stats) != 0 ||
          (state_file_size > 0 && stats.st_size < state_file_size)) {
        struct log_item_t *item, *tmp;
        HASH_ITER(hh, log_table, item, tmp) {
          HASH_DEL(log_table, item);
          free(item->key);
          free(item);
        }

        DEBUG_LOG("Reopening state file.");

        // Reopen state file
        fclose(state_file);
        state_file = fopen(state_filename, "w");
        if (!state_file) {
          fprintf(stderr, "ERROR: Unable to reopen state file.\n");
          return false;
        }
      }

      state_file_size = stats.st_size;

      // Check for log file truncation
      stats.st_size = 0;
      if (fstat(fileno(log_file), &stats) != 0 ||
          (log_file_size > 0 && stats.st_size < log_file_size)) {
        DEBUG_LOG("Reopening log file.");

        // Reopen log file
        gzclose(log_file_gz);
        fclose(log_file);
        log_file = fopen(log_filename, "w");
        if (!log_file) {
          fprintf(stderr, "ERROR: Unable to reopen log file.\n");
          return false;
        }
        log_file_gz = gzdopen(fileno(log_file), "a");
      }

      log_file_size = stats.st_size;

      collector_parse_response(&cfg, &log_table, response, log_file_gz, state_file, last_state_file);
      free(response);
    }
  }
}