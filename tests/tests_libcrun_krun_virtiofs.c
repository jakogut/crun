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
#include <libcrun/handlers/krun-virtiofs-config.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*test) ();

struct recorded_device
{
  uint32_t ctx_id;
  char tag[64];
  char path[128];
  uint64_t shm_size;
};

struct expected_device
{
  const char *tag;
  const char *path;
  uint64_t shm_size;
};

static struct recorded_device recorded[8];
static size_t recorded_len;
static size_t fail_at;
static int fail_with;

static void
reset_recorder ()
{
  memset (recorded, 0, sizeof (recorded));
  recorded_len = 0;
  fail_at = (size_t) -1;
  fail_with = 0;
}

static int32_t
record_virtiofs (uint32_t ctx_id, const char *tag, const char *path,
                 uint64_t shm_size)
{
  if (recorded_len == fail_at)
    return -fail_with;
  if (recorded_len >= sizeof (recorded) / sizeof (recorded[0]))
    return -E2BIG;

  recorded[recorded_len].ctx_id = ctx_id;
  snprintf (recorded[recorded_len].tag,
            sizeof (recorded[recorded_len].tag), "%s", tag);
  snprintf (recorded[recorded_len].path,
            sizeof (recorded[recorded_len].path), "%s", path);
  recorded[recorded_len].shm_size = shm_size;
  recorded_len++;
  return 0;
}

static json_object *
parse_json (const char *value)
{
  return json_tokener_parse (value);
}

static int
expect_devices (uint32_t ctx_id, json_object *config, const char *annotation,
                const struct expected_device *expected, size_t expected_len)
{
  libcrun_error_t err = NULL;
  size_t i;
  int ret;

  reset_recorder ();
  ret = krun_configure_virtiofs (ctx_id, config, annotation,
                                 record_virtiofs, &err);
  if (ret < 0)
    {
      fprintf (stderr, "unexpected error: %s\n",
               err == NULL ? "no error" : err->msg);
      crun_error_release (&err);
      return -1;
    }

  if (recorded_len != expected_len)
    {
      fprintf (stderr, "recorded %zu devices, expected %zu\n",
               recorded_len, expected_len);
      crun_error_release (&err);
      return -1;
    }

  for (i = 0; i < expected_len; i++)
    if (recorded[i].ctx_id != ctx_id
        || strcmp (recorded[i].tag, expected[i].tag) != 0
        || strcmp (recorded[i].path, expected[i].path) != 0
        || recorded[i].shm_size != expected[i].shm_size)
      {
        fprintf (stderr,
                 "device %zu: got ctx=%" PRIu32 ", tag=`%s`, path=`%s`, "
                 "shm_size=%" PRIu64 "; expected ctx=%" PRIu32 ", tag=`%s`, "
                 "path=`%s`, shm_size=%" PRIu64 "\n",
                 i, recorded[i].ctx_id, recorded[i].tag, recorded[i].path,
                 recorded[i].shm_size, ctx_id, expected[i].tag,
                 expected[i].path, expected[i].shm_size);
        crun_error_release (&err);
        return -1;
      }

  crun_error_release (&err);
  return 0;
}

static int
expect_error (json_object *config, const char *annotation, const char *message)
{
  libcrun_error_t err = NULL;
  int ret;

  reset_recorder ();
  ret = krun_configure_virtiofs (7, config, annotation,
                                 record_virtiofs, &err);
  if (ret >= 0 || err == NULL || strstr (err->msg, message) == NULL
      || recorded_len != 0)
    {
      fprintf (stderr, "expected error containing `%s`, got `%s`\n",
               message, err == NULL ? "no error" : err->msg);
      crun_error_release (&err);
      return -1;
    }
  crun_error_release (&err);
  return 0;
}

static int
test_configuration_sources ()
{
  static const struct expected_device expected[] = {
    { "rootfs", "/", 1048576 },
    { "cache", "/var/cache", 2097152 },
    { "devshm", "/dev/shm", 536870912 },
  };
  json_object *config;
  int ret = -1;

  config = parse_json (
      "{\"virtiofs_tag\":\"rootfs\",\"virtiofs_shm_size\":1048576,"
      "\"virtiofs\":[{\"tag\":\"cache\",\"path\":\"/var/cache\","
      "\"shm_size\":2097152}]}");
  if (config == NULL)
    return -1;

  if (expect_devices (
          42, config,
          "[{\"tag\":\"devshm\",\"path\":\"/dev/shm\","
          "\"shm_size\":536870912}]",
          expected, sizeof (expected) / sizeof (expected[0]))
      < 0)
    goto out;
  ret = 0;

out:
  json_object_put (config);
  return ret;
}

