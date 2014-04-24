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
#ifndef KORUZA_CONTROLLER_GLOBAL_H
#define KORUZA_CONTROLLER_GLOBAL_H

// Uncomment this to enable verbose debug output
#define DEBUG

#ifdef DEBUG
#define DEBUG_LOG(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...)
#endif

#endif
