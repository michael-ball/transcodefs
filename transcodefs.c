#include <dirent.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

#include "transcodefs.h"

struct transcodefs_params params = {
    .basepath           = NULL,
    .bitrate            = 64,
    .thresholdbitrate   = 256,
    .debug              = 0,
};

enum {
    KEY_HELP,
    KEY_VERSION,
    KEY_KEEP_OPT
};

#define TRANSCODEFS_OPT(t, p, v) { t, offsetof(struct transcodefs_params, p), v }

static struct fuse_opt transcodefs_opts[] = {
    TRANSCODEFS_OPT("-d", debug, 1),
    TRANSCODEFS_OPT("debug", debug, 1),
    TRANSCODEFS_OPT("-b %u", bitrate, 0),
    TRANSCODEFS_OPT("bitrate=%u", bitrate, 0),
    TRANSCODEFS_OPT("-tb %u", thresholdbitrate, 0),
    TRANSCODEFS_OPT("thresholdbitrate=%u", thresholdbitrate, 0),

    FUSE_OPT_KEY("-h",            KEY_HELP),
    FUSE_OPT_KEY("--help",        KEY_HELP),
    FUSE_OPT_KEY("-V",            KEY_VERSION),
    FUSE_OPT_KEY("--version",     KEY_VERSION),
    FUSE_OPT_KEY("-d",            KEY_KEEP_OPT),
    FUSE_OPT_KEY("debug",         KEY_KEEP_OPT),
    FUSE_OPT_END
};

typedef struct _DiscovererData {
    GstDiscoverer *discoverer;
    guint thresholdbitrate;
} DiscovererData;

static DiscovererData data;

void usage(char *name) {
    printf("Usage: %s [OPTION]... MEDIADIR MOUNTDIR\n", name);
    fputs("\
Mount MEDIADIR on MOUNTDIR, transcoding media files on access.\n\
\n\
Encoding options:\n\
    -b RATE, -obitrate=RATE\n\
\n\
Filter options:\n\
    -tb RATE, -othresholdbitrate=RATE\n\
\n\
General options:\n\
    -h, --help             display this help and exit\n\
    -V, --version          output version information and exit\n\
\n", stdout);
}

static int transcodefs_opt_proc(void *data, const char *arg, int key,
                          struct fuse_args *outargs) {
    switch(key) {
        case FUSE_OPT_KEY_NONOPT:
            // check for mediadir and bitrate parameters
            if (!params.basepath) {
                params.basepath = arg;
                return 0;
            }
            break;

        case KEY_HELP:
            usage(outargs->argv[0]);
            fuse_opt_add_arg(outargs, "-ho");
            fuse_main(outargs->argc, outargs->argv, &transcodefs_oper, NULL);
            exit(1);

        case KEY_VERSION:
            printf("TRANSCODEFS version %s\n", "0.1");
            fuse_opt_add_arg(outargs, "--version");
            fuse_main(outargs->argc, outargs->argv, &transcodefs_oper, NULL);
            exit(0);
    }

    return 1;
}

/*
 * Translate file names from FUSE to the original absolute path. A buffer
 * is allocated using malloc for the translated path. It is the caller's
 * responsibility to free it.
 */
char* translate_path(const char* path) {
    char* result;
    /*
     * Allocate buffer. The +2 is for the terminating '\0' and to
     * accomodate possibly translating .mp3 to .flac later.
     */
    result = malloc(strlen(params.basepath) + strlen(path) + 2);
    
    if (result) {
        strcpy(result, params.basepath);
        strcat(result, path);
    }
    
    return result;
}

/* Convert file extension between opus and flac. */
void convert_path(char* path, int filetype, const char* parent) {
    
    char* ptr;
    char* prefix = "file://";
    char full_path[strlen(path) + strlen(parent) + strlen(params.basepath) + strlen(prefix) + 2];
    GstDiscovererInfo* info;
    GList* stream_list;
    GstDiscovererAudioInfo* astream;
    double bitrate;

    snprintf(full_path, sizeof full_path, "%s%s%s%s%s", prefix, params.basepath, parent, "/", path);
    ptr = strrchr(path, '.');


    switch (filetype) {
        case 1:
            if (ptr) {
                strcpy(ptr, ".flac");
            }
            break;
        
        case 2:
            if (ptr) {
                strcpy(ptr, ".mp3");
            }
            break;
        
        case 3:
            if (ptr) {
                strcpy(ptr, ".ogg");
            }
            break;
    
        default : 

            /* Add a request to process synchronously the URI passed through the command line */
            info = gst_discoverer_discover_uri(data.discoverer, full_path, NULL);

            if (!info) {
                transcodefs_debug("Failed to start discovering URI '%s'\n", full_path);
                g_object_unref(data.discoverer);
            } else {
                // if the file is flac, rename immediately
                if (ptr && strcmp(ptr, ".flac") == 0) {
                    strcpy(ptr, ".opus");
                } else { // otherwise check its bitrate to see if it's above the threshold
                
                    // get the stream list
                    stream_list = gst_discoverer_info_get_audio_streams(info);
                    
                    //TODO: Handle multiple audio streams intelligently
                    transcodefs_debug("Found %d streams in %s", g_list_length(stream_list), full_path);
                    if (g_list_first(stream_list)) {
                        astream = g_list_nth_data(stream_list,0);

                        bitrate = gst_discoverer_audio_info_get_bitrate(astream) / 1000;
                        transcodefs_debug("Found bitrate of %.0f for %s", bitrate, full_path);
                    }
                    // free the list of streams
                    gst_discoverer_stream_info_list_free(stream_list); 
                    if ( bitrate >= data.thresholdbitrate) {
                        strcpy(ptr, ".opus");
                    }
                }  
            }
    }
}