static int
test_independent_sources ()
{
  static const struct expected_device config_expected[] = {
    { "/dev/root", "/", 536870912 },
    { "data", "/srv/data", 4096 },
  };
  static const struct expected_device annotation_expected[] = {
    { "/dev/root", "/", 536870912 },
    { "runtime", "/run/user/1000", 8192 },
  };
  json_object *config;
  int ret = -1;

  config = parse_json (
      "{\"virtiofs\":[{\"tag\":\"data\",\"path\":\"/srv/data\","
      "\"shm_size\":4096}]}");
  if (config == NULL)
    return -1;

  if (expect_devices (
          1, config, NULL, config_expected,
          sizeof (config_expected) / sizeof (config_expected[0]))
      < 0)
    goto out;

  if (expect_devices (
          2, NULL,
          "[{\"tag\":\"runtime\",\"path\":\"/run/user/1000\","
          "\"shm_size\":8192}]",
          annotation_expected,
          sizeof (annotation_expected) / sizeof (annotation_expected[0]))
      < 0)
    goto out;
  ret = 0;

out:
  json_object_put (config);
  return ret;
}

static int
test_zero_root_shm_size ()
{
  static const struct expected_device expected[] = {
    { "/dev/root", "/", 0 },
  };
  json_object *config;
  int ret = -1;

  config = parse_json ("{\"virtiofs_shm_size\":0}");
  if (config == NULL)
    return -1;

  if (expect_devices (1, config, NULL, expected,
                      sizeof (expected) / sizeof (expected[0]))
      < 0)
    goto out;
  ret = 0;

out:
  json_object_put (config);
  return ret;
}

static int
test_legacy_root_types ()
{
  static const struct expected_device expected[] = {
    { "/dev/root", "/", 536870912 },
  };
  json_object *config;
  int ret;

  config = parse_json (
      "{\"virtiofs_tag\":42,\"virtiofs_shm_size\":\"0\"}");
  if (config == NULL)
    return -1;

  ret = expect_devices (1, config, NULL, expected,
                        sizeof (expected) / sizeof (expected[0]));
  json_object_put (config);
  return ret;
}

static int
test_validation ()
{
  static const struct
  {
    const char *json;
    const char *message;
  } cases[] = {
    { "{\"virtiofs_shm_size\":-1}", "virtiofs_shm_size must not be negative" },
    { "{\"virtiofs\":{}}", "must be an array" },
    { "{\"virtiofs\":[null]}", "must be an object" },
    { "{\"virtiofs\":[{\"path\":\"/x\",\"shm_size\":1}]}", ".tag must be a string" },
    { "{\"virtiofs\":[{\"tag\":\"\",\"path\":\"/x\",\"shm_size\":1}]}", ".tag must not be empty" },
    { "{\"virtiofs\":[{\"tag\":\"abcdefghijklmnopqrstuvwxyzabcdefghijk\","
      "\"path\":\"/x\",\"shm_size\":1}]}",
      "exceeds 36 bytes" },
    { "{\"virtiofs\":[{\"tag\":\"x\",\"path\":\"relative\",\"shm_size\":1}]}", "is not absolute" },
    { "{\"virtiofs\":[{\"tag\":\"x\",\"path\":\"/x\"}]}", ".shm_size must be an integer" },
    { "{\"virtiofs\":[{\"tag\":\"x\",\"path\":\"/x\",\"shm_size\":\"1\"}]}", ".shm_size must be an integer" },
    { "{\"virtiofs\":[{\"tag\":\"x\",\"path\":\"/x\",\"shm_size\":0}]}", ".shm_size must be positive" },
    { "{\"virtiofs\":[{\"tag\":\"x\",\"path\":\"/x\",\"shm_size\":-1}]}", ".shm_size must be positive" },
  };
  json_object *config;
  size_t i;

  for (i = 0; i < sizeof (cases) / sizeof (cases[0]); i++)
    {
      config = parse_json (cases[i].json);
      if (config == NULL)
        return -1;
      if (expect_error (config, NULL, cases[i].message) < 0)
        {
          json_object_put (config);
          return -1;
        }
      json_object_put (config);
    }

  if (expect_error (NULL, "not-json", "not valid JSON") < 0
      || expect_error (NULL, "{}", "must be an array") < 0
      || expect_error (NULL, "[] trailing", "not valid JSON") < 0)
    return -1;
  return 0;
}

