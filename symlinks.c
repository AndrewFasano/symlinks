#include <unistd.h>

#ifndef _POSIX_SOURCE
#define _POSIX_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>

#if !defined(__APPLE__)
#include <malloc.h>
#endif

#include <string.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <stddef.h>
#include <errno.h>
#include <libgen.h> // for dirname() and basename()

#ifndef S_ISLNK
#define S_ISLNK(mode) (((mode) & (_S_IFMT)) == (_S_IFLNK))
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define progver "%s: scan/change symbolic links - v1.4.3\n\n"

static char *progname;
static int verbose = 0,
            fix_links = 0,
            recurse = 0,
            delete = 0,
            shorten = 0,
            testing = 0,
            single_fs = 1,
            emb_rootfs = 0;


/*
 * tidypath removes excess slashes and "." references from a path string
 */

static int substr(char *s, char *old, char *new) {
    char *tmp = NULL;
    unsigned long oldlen = strlen(old), newlen = 0;
    
    if (NULL == strstr(s, old)) {
        return 0;
    }
    
    if (new) {
        newlen = strlen(new);
    }
    
    if (newlen > oldlen) {
        if ((tmp = malloc(strlen(s))) == NULL) {
            fprintf(stderr, "no memory\n");
            exit(1);
        }
    }
    
    while (NULL != (s = strstr(s, old))) {
        char *p, *old_s = s;
        
        if (new) {
            if (newlen > oldlen) {
                old_s = strcpy(tmp, s);
            }
            
            p = new;
            
            while (*p) {
                *s++ = *p++;
            }
        }
        
        p = old_s + oldlen;
        
        while ((*s++ = *p++));
    }
    
    if (tmp) {
        free(tmp);
    }
    
    return 1;
}


static int tidy_path(char *path) {
    int tidied = 0;
    char *s, *p;
    s = path + strlen(path) - 1;
    
    if (s[0] != '/') {  /* tmp trailing slash simplifies things */
        s[1] = '/';
        s[2] = '\0';
    }
    
    while (substr(path, "/./", "/")) {
        tidied = 1;
    }
    
    while (substr(path, "//", "/")) {
        tidied = 1;
    }
    
    while ((p = strstr(path, "/../")) != NULL) {
        s = p + 3;
        
        for (p--; p != path; p--) {
            if (*p == '/') { break; }
        }
        
        if (*p != '/') { break; }
        
        while ((*p++ = *s++));
        
        tidied = 1;
    }
    
    if (*path == '\0') {
        strcpy(path, "/");
    }
    
    p = path + strlen(path) - 1;
    
    if (p != path && *p == '/') {
        *p-- = '\0';  /* remove tmp trailing slash */
    }
    
    while (p != path && *p == '/') {  /* remove any others */
        *p-- = '\0';
        tidied = 1;
    }
    
    while (!strncmp(path, "./", 2)) {
        for (p = path, s = path + 2; (*p++ = *s++););
        
        tidied = 1;
    }
    
    return tidied;
}


static int shorten_path(char *path, char *abspath) {
    static char dir[PATH_MAX];
    int shortened = 0;
    char *p;
    
    /* get rid of unnecessary "../dir" sequences */
    while (abspath && strlen(abspath) > 1 && (p = strstr(path, "../"))) {
        /* find innermost occurance of "../dir", and save "dir" */
        int slashes = 2;
        char *a, *s, *d = dir;
        
        while ((s = strstr(p + 3, "../"))) {
            ++slashes;
            p = s;
        }
        
        s = p + 3;
        *d++ = '/';
        
        while (*s && *s != '/') {
            *d++ = *s++;
        }
        
        *d++ = '/';
        *d = '\0';
        
        if (!strcmp(dir, "//")) {
            break;
        }
        
        /* note: p still points at ../dir */
        if (*s != '/' || !*++s) {
            break;
        }
        
        a = abspath + strlen(abspath) - 1;
        
        while (slashes-- > 0) {
            if (a <= abspath) {
                goto ughh;
            }
            
            while (*--a != '/') {
                if (a <= abspath) {
                    goto ughh;
                }
            }
        }
        
        if (strncmp(dir, a, strlen(dir))) {
            break;
        }
        
        while ((*p++ = *s++));  /* delete the ../dir */
        
        shortened = 1;
    }
    
ughh:
    return shortened;
}

char* convert_to_relative_path(char *cwd, char *path, char *abs_target) {
    char *cwd_copy = strdup(cwd); // Duplicate because dirname and basename may modify the input
    char *path_copy = strdup(path);
    char *target_copy = strdup(abs_target);
    char *result = (char*)malloc(PATH_MAX); // Allocate memory for the result
    if (!result || !cwd_copy || !path_copy || !target_copy) {
        // Handle allocation failure
        free(cwd_copy);
        free(path_copy);
        free(target_copy);
        return NULL;
    }

    //char *path_dir = dirname(path_copy); // Directory part of the path
    char *rel_path = NULL;
    size_t up_count = 0;

    // Count how many levels up we need to go from the path directory to reach the common base with the target
    char *common_base = cwd_copy;
    while (strncmp(common_base, target_copy, strlen(common_base)) != 0) {
        char *parent_dir = dirname(common_base);
        if (strcmp(parent_dir, common_base) == 0) { // Reached the root without complete match
            break;
        }
        common_base = strdup(parent_dir);
        up_count++;
    }

    // Construct the relative path
    rel_path = result;
    for (size_t i = 0; i < up_count; i++) {
        strcat(rel_path, "../");
        rel_path += 3; // Move past the "../"
    }

    // Append the unique part of the target path that follows the common base
    size_t base_len = strlen(common_base);
    if (strlen(abs_target) > base_len) {
        strcat(rel_path, abs_target + base_len + 1); // +1 to skip the leading '/'
    }

    free(cwd_copy);
    free(path_copy);
    free(target_copy);

    return result;
}

