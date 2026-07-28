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
#include <config.h>
#include "../utils.h"
#include "krun-virtiofs-config.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define KRUN_VIRTIOFS_DEFAULT_TAG "/dev/root"
#define KRUN_VIRTIOFS_DEFAULT_SHM_SIZE (512 * 1024 * 1024ULL)
#define KRUN_VIRTIOFS_MAX_TAG_LENGTH 36

struct krun_virtiofs_device
{
  const char *tag;
  const char *path;
  uint64_t shm_size;
};

struct krun_virtiofs_devices
{
  struct krun_virtiofs_device *entries;
  size_t len;
};

static void
free_devices (struct krun_virtiofs_devices *devices)
{
  free (devices->entries);
  devices->entries = NULL;
  devices->len = 0;
}

static int
append_entry (struct krun_virtiofs_devices *devices, const char *tag,
              const char *path, uint64_t shm_size, libcrun_error_t *err)
{
  struct krun_virtiofs_device *entries;
  size_t i;

  for (i = 0; i < devices->len; i++)
    if (strcmp (devices->entries[i].tag, tag) == 0)
      return crun_make_error (err, EINVAL, "duplicate virtiofs tag `%s`", tag);

  entries = xrealloc (devices->entries,
                      sizeof (*devices->entries) * (devices->len + 1));
  devices->entries = entries;
  devices->entries[devices->len].tag = tag;
  devices->entries[devices->len].path = path;
  devices->entries[devices->len].shm_size = shm_size;
  devices->len++;
  return 0;
}

static int
validate_string (json_object *value, const char *field, const char *source,
                 size_t index, const char **result, libcrun_error_t *err)
{
  const char *string;
  size_t length;

  if (value == NULL || ! json_object_is_type (value, json_type_string))
    return crun_make_error (err, EINVAL, "%s[%zu].%s must be a string",
                            source, index, field);

  string = json_object_get_string (value);
  length = (size_t) json_object_get_string_len (value);
  if (length == 0 || strlen (string) != length)
    return crun_make_error (err, EINVAL, "%s[%zu].%s must not be empty or contain NUL bytes",
                            source, index, field);

  *result = string;
  return 0;
}

static int
append_device (struct krun_virtiofs_devices *devices, json_object *value,
               const char *source, size_t index, libcrun_error_t *err)
{
  json_object *tag_value;
  json_object *path_value;
  json_object *shm_size_value;
  const char *tag;
  const char *path;
  int64_t shm_size;
  int ret;

  if (value == NULL || ! json_object_is_type (value, json_type_object))
    return crun_make_error (err, EINVAL, "%s[%zu] must be an object", source, index);

  tag_value = json_object_object_get (value, "tag");
  ret = validate_string (tag_value, "tag", source, index, &tag, err);
  if (UNLIKELY (ret < 0))
    return ret;
  if (strlen (tag) > KRUN_VIRTIOFS_MAX_TAG_LENGTH)
    return crun_make_error (err, EINVAL, "%s[%zu].tag exceeds %d bytes",
                            source, index, KRUN_VIRTIOFS_MAX_TAG_LENGTH);

  path_value = json_object_object_get (value, "path");
  ret = validate_string (path_value, "path", source, index, &path, err);
  if (UNLIKELY (ret < 0))
    return ret;
  if (path[0] != '/')
    return crun_make_error (err, EINVAL, "%s[%zu].path `%s` is not absolute",
                            source, index, path);

  shm_size_value = json_object_object_get (value, "shm_size");
  if (shm_size_value == NULL || ! json_object_is_type (shm_size_value, json_type_int))
    return crun_make_error (err, EINVAL, "%s[%zu].shm_size must be an integer",
                            source, index);
  shm_size = json_object_get_int64 (shm_size_value);
  if (shm_size <= 0)
    return crun_make_error (err, EINVAL, "%s[%zu].shm_size must be positive",
                            source, index);

  return append_entry (devices, tag, path, (uint64_t) shm_size, err);
}