static int transcodefs_readlink(const char *path, char *buf, size_t size) {
    char* origpath;
    ssize_t len;
    
    transcodefs_debug("readlink %s", path);
    
    errno = 0;
    
    origpath = translate_path(path);
    if (!origpath) {
        goto translate_fail;
    }
    
    convert_path(origpath, 1, path);
    
    len = readlink(origpath, buf, size - 2);
    if (len == -1) {
        transcodefs_debug("Readlink fail for %s", origpath);
        
        convert_path(origpath, 2, path);
        
        len = readlink(origpath, buf, size - 2);
        if (len == -1) {
            transcodefs_debug("Readlink fail for %s", origpath);
            goto readlink_fail;
        }
    }
    transcodefs_debug("Readlink SUCCESS for %s", origpath);
    
    buf[len] = '\0';
    
    convert_path(buf, 0, path);
    
readlink_fail:
    transcodefs_debug("readlink fail");
    free(origpath);
translate_fail:
    return -errno;
}

static int transcodefs_getattr(const char *path, struct stat *stbuf) {
    char* origpath;
    //struct transcoder* trans;
    
    transcodefs_debug("getattr %s", path);
    
    errno = 0;
    
    origpath = translate_path(path);
    if (!origpath) {
        goto translate_fail;
    }
    
    /* pass-through for regular files */
    if (lstat(origpath, stbuf) == 0) {
        goto passthrough;
    } else {
        /* Not really an error. */
        errno = 0;
    }
    
    convert_path(origpath, 1, path);
    
    if (lstat(origpath, stbuf) == -1) {
        transcodefs_debug("Stat fail for %s", origpath);
        
        convert_path(origpath, 2, path);
        
        errno = 0;

        if (lstat(origpath, stbuf) == -1) {
            transcodefs_debug("Stat fail for %s", origpath);
            goto stat_fail;
        }
    }
    transcodefs_debug("Stat SUCCESS for %s", origpath);
    
    /*
     * Get size for resulting mp3 from regular file, otherwise it's a
     * symbolic link. */
    //if (S_ISREG(stbuf->st_mode)) {
    //    trans = transcoder_new(origpath);
    //    if (!trans) {
    //        goto transcoder_fail;
    //    }
    //    
    //    stbuf->st_size = trans->totalsize;
    //    stbuf->st_blocks = (stbuf->st_size + 512 - 1) / 512;
    //    
    //    transcoder_finish(trans);
    //    transcoder_delete(trans);
    //}
    
//transcoder_fail:
stat_fail:
passthrough:
    free(origpath);
translate_fail:
    return -errno;
}

static int transcodefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    char* origpath;
    char* origfile;
    DIR *dp;
    struct dirent *de;
    
    transcodefs_debug("readdir %s", path);
    
    errno = 0;
    
    origpath = translate_path(path);
    if (!origpath) {
        goto translate_fail;
    }
    
    /* 2 for directory separator and NULL byte */
    origfile = malloc(strlen(origpath) + NAME_MAX + 2);
    if (!origfile) {
        goto origfile_fail;
    }
    
    dp = opendir(origpath);
    if (!dp) {
        goto opendir_fail;
    }
    
    while ((de = readdir(dp))) {
        struct stat st;
        
        snprintf(origfile, strlen(origpath) + NAME_MAX + 2, "%s/%s", origpath,
                 de->d_name);
        
        if (lstat(origfile, &st) == -1) {
            goto stat_fail;
        } else {
            if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
                convert_path(de->d_name, 0, path);
            }
        }
        
        if (filler(buf, de->d_name, &st, 0)) break;
    }
    
stat_fail:
    closedir(dp);
opendir_fail:
    free(origfile);
origfile_fail:
    free(origpath);
translate_fail:
    return -errno;
}