static int
test_duplicate_tags ()
{
  json_object *config;
  int ret = -1;

  config = parse_json (
      "{\"virtiofs\":[{\"tag\":\"same\",\"path\":\"/one\",\"shm_size\":1},"
      "{\"tag\":\"same\",\"path\":\"/two\",\"shm_size\":2}]}");
  if (config == NULL)
    return -1;
  if (expect_error (config, NULL, "duplicate virtiofs tag `same`") < 0)
    goto out;
  json_object_put (config);

  config = parse_json (
      "{\"virtiofs\":[{\"tag\":\"same\",\"path\":\"/one\",\"shm_size\":1}]}");
  if (config == NULL)
    return -1;
  if (expect_error (
          config,
          "[{\"tag\":\"same\",\"path\":\"/two\",\"shm_size\":2}]",
          "duplicate virtiofs tag `same`")
      < 0)
    goto out;
  json_object_put (config);

  config = parse_json (
      "{\"virtiofs\":[{\"tag\":\"/dev/root\",\"path\":\"/one\","
      "\"shm_size\":1}]}");
  if (config == NULL)
    return -1;
  if (expect_error (config, NULL, "duplicate virtiofs tag `/dev/root`") < 0)
    goto out;
  ret = 0;

out:
  json_object_put (config);
  return ret;
}

static int
test_libkrun_errors ()
{
  json_object *config;
  libcrun_error_t err = NULL;
  int ret = -1;

  config = parse_json (
      "{\"virtiofs\":[{\"tag\":\"data\",\"path\":\"/data\","
      "\"shm_size\":4096}]}");
  if (config == NULL)
    return -1;

  reset_recorder ();
  fail_at = 0;
  fail_with = EIO;
  if (krun_configure_virtiofs (3, config, NULL,
                               record_virtiofs, &err)
          >= 0
      || crun_error_get_errno (&err) != EIO
      || strstr (err->msg, "virtiofs root") == NULL)
    goto out;
  crun_error_release (&err);

  reset_recorder ();
  fail_at = 1;
  fail_with = EACCES;
  if (krun_configure_virtiofs (3, config, NULL,
                               record_virtiofs, &err)
          >= 0
      || crun_error_get_errno (&err) != EACCES
      || strstr (err->msg, "device with tag `data`") == NULL
      || recorded_len != 1)
    goto out;
  crun_error_release (&err);

  if (krun_configure_virtiofs (3, config, NULL, NULL, &err) >= 0
      || crun_error_get_errno (&err) != ENOSYS
      || strstr (err->msg, "krun_add_virtiofs2") == NULL)
    goto out;
  ret = 0;

out:
  if (ret < 0 && err != NULL)
    fprintf (stderr, "unexpected error: %s\n", err->msg);
  crun_error_release (&err);
  json_object_put (config);
  return ret;
}

static int
run_and_print_test_result (const char *name, int id, test t)
{
  int ret = t ();

  if (ret == 0)
    printf ("ok %d - %s\n", id, name);
  else if (ret == 77)
    printf ("ok %d - %s #SKIP\n", id, name);
  else
    printf ("not ok %d - %s\n", id, name);
  return ret < 0;
}

#define RUN_TEST(T)                                      \
  do                                                     \
    {                                                    \
      failed += run_and_print_test_result (#T, id++, T); \
  } while (0)

int
main ()
{
  int failed = 0;
  int id = 1;

  printf ("1..7\n");
  RUN_TEST (test_configuration_sources);
  RUN_TEST (test_independent_sources);
  RUN_TEST (test_zero_root_shm_size);
  RUN_TEST (test_legacy_root_types);
  RUN_TEST (test_validation);
  RUN_TEST (test_duplicate_tags);
  RUN_TEST (test_libkrun_errors);
  return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
