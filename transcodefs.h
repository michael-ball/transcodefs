#define FUSE_USE_VERSION 26

#include <stdio.h>
#include <stdlib.h>
#include <fuse.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <syslog.h>

/* Global program parameters */
extern struct transcodefs_params {
    const char *basepath;
    unsigned int bitrate;
    unsigned int thresholdbitrate;
    int debug;
} params;

/* Fuse operations struct */
extern struct fuse_operations transcodefs_oper;

#define transcodefs_debug(f, ...) syslog(LOG_DEBUG, f, ## __VA_ARGS__)
#define transcodefs_info(f, ...) syslog(LOG_INFO, f, ## __VA_ARGS__)
#define transcodefs_error(f, ...) syslog(LOG_ERR, f, ## __VA_ARGS__)


