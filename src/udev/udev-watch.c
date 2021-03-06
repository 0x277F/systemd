/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright © 2009 Canonical Ltd.
 * Copyright © 2009 Scott James Remnant <scott@netsplit.com>
 */

#include <sys/inotify.h>
#include <unistd.h>

#include "device-private.h"
#include "dirent-util.h"
#include "mkdir.h"
#include "stdio-util.h"
#include "udev-watch.h"

static int inotify_fd = -1;

/* inotify descriptor, will be shared with rules directory;
 * set to cloexec since we need our children to be able to add
 * watches for us. */
int udev_watch_init(void) {
        inotify_fd = inotify_init1(IN_CLOEXEC);
        if (inotify_fd < 0)
                return log_error_errno(errno, "Failed to create inotify descriptor: %m");

        return inotify_fd;
}

/* Move any old watches directory out of the way, and then restore the watches. */
int udev_watch_restore(void) {
        struct dirent *ent;
        DIR *dir;
        int r;

        if (inotify_fd < 0)
                return log_error_errno(EINVAL, "Invalid inotify descriptor.");

        if (rename("/run/udev/watch", "/run/udev/watch.old") < 0) {
                if (errno != ENOENT)
                        return log_error_errno(errno, "Failed to move watches directory /run/udev/watch. Old watches will not be restored: %m");

                return 0;
        }

        dir = opendir("/run/udev/watch.old");
        if (!dir)
                return log_error_errno(errno, "Failed to open old watches directory /run/udev/watch.old. Old watches will not be restored: %m");

        FOREACH_DIRENT_ALL(ent, dir, break) {
                _cleanup_(sd_device_unrefp) sd_device *dev = NULL;
                char device[PATH_MAX];
                ssize_t len;

                if (ent->d_name[0] == '.')
                        continue;

                len = readlinkat(dirfd(dir), ent->d_name, device, sizeof(device));
                if (len <= 0) {
                        log_error_errno(errno, "Failed to read link '/run/udev/watch.old/%s', ignoring: %m", ent->d_name);
                        goto unlink;
                } else if (len >= (ssize_t) sizeof(device)) {
                        log_error("Path specified by link '/run/udev/watch.old/%s' is truncated, ignoring.", ent->d_name);
                        goto unlink;
                }
                device[len] = '\0';

                r = sd_device_new_from_device_id(&dev, device);
                if (r < 0) {
                        log_error_errno(r, "Failed to create sd_device object for '%s', ignoring: %m", device);
                        goto unlink;
                }

                if (DEBUG_LOGGING) {
                        const char *devnode = NULL;

                        (void) sd_device_get_devname(dev, &devnode);
                        log_debug("Restoring old watch on '%s'", strnull(devnode));
                }
                (void) udev_watch_begin(dev);
unlink:
                (void) unlinkat(dirfd(dir), ent->d_name, 0);
        }

        (void) closedir(dir);
        (void) rmdir("/run/udev/watch.old");

        return 0;
}

int udev_watch_begin(sd_device *dev) {
        char filename[STRLEN("/run/udev/watch/") + DECIMAL_STR_MAX(int)];
        const char *devnode, *id_filename;
        int wd, r;

        if (inotify_fd < 0)
                return log_error_errno(EINVAL, "Invalid inotify descriptor.");

        r = sd_device_get_devname(dev, &devnode);
        if (r < 0)
                return log_error_errno(r, "Failed to get device name: %m");

        log_debug("Adding watch on '%s'", devnode);
        wd = inotify_add_watch(inotify_fd, devnode, IN_CLOSE_WRITE);
        if (wd < 0)
                return log_error_errno(errno, "Failed to add device '%s' to watch: %m", devnode);

        device_set_watch_handle(dev, wd);

        xsprintf(filename, "/run/udev/watch/%d", wd);
        r = mkdir_parents(filename, 0755);
        if (r < 0)
                return log_error_errno(r, "Failed to create parent directory of '%s': %m", filename);
        (void) unlink(filename);

        r = device_get_id_filename(dev, &id_filename);
        if (r < 0)
                return log_error_errno(r, "Failed to get device id-filename: %m");

        if (symlink(id_filename, filename) < 0)
                return log_error_errno(errno, "Failed to create symlink %s: %m", filename);

        return 0;
}

int udev_watch_end(sd_device *dev) {
        char filename[STRLEN("/run/udev/watch/") + DECIMAL_STR_MAX(int)];
        const char *devnode;
        int wd, r;

        if (inotify_fd < 0)
                return log_error_errno(EINVAL, "Invalid inotify descriptor.");

        r = device_get_watch_handle(dev, &wd);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return log_error_errno(r, "Failed to get watch handle for device '%s', ignoring: %m", devnode);

        r = sd_device_get_devname(dev, &devnode);
        if (r < 0)
                return log_error_errno(r, "Failed to get device name: %m");

        log_debug("Removing watch on '%s'", devnode);
        (void) inotify_rm_watch(inotify_fd, wd);

        xsprintf(filename, "/run/udev/watch/%d", wd);
        (void) unlink(filename);

        device_set_watch_handle(dev, -1);

        return 0;
}

int udev_watch_lookup(int wd, sd_device **ret) {
        char filename[STRLEN("/run/udev/watch/") + DECIMAL_STR_MAX(int)], device[PATH_MAX];
        ssize_t len;
        int r;

        assert(ret);

        if (inotify_fd < 0)
                return log_error_errno(EINVAL, "Invalid inotify descriptor.");

        if (wd < 0)
                return log_error_errno(EINVAL, "Invalid watch handle.");

        xsprintf(filename, "/run/udev/watch/%d", wd);
        len = readlink(filename, device, sizeof(device));
        if (len <= 0) {
                if (errno != ENOENT)
                        return log_error_errno(errno, "Failed to read link '%s': %m", filename);
                return 0;
        } else if (len >= (ssize_t) sizeof(device))
                return log_error_errno(ENAMETOOLONG, "Path specified by link '%s' is truncated.", filename);
        device[len] = '\0';

        r = sd_device_new_from_device_id(ret, device);
        if (r < 0)
                return log_error_errno(r, "Failed to create sd_device object for '%s': %m", device);

        return 0;
}
