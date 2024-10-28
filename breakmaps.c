#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// needs a fs with at least 4Mi inodes
// and quite a bit of space, but how much is fs-dependent

// e.g. on ext4 this needs 4M * (block + inode) => ~17GiB with default settings
// or like ~5GiB at the very least if you make the block sizes 1KiB
// and probably more because i don't know how to count

// on my system this takes up to ~1min to descend and up to ~3min to clean up
// but i guess it could genuinely take hours on slower disks, so maybe try on a tmpfs?
// if you have like 64GiB ram, your distro probably has a suitable (and fast) fs at /dev/shm

// with tmpfs or a scratch fs, you can also umount in one go, cutting the cleanup short

void shittyshell(void);
uintptr_t lowestmap(void);
int checkfs(void);
void cleanup(int);

volatile sig_atomic_t maxdepth = 4 * 1024 * 1024, currdepth = 0;

int main(int argc, char **argv) {
    if (argc > 1) {
        if (chdir(argv[1]) == -1) {
            fprintf(stderr, "could not chdir %s: %m\n", argv[1]);
            return 1;
        }
    }

    int checkfs();
    if (checkfs() == -1)
        return 1;

    // fs looks suitable
    // let's descend 4M levels deep

    char name[256];
    memset(name, 'd', sizeof name - 1);
    name[255] = 0;
    signal(SIGINT, cleanup);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    t1 = t0;
    double left = INFINITY, diff, total;

    while (currdepth < maxdepth) {
        if (mkdir(name, 0755) == -1) {
            fprintf(stderr, "\ncould not mkdir at depth %d: %m\n", currdepth);
            cleanup(-1);
        }
        if (chdir(name) == -1) {
            fprintf(stderr, "\ncould not chdir at depth %d: %m\n", currdepth);
            cleanup(-1);
        }

        currdepth++;

        if (currdepth % 1000 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            diff = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            total = diff / currdepth * maxdepth;
            left = diff / currdepth * (maxdepth - currdepth);
        }
        fprintf(stderr, "depth %d/%d (time left/total: %.3f/%.3fs)\x1b[K\r", currdepth, maxdepth,
                left, total);
    }
    // your filesystem is probably crying

    memset(name, 'f', sizeof name - 1);
    int fd = open(name, O_CREAT, 0644);
    if (fd == -1) {
        fprintf(stderr, "\ncould not open the file at depth %d: %m\n", currdepth);
        cleanup(-1);
    }
    if (unlink(name) == -1) {
        fprintf(stderr, "\ncould not unlink file at depth %d: %m (CLEANUP WILL LIKELY FAIL)\n",
                currdepth);
        cleanup(-1);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    diff = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "\ndescent time: %.3fs\n", diff);

    // find an address where to place our funky map
    uintptr_t lo = lowestmap();
    if (lo == (uintptr_t)-1)
        cleanup(-1);

    void *target = (void *)(lo - 4096);

    // try to place a map one page lower than all the other maps
    void *m = mmap(target, 4096, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    if (m == MAP_FAILED) {
        fprintf(stderr, "could not mmap at %p: %m\n", target);
        cleanup(-1);
    }
    close(fd);

    // done!!!  your /proc/self/maps should now be unreadable

    // i wanted to just drop you into a shell so you could try things out, with like system("sh")
    // but basically all shells want to know what their cwd is...  how silly...
    // we can't have that, so...
    shittyshell();

    munmap(m, 4096);

    cleanup(0);
}

void cleanup(int sig) {
    char name[256];
    name[255] = 0;

    fputs("\n", stderr);
    if (sig > 0)
        fprintf(stderr, "caught signal %d, attempting cleanup\n", sig);
    else if (sig == 0)
        fputs("cleanup\n", stderr);
    else {
        fputs("attempting cleanup\n", stderr);
        if (currdepth >= maxdepth - 1) {
            memset(name, 'f', sizeof name - 1);
            unlink(name);
        }
    }

    memset(name, 'd', sizeof name - 1);
    rmdir(name);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    t1 = t0;
    double left = INFINITY, diff, total;

    int startdepth = currdepth;
    while (currdepth > 0) {
        if (chdir("..") == -1) {
            fprintf(stderr, "\ncould not chdir .. at depth %d: %m\n", currdepth);
            exit(1);
        }
        if (rmdir(name) == -1) {
            fprintf(stderr, "\ncould not rmdir at depth %d: %m\n", currdepth);
            exit(1);
        }

        currdepth--;

        if (currdepth % 1000 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            diff = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            total = diff / (startdepth - currdepth) * startdepth;
            left = diff / (startdepth - currdepth) * currdepth;
        }
        fprintf(stderr, "depth %d/%d (time left/total: %.3f/%.3fs)\x1b[K\r", currdepth, maxdepth,
                left, total);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    diff = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "\ncleanup time: %.3fs\n", diff);

    exit(sig != 0);
}

int checkfs() {
    struct statfs fs;
    if (statfs(".", &fs) == -1) {
        fprintf(stderr, "could not statfs .: %m\n");
        return -1;
    }

    size_t want = maxdepth + 10; // just to be safe
    if (fs.f_ffree < want) {
        fprintf(stderr, "fs needs at least %zu free inodes\n", want);
        return -1;
    }
    if (fs.f_bavail < want) {
        fprintf(stderr, "fs needs at least %zu free blocks (possibly more idk)\n", want);
        return -1;
    }

    int rootfd = open(".", 0);
    if (rootfd == -1) {
        fprintf(stderr, "could not open .: %m\n");
        return -1;
    }

    return 0;
}

uintptr_t lowestmap() {
    DIR *dir = opendir("/proc/self/map_files");
    if (!dir) {
        fprintf(stderr, "\ncould not open /proc/self/map_files: %m\n");
        return -1;
    }

    puts("");
    struct dirent *ent;
    ent = readdir(dir); // gets .
    if (!ent)
        return -1;
    ent = readdir(dir); // gets ..
    if (!ent)
        return -1;
    ent = readdir(dir); // gets the lowest map
    if (!ent)
        return -1;

    uintptr_t lo = -1;
    sscanf(ent->d_name, "%" SCNxPTR, &lo);
    closedir(dir);

    return lo;
}

void shittyshell() {
    fprintf(stderr, "shell pid: %d\n", getpid());
    fprintf(stderr, "hint: check /proc/%d/maps and the likes\n", getpid());

    while (1) {
        fprintf(stderr, "$ ");
        static char line[1024];
        if (scanf(" %1023[^\n]%*[^\n]", line) < 1 || strcmp(line, "exit") == 0)
            break;

        char *files[3] = {};

        static char *tokens[1024];
        char **currtok = tokens;
        char *token = strtok(line, " ");
        while (token) {
            char *redirs[] = {"<", ">", "2>"};
            for (int i = 0; i < 3; i++) {
                if (strncmp(token, redirs[i], strlen(redirs[i])) == 0) {
                    token += strlen(redirs[i]);
                    if (!token[0])
                        token = strtok(NULL, " ");
                    files[i] = token;
                    goto next;
                }
            }
            *currtok++ = token;
        next:
            token = strtok(NULL, " ");
        }
        *currtok = NULL;

        pid_t pid = fork();
        if (pid == -1) {
            fprintf(stderr, "could not fork: %m\n");
            break;
        }
        if (pid == 0) {
            int fds[3] = {-1, -1, -1};
            for (int i = 0; i < 3; i++) {
                if (files[i]) {
                    fds[i] = open(files[i], i == 0 ? O_RDONLY : O_WRONLY | O_TRUNC | O_CREAT, 0664);
                    if (fds[i] == -1)
                        fprintf(stderr, "could not open %s: %m\n", files[i]);
                }
            }
            for (int i = 0; i < 3; i++) {
                if (fds[i] != -1) {
                    dup2(fds[i], i);
                    close(fds[i]);
                }
            }
            execvp(tokens[0], tokens);
            exit(127);
        }
        waitpid(pid, NULL, 0);
    }
}
