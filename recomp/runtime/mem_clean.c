#include "mem_clean.h"
#include "settings.h"
#include "platform.h"
#include "rt.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
#endif

__attribute__((weak)) void vk_collect_dead(void);

static pthread_t s_clean_thread;
static bool s_running = false;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static bool s_trigger_now = false;

static void compute_deadline(struct timespec *ts, int seconds) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += seconds;
}

void mem_clean_now(void) {
    if (vk_collect_dead) {
        vk_collect_dead();
    }
#ifdef _WIN32
    HANDLE heaps[64];
    DWORD n = GetProcessHeaps(64, heaps);
    if (n > 0 && n <= 64) {
        for (DWORD i = 0; i < n; i++) {
            if (heaps[i]) HeapCompact(heaps[i], 0);
        }
    } else {
        HeapCompact(GetProcessHeap(), 0);
    }
    _heapmin();
#elif defined(__GLIBC__) || defined(__linux__)
    extern int malloc_trim(size_t pad) __attribute__((weak));
    if (malloc_trim) malloc_trim(0);
#endif
    LOG("[mem_clean] dynamic memory trim executed\n");
}

static void *clean_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&s_lock);
    while (s_running) {
        if (!g_settings.mem_cleanup) {
            // Disabled: wait until notified
            pthread_cond_wait(&s_cond, &s_lock);
            if (!s_running) break;
            if (!g_settings.mem_cleanup) continue;
        }

        int interval = g_settings.mem_cleanup_interval;
        if (interval < 5) interval = 5;
        if (interval > 600) interval = 600;

        struct timespec ts;
        compute_deadline(&ts, interval);

        while (s_running && !s_trigger_now && g_settings.mem_cleanup) {
            int r = pthread_cond_timedwait(&s_cond, &s_lock, &ts);
            if (r != 0) break; // timeout reached
        }

        if (!s_running) break;
        bool do_clean = s_trigger_now || g_settings.mem_cleanup;
        s_trigger_now = false;

        if (do_clean && g_settings.mem_cleanup) {
            pthread_mutex_unlock(&s_lock);
            mem_clean_now();
            pthread_mutex_lock(&s_lock);
        }
    }
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

void mem_clean_init(void) {
    pthread_mutex_lock(&s_lock);
    if (!g_settings.mem_cleanup) {
        pthread_mutex_unlock(&s_lock);
        LOG("[mem_clean] dynamic memory cleaner disabled\n");
        return;
    }
    if (s_running) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    s_running = true;
    s_trigger_now = false;
    pthread_create(&s_clean_thread, NULL, clean_worker, NULL);
    pthread_mutex_unlock(&s_lock);
    LOG("[mem_clean] dynamic memory cleaner initialized (interval: %d s)\n", g_settings.mem_cleanup_interval);
}

void mem_clean_shutdown(void) {
    pthread_mutex_lock(&s_lock);
    if (!s_running) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    s_running = false;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);

    pthread_join(s_clean_thread, NULL);
    LOG("[mem_clean] dynamic memory cleaner stopped\n");
}

void mem_clean_update(void) {
    pthread_mutex_lock(&s_lock);
    if (g_settings.mem_cleanup) {
        if (!s_running) {
            s_running = true;
            s_trigger_now = false;
            pthread_create(&s_clean_thread, NULL, clean_worker, NULL);
            LOG("[mem_clean] dynamic memory cleaner enabled (interval: %d s)\n", g_settings.mem_cleanup_interval);
        } else {
            pthread_cond_broadcast(&s_cond);
        }
    } else {
        if (s_running) {
            LOG("[mem_clean] dynamic memory cleaner disabled\n");
            pthread_cond_broadcast(&s_cond);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

void clean_courses_and_thumbnails(void) {
    LOG("[cleanup] cleaning cached courses and thumbnails...\n");
    int count = 0;

    // 1. Clean server/courses directory
    DIR *sd = opendir("server/courses");
    if (sd) {
        struct dirent *e;
        while ((e = readdir(sd))) {
            if (e->d_name[0] == '.') continue;
            char path[1024];
            snprintf(path, sizeof path, "server/courses/%s", e->d_name);
            if (unlink(path) == 0) count++;
        }
        closedir(sd);
    }

    // 2. Clean save directories (save/save_0, save/save_1, save)
    const char *save_dirs[] = {"save/save_0", "save/save_1", "save"};
    for (size_t i = 0; i < sizeof(save_dirs) / sizeof(save_dirs[0]); i++) {
        DIR *d = opendir(save_dirs[i]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            // CRITICAL: NEVER touch save.dat, quest.dat, myworld.dat, or network.dat
            if (!strcmp(e->d_name, "save.dat")) continue;
            if (!strcmp(e->d_name, "quest.dat")) continue;
            if (!strcmp(e->d_name, "myworld.dat")) continue;
            if (strstr(e->d_name, "network.dat")) continue;

            // Delete thumbnail cache files (.btl) for downloaded courses (slots >= 60)
            // User-created courses (slots 000-059) are ALWAYS preserved!
            bool is_downloaded_thumb = false;
            if (strstr(e->d_name, "course_thumb_") != NULL && strstr(e->d_name, ".btl") != NULL) {
                int slot = -1;
                if (sscanf(e->d_name, "course_thumb_%d.btl", &slot) == 1) {
                    if (slot >= 60) is_downloaded_thumb = true;
                } else {
                    is_downloaded_thumb = true;
                }
            }
            // Delete downloaded courses (slots >= 60)
            bool is_downloaded_course = false;
            if (strstr(e->d_name, "course_data_") != NULL && strstr(e->d_name, ".bcd") != NULL) {
                int slot = -1;
                if (sscanf(e->d_name, "course_data_%d.bcd", &slot) == 1) {
                    if (slot >= 60) is_downloaded_course = true;
                }
            }
            // Delete downloaded replays (slots >= 60)
            bool is_downloaded_replay = false;
            if (strstr(e->d_name, "course_replay_") != NULL && strstr(e->d_name, ".dat") != NULL) {
                int slot = -1;
                if (sscanf(e->d_name, "course_replay_%d.dat", &slot) == 1) {
                    if (slot >= 60) is_downloaded_replay = true;
                }
            }
            // Delete later.dat (Play Later / downloaded course list)
            bool is_later = (!strcmp(e->d_name, "later.dat"));

            if (is_downloaded_thumb || is_downloaded_course || is_downloaded_replay || is_later) {
                char p[1024];
                snprintf(p, sizeof p, "%s/%s", save_dirs[i], e->d_name);
                if (unlink(p) == 0) {
                    count++;
                    LOG("[cleanup] removed %s\n", p);
                }
            }
        }
        closedir(d);
    }

    LOG("[cleanup] finished (%d items removed)\n", count);
}