static int
append_devices (struct krun_virtiofs_devices *devices, json_object *array,
                const char *source, libcrun_error_t *err)
{
  size_t i;

  if (array == NULL)
    return 0;
  if (! json_object_is_type (array, json_type_array))
    return crun_make_error (err, EINVAL, "%s must be an array", source);

  for (i = 0; i < json_object_array_length (array); i++)
    {
      int ret = append_device (
          devices, json_object_array_get_idx (array, i), source, i, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  return 0;
}

static int
parse_annotation (const char *annotation, json_object **doc,
                  libcrun_error_t *err)
{
  struct json_tokener *tokener;
  enum json_tokener_error json_error;
  size_t length;
  size_t parsed;

  *doc = NULL;
  if (annotation == NULL)
    return 0;

  tokener = json_tokener_new ();
  if (tokener == NULL)
    return crun_make_error (err, ENOMEM, "cannot allocate JSON tokener");
  json_tokener_set_flags (tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);

  length = strlen (annotation);
  *doc = json_tokener_parse_ex (tokener, annotation, (int) length);
  json_error = json_tokener_get_error (tokener);
  parsed = json_tokener_get_parse_end (tokener);
  json_tokener_free (tokener);

  while (parsed < length
         && (annotation[parsed] == ' ' || annotation[parsed] == '\t'
             || annotation[parsed] == '\n' || annotation[parsed] == '\r'))
    parsed++;

  if (*doc == NULL || json_error != json_tokener_success || parsed != length)
    {
      if (*doc != NULL)
        json_object_put (*doc);
      *doc = NULL;
      return crun_make_error (err, EINVAL, "krun.virtiofs is not valid JSON");
    }
  return 0;
}

static int
parse_root_device (json_object *config_tree,
                   struct krun_virtiofs_device *root,
                   json_object **config_devices, libcrun_error_t *err)
{
  json_object *root_shm_size_value = NULL;
  json_object *root_tag_value = NULL;
  int64_t configured_root_shm_size;

  root->tag = KRUN_VIRTIOFS_DEFAULT_TAG;
  root->path = "/";
  root->shm_size = KRUN_VIRTIOFS_DEFAULT_SHM_SIZE;
  *config_devices = NULL;

  if (config_tree == NULL)
    return 0;

  root_tag_value = json_object_object_get (config_tree, "virtiofs_tag");
  if (root_tag_value != NULL
      && json_object_is_type (root_tag_value, json_type_string))
    root->tag = json_object_get_string (root_tag_value);

  root_shm_size_value = json_object_object_get (config_tree, "virtiofs_shm_size");
  if (root_shm_size_value != NULL
      && json_object_is_type (root_shm_size_value, json_type_int))
    {
      configured_root_shm_size = json_object_get_int64 (root_shm_size_value);
      if (configured_root_shm_size < 0)
        return crun_make_error (err, EINVAL, "virtiofs_shm_size must not be negative");
      root->shm_size = (uint64_t) configured_root_shm_size;
    }

  *config_devices = json_object_object_get (config_tree, "virtiofs");
  if (root->tag[0] == '\0'
      || strlen (root->tag) > KRUN_VIRTIOFS_MAX_TAG_LENGTH)
    return crun_make_error (err, EINVAL, "virtiofs root tag must contain between 1 and %d bytes",
                            KRUN_VIRTIOFS_MAX_TAG_LENGTH);
  return 0;
}

int
krun_configure_virtiofs (uint32_t ctx_id, json_object *config_tree,
                         const char *annotation,
                         krun_add_virtiofs2_fn add_virtiofs,
                         libcrun_error_t *err)
{
  struct krun_virtiofs_device root;
  struct krun_virtiofs_devices devices = { 0 };
  json_object *annotation_doc = NULL;
  json_object *config_devices = NULL;
  size_t i;
  int ret;

  if (add_virtiofs == NULL)
    return crun_make_error (err, ENOSYS, "could not find symbol `krun_add_virtiofs2` in `libkrun.so`");

  ret = parse_root_device (config_tree, &root, &config_devices, err);
  if (UNLIKELY (ret < 0))
    goto out;

  ret = append_entry (&devices, root.tag, root.path, root.shm_size, err);
  if (UNLIKELY (ret < 0))
    goto out;

  ret = append_devices (&devices, config_devices,
                        ".krun_vm.json virtiofs", err);
  if (UNLIKELY (ret < 0))
    goto out;

  ret = parse_annotation (annotation, &annotation_doc, err);
  if (UNLIKELY (ret < 0))
    goto out;
  ret = append_devices (&devices, annotation_doc, "krun.virtiofs", err);
  if (UNLIKELY (ret < 0))
    goto out;

  for (i = 0; i < devices.len; i++)
    {
      ret = add_virtiofs (ctx_id, devices.entries[i].tag,
                          devices.entries[i].path,
                          devices.entries[i].shm_size);
      if (UNLIKELY (ret < 0))
        {
          if (i == 0)
            ret = crun_make_error (err, -ret,
                                   "could not add virtiofs root with tag `%s`",
                                   devices.entries[i].tag);
          else
            ret = crun_make_error (err, -ret,
                                   "could not add virtiofs device with tag `%s`",
                                   devices.entries[i].tag);
          goto out;
        }
    }
  ret = 0;

out:
  if (annotation_doc != NULL)
    json_object_put (annotation_doc);
  free_devices (&devices);
  return ret;
}