static void fix_symlink(char *cwd, char *path, dev_t my_dev) {
    static char lpath[PATH_MAX], new[PATH_MAX];
    //struct stat lstbuf;
    ssize_t c;
    
    // Read the target of the symlink
    c = readlink(path, lpath, sizeof(lpath) - 1);
    if (c == -1) {
        perror(path);
        return;
    }
    lpath[c] = '\0'; // Ensure null-termination

    // Determine if the symlink is absolute and handle it accordingly
    int is_absolute = lpath[0] == '/';

    if (is_absolute) {
        // Convert the absolute symlink to a relative one
        char *relative_target = convert_to_relative_path(cwd, path, lpath);
        if (relative_target) {
            strncpy(new, relative_target, PATH_MAX);
            free(relative_target); // Assume convert_to_relative_path dynamically allocates memory
        } else {
            fprintf(stderr, "Error converting to relative path\n");
            return;
        }
    } else {
        // If already relative, simply copy it over
        strncpy(new, lpath, PATH_MAX);
    }

    // Optionally, further tidy, shorten, or process the new path here
    tidy_path(new);

    if (shorten) {
        shorten_path(new, path);
    }

    // Check if we are in testing mode or actually should fix the link
    if (!testing) {
        if (unlink(path)) {
            perror("unlink");
            return;
        }
        
        if (symlink(new, path)) {
            perror("symlink");
            return;
        }
    }

    printf("changed: %s -> %s\n", path, new);
}

static void dirwalk(char *cwd, char *path, unsigned long pathlen, dev_t dev) {
    char *name;
    DIR *dfd;
    static struct stat st;
    static struct dirent *dp;
    
    if ((dfd = opendir(path)) == NULL) {
        perror(path);
        return;
    }
    
    name = path + pathlen;
    
    if (*(name - 1) != '/') {
        *name++ = '/';
    }
    
    while ((dp = readdir(dfd)) != NULL) {
        strcpy(name, dp->d_name);
        
        if (strcmp(name, ".") && strcmp(name, "..")) {
            if (lstat(path, &st) == -1) {
                perror(path);
            } else if (st.st_dev == dev) {
                if (S_ISLNK(st.st_mode)) {
                    fix_symlink(cwd, path, dev);
                } else if (recurse && S_ISDIR(st.st_mode)) {
                    dirwalk(cwd, path, strlen(path), dev);
                }
            }
        }
    }
    
    closedir(dfd);
    path[pathlen] = '\0';
}

static void usage_error(void) {
    fprintf(stderr, progver, progname);
    fprintf(stderr, "Usage:\t%s [-cdorstv] dirlist\n\n", progname);
    fprintf(stderr, "Flags:"
            "\t-c  change absolute/messy links to relative\n"
            "\t-d  delete dangling links\n"
            "\t-o  warn about links across file systems\n"
            "\t-r  recurse into subdirs\n"
            "\t-s  shorten lengthy links (displayed in output only when -c not specified)\n"
            "\t-t  show what would be done by -c\n"
            "\t-e  work on an embedded rootfs which is relative to the system rootfs without\n"
            "\t    the need to chroot / fakechroot into it, e.g. in /gpfs/sys/ubuntu11.04.\n"
            "\t    Make sure to cd into rootfs before executing %s\n"
            "\t-v  verbose (show all symlinks)\n\n", progname);
    exit(1);
}

int main(int argc, char **argv) {
    static char path[PATH_MAX + 2], cwd[PATH_MAX + 2];
    int dircount = 0;
    char c, *p;
    
    if ((progname = (char *) strrchr(*argv, '/')) == NULL) {
        progname = *argv;
    } else {
        progname++;
    }
    
    if (NULL == getcwd(cwd, PATH_MAX)) {
        fprintf(stderr, "getcwd() failed\n");
        exit(1);
    }
    
    if (!*cwd || cwd[strlen(cwd) - 1] != '/') {
        strcat(cwd, "/");
    }

    while (--argc) {
        p = *++argv;
        
        if (*p == '-') {
            if (*++p == '\0') {
                usage_error();
            }
            
            while ((c = *p++)) {
                if (c == 'c') { fix_links  = 1;
                } else if (c == 'd') { delete     = 1;
                } else if (c == 'o') { single_fs  = 0;
                } else if (c == 'r') { recurse    = 1;
                } else if (c == 's') { shorten    = 1;
                } else if (c == 't') { testing    = 1;
                } else if (c == 'e') { emb_rootfs = 1;
                } else if (c == 'v') { verbose    = 1;
                } else {
                    usage_error();
                }
            }
        } else {
            struct stat st;
            if (*p == '/') {
                *path = '\0';
            } else {
                strcpy(path, cwd);
            }
            
            tidy_path(strcat(path, p));
            
            if (lstat(path, &st) == -1) {
                perror(path);
            } else {
                dirwalk(cwd, path, strlen(path), st.st_dev);
            }
            
            ++dircount;
        }
    }
    
    if (dircount == 0) {
        usage_error();
    }
    
    exit(0);
}
