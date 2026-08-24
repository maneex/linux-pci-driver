// C headers.
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Velocitor headers.
#include "private.h"
#include "velocitor/velocitor.h"

static const char *sysfs_path = "/sys/class/velocitor";

static int sysfs_read_u64(const char *entry, const char *attr, uint64_t *out) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s/%s", sysfs_path, entry, attr);

  FILE *f = fopen(path, "re"); /* 'e' = O_CLOEXEC: a library must not leak */
  if (NULL == f)
    return -errno;

  int ok = fscanf(f, "%" SCNi64, (int64_t *)out) == 1;
  fclose(f);
  return ok ? 0 : -EINVAL;
}

static int sysfs_read_uevent(const char *entry, char *devname, size_t size,
                             unsigned int *minor) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s/uevent", sysfs_path, entry);

  FILE *f = fopen(path, "re");
  if (NULL == f)
    return -errno;

  char line[256];
  bool has_name = false, has_minor = false;

  while (NULL != fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';

    if (0 == strncmp(line, "DEVNAME=", 8)) {
      int n = snprintf(devname, size, "%s", line + 8);
      if (n < 0 || (size_t)n >= size) {
        fclose(f);
        return -ENAMETOOLONG;
      }
      has_name = true;
    } else if (0 == strncmp(line, "MINOR=", 6)) {
      *minor = (unsigned int)strtoul(line + 6, NULL, 10);
      has_minor = true;
    }
  }

  fclose(f);
  return (has_name && has_minor) ? 0 : -ENOENT;
}

int velocitor_sysfs_read_device_attrs(const char *dname,
                                      struct velocitor_device_info *device) {
  device->present = false;

  int err = 0;
  if ((err = sysfs_read_uevent(dname, device->devname, sizeof(device->devname),
                               &device->minor)))
    return err;

  uint64_t temp = 0;
  if ((err = sysfs_read_u64(dname, "version", &temp)))
    return err;

  device->firmware_version.major = (unsigned int)(temp >> 16);
  device->firmware_version.minor = (unsigned int)(temp & 0xffff);

  if ((err = sysfs_read_u64(dname, "mem_size", &temp)))
    return err;

  device->topology.memory = (size_t)temp;

  if ((err = sysfs_read_u64(dname, "nodes", &temp)))
    return err;

  device->topology.nodes = (unsigned int)temp;

  if ((err = sysfs_read_u64(dname, "engines", &temp)))
    return err;

  device->topology.engines = (unsigned int)temp;
  device->present = true;
  return 0;
}

int velocitor_scan(struct velocitor_device_info *devices, unsigned int count) {
  if (NULL == devices)
    return -EINVAL;

  if (count > VEL_MAX_DEVICES)
    count = VEL_MAX_DEVICES;

  DIR *sys = opendir(sysfs_path);
  if (NULL == sys)
    return -errno;

  unsigned int idx = 0;
  for (struct dirent *entry = readdir(sys); (NULL != entry) && (idx < count);
       entry = readdir(sys)) {
    if ('.' == entry->d_name[0])
      continue;

    if (0 == velocitor_sysfs_read_device_attrs(entry->d_name, devices + idx))
      ++idx;
  }

  closedir(sys);
  return (int)idx;
}
