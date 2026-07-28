/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2026 Joseph Kogut <joseph.kogut@gmail.com>
 * crun is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * crun is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef KRUN_VIRTIOFS_CONFIG_H
#define KRUN_VIRTIOFS_CONFIG_H

#include "../error.h"
#include <json-c/json.h>
#include <stdint.h>

typedef int32_t (*krun_add_virtiofs2_fn) (uint32_t ctx_id, const char *tag,
                                          const char *path, uint64_t shm_size);

int krun_configure_virtiofs (uint32_t ctx_id, json_object *config_tree,
                             const char *annotation,
                             krun_add_virtiofs2_fn add_virtiofs,
                             libcrun_error_t *err);

#endif