static int transcodefs_open(const char *path, struct fuse_file_info *fi) {
    char* origpath;
    //struct transcoder* trans;
    int fd;
    
    transcodefs_debug("open %s", path);
    
    errno = 0;
    
    origpath = translate_path(path);
    if (!origpath) {
        goto translate_fail;
    }
    
    fd = open(origpath, fi->flags);
    
    /* File does exist, but can't be opened. */
    if (fd == -1 && errno != ENOENT) {
        goto open_fail;
    } else {
        /* Not really an error. */
        errno = 0;
    }
    
    /* File is real and can be opened. */
    if (fd != -1) {
        close(fd);
        goto passthrough;
    }
    
    convert_path(origpath, 1, path);
    
    fd = open(origpath, fi->flags);
    
    if (fd == -1) {
        errno = 0;
        convert_path(origpath, 2, path);
        
        fd = open(origpath, fi->flags);

    } 
    /* File is real and can be opened. */
    if (fd != -1) {
        close(fd);
        goto passthrough;
    }
    
    //trans = transcoder_new(origpath);
    //if (!trans) {
    //    goto transcoder_fail;
    //}
    
    /* Store transcoder in the fuse_file_info structure. */
    //fi->fh = (uint64_t)trans;
    
//transcoder_fail:
passthrough:
open_fail:
    free(origpath);
translate_fail:
    return -errno;
}

static int transcodefs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    char* origpath;
    int fd;
    int read = 0;
    //struct transcoder* trans;
    
    transcodefs_debug("read %s: %zu bytes from %jd", path, size, (intmax_t)offset);
    
    errno = 0;
    
    origpath = translate_path(path);
    if (!origpath) {
        goto translate_fail;
    }
    
    /* If this is a real file, pass the call through. */
    fd = open(origpath, O_RDONLY);
    if (fd != -1) {
        read = pread(fd, buf, size, offset);
        close(fd);
        goto passthrough;
    } else if (errno != ENOENT) {
        /* File does exist, but can't be opened. */
        goto open_fail;
    } else {
        /* File does not exist, and this is fine. */
        errno = 0;
    }
    
    //trans = (struct transcoder*)fi->fh;
    
    //if (!trans) {
    //    transcodefs_error("Tried to read from unopen file: %s", origpath);
    //    goto transcoder_fail;
    //}
    
    //read = transcoder_read(trans, buf, offset, size);
    
//transcoder_fail:
passthrough:
open_fail:
    free(origpath);
translate_fail:
    if (read) {
        return read;
    } else {
        return -errno;
    }
}

static int transcodefs_statfs(const char *path, struct statvfs *stbuf) {
    char* origpath;
    
    transcodefs_debug("statfs %s", path);
    
    errno = 0;
    
    origpath = translate_path(path);
    if (!origpath) {
        goto translate_fail;
    }
    
    statvfs(origpath, stbuf);
    
    free(origpath);
translate_fail:
    return -errno;
}

static int transcodefs_release(const char *path, struct fuse_file_info *fi) {
    //struct transcoder* trans;
    
    transcodefs_debug("release %s", path);
    
    //trans = (struct transcoder*)fi->fh;
    //if (trans) {
    //    transcoder_finish(trans);
    //    transcoder_delete(trans);
    //}
    
    return 0;
}

/* We need synchronous reads. */
static void *transcodefs_init(struct fuse_conn_info *conn) {
    conn->async_read = 0;
    
    return NULL;
}

struct fuse_operations transcodefs_oper = {
    .getattr = transcodefs_getattr,
    .readlink = transcodefs_readlink,
    .readdir = transcodefs_readdir,
    .open = transcodefs_open,
    .read = transcodefs_read,
    .statfs = transcodefs_statfs,
    .release = transcodefs_release,
    .init = transcodefs_init,
};

int main(int argc, char** argv) {
    int ret;
    GError *err = NULL;

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    if (fuse_opt_parse(&args, &params, transcodefs_opts, transcodefs_opt_proc)) {
        fprintf(stderr, "Error parsing options.\n\n");
        usage(argv[0]);
        return 1;
    }

    if (!params.basepath) {
        fprintf(stderr, "mediadir must be an absolute path.\n\n");
    }
    
    struct stat st;    
    if (stat(params.basepath, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "mediadir is not a valid directory: %s\n", params.basepath);
        usage(argv[0]);
        return 1;
    }

    openlog("transcodefs", params.debug? LOG_PERROR : 0, LOG_USER);

    transcodefs_debug("TranscodeFS options:\n"
        "basepath: %s\n"
        "bitrate: %u\n"
        "thresholdbitrate: %u\n"
        "\n",
        params.basepath, params.bitrate, params.thresholdbitrate);

    memset(&data, 0, sizeof(data));

    // pass thresholdbitrate into the data container
    data.thresholdbitrate = params.thresholdbitrate;

    gst_init(NULL,NULL);

    // Start the Gstreamer Discoverer
    data.discoverer = gst_discoverer_new (5 * GST_SECOND, &err);
    if (!data.discoverer) {
        transcodefs_debug("Error creating discoverer instance: %s\n", err->message);
        g_clear_error(&err);
        return -1;
    }

    ret = fuse_main(args.argc, args.argv, &transcodefs_oper, NULL);

    g_object_unref(data.discoverer);

    fuse_opt_free_args(&args);

    return ret;
}



