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
#include "util.h"

#include <stdio.h>
#include <time.h>

long timer_now()
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
    fprintf(stderr, "ERROR: Failed to get monotonic clock, weird things may happen!");
    return -1;
  }
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int is_timeout(long *timer, long period)
{
  if (*timer < 0)
    return 0;

  long now = timer_now();
  if (now - *timer > period) {
    *timer = now;
    return 1;
  }

  return 0;
}
