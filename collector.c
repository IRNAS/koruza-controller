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
#include <zlib.h>

struct log_item_t {
  /// Unique item key
  char *key;
  /// Last stored value
  double last;
  /// Number of stored values
  size_t count;
  /// Sum of stored values
  double sum;

  UT_hash_handle hh;
};

void collector_parse_response(struct log_item_t **log_table,
                              const char *response,
                              gzFile log,
                              FILE *state)
{
  char *rsp = strdup(response);
  char *rsp_tok = rsp;

  // Each line in the form of <key>: <double> is a valid response
  for (;; rsp_tok = NULL) {
    char *line = strtok(rsp_tok, "\n");
    if (!line)
      break;

    char key[256];
    double value;

    if (sscanf(line, "%[^:]%*c%lf", key, &value) != 2)
      continue;

    // Store into the log hash table
    struct log_item_t *item;
    HASH_FIND_STR(*log_table, key, item);
    if (!item) {
      // Create new item and store it
      item = (struct log_item_t*) malloc(sizeof(struct log_item_t));
      item->key = strdup(key);
      item->count = 0;
      item->sum = 0.0;

      HASH_ADD_KEYPTR(hh, *log_table, item->key, strlen(item->key), item);
    }

    item->last = value;
    item->count++;
    item->sum += value;
  }

  free(rsp);

  // Output current state and log last values
  struct log_item_t *item;

  ftruncate(fileno(state), 0);
  rewind(state);
  for (item = *log_table; item != NULL; item = item->hh.next) {
    fprintf(state, "%s: %f\n", item->key, item->sum / item->count);
    gzprintf(log, "%d\t%s\t%f\n", time(NULL), item->key, item->last);
  }
  fflush(state);
  gzflush(log, Z_SYNC_FLUSH);
}

/**
 * Starts the collector.
 *
 * @param config Root configuration object
 * @return True on success, false when some error has ocurred
 */
bool start_collector(ucl_object_t *config)
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

  long timer_poll = timer_now();
  double poll_interval_sec;
  long poll_interval_msec;

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

  ucl_object_t *obj = ucl_object_find_key(cfg_collector, "log_file");
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

  gzFile log_file_gz = gzdopen(fileno(log_file), "a");
  poll_interval_msec = (long) (poll_interval_sec * 1000);

  int client_fd = client_connect(cfg_server);
  if (client_fd < 0)
    return false;

  struct log_item_t *log_table = NULL;
  size_t state_file_size = 0;
  size_t log_file_size = 0;
  struct timespec tsp = {0, 10000000};

  for (;;) {
    nanosleep(&tsp, NULL);

    // Periodically request data
    if (is_timeout(&timer_poll, poll_interval_msec)) {
      char *response;
      if (!client_send_device_command(client_fd, "A 4\n", &response))
        continue;

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
        log_file = fopen(log_filename, "w");
        if (!log_file) {
          fprintf(stderr, "ERROR: Unable to reopen log file.\n");
          return false;
        }
        log_file_gz = gzdopen(fileno(log_file), "a");
      }

      log_file_size = stats.st_size;

      collector_parse_response(&log_table, response, log_file_gz, state_file);
      free(response);
    }
  }
}