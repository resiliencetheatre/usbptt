#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "wav.h"

#define RATE 16000U
#define CHUNK 1024U
#define BITS_PER_LONG (8U * sizeof(unsigned long))
#define BIT_SET(a, n) (((a)[(n) / BITS_PER_LONG] >> ((n) % BITS_PER_LONG)) & 1UL)

enum audio_mode { SILENT, CAPTURE, PLAYBACK };
struct app {
    const char *capture, *playback, *directory;
    int input, signals;
    bool held, pending, draining, quit, failed;
    unsigned attempts, seconds;
    unsigned long generation;
    int64_t retry_at, deadline;
    enum audio_mode mode;
    snd_pcm_t *pcm;
    FILE *record, *player;
    uint32_t bytes;
    char path[PATH_MAX];
    unsigned char buffer[CHUNK * 2];
    size_t frames, offset;
};

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool usb_match(const char *event)
{
    char link[PATH_MAX], path[PATH_MAX], name[PATH_MAX];
    snprintf(link, sizeof link, "/sys/class/input/%s/device", event);
    if (!realpath(link, path))
        return false;
    for (;;) {
        unsigned vid = 0, pid = 0;
        if (snprintf(name, sizeof name, "%s/idVendor", path) >= (int)sizeof name)
            return false;
        FILE *f = fopen(name, "r");
        if (f) {
            int ok = fscanf(f, "%x", &vid);
            fclose(f);
            if (ok != 1) return false;
            if (snprintf(name, sizeof name, "%s/idProduct", path) >= (int)sizeof name)
                return false;
            f = fopen(name, "r");
            if (!f) return false;
            ok = fscanf(f, "%x", &pid);
            fclose(f);
            return ok == 1 && vid == 0x7273 && pid == 0x8378;
        }
        char *slash = strrchr(path, '/');
        if (!slash || slash == path) return false;
        *slash = '\0';
    }
}

static int keyboard_open(const char *path)
{
    unsigned long keys[KEY_MAX / BITS_PER_LONG + 1] = {0};
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (!BIT_SET(keys, KEY_F2)) {
        close(fd);
        errno = ENOTSUP;
        return -1;
    }
    return fd;
}

static int find_input(const char *selection)
{
    char chosen[PATH_MAX] = "";
    int result = -1;
    unsigned count = 0;
    bool inaccessible = false;
    if (selection) {
        char resolved[PATH_MAX];
        if (!realpath(selection, resolved)) {
            perror(selection);
            return -1;
        }
        const char *event = strrchr(resolved, '/');
        if (!event || strncmp(event + 1, "event", 5) || !usb_match(event + 1)) {
            fprintf(stderr, "%s is not an evdev interface of USB 7273:8378.\n", selection);
            return -1;
        }
        result = keyboard_open(resolved);
        if (result < 0) fprintf(stderr, "%s: %s (requires KEY_F2 and read access).\n",
                                selection, strerror(errno));
        return result;
    }
    DIR *dir = opendir("/sys/class/input");
    if (!dir) { perror("/sys/class/input"); return -1; }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "event", 5) || !usb_match(entry->d_name)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "/dev/input/%s", entry->d_name);
        int fd = keyboard_open(path);
        if (fd < 0) {
            if (errno != ENOTSUP) {
                fprintf(stderr, "%s: %s; use sudo or existing device permissions.\n",
                        path, strerror(errno));
                inaccessible = true;
            }
            continue;
        }
        fprintf(stderr, "Matching PTT keyboard: %s\n", path);
        ++count;
        if (result >= 0) close(result);
        result = fd;
        snprintf(chosen, sizeof chosen, "%s", path);
    }
    closedir(dir);
    if (count != 1 || inaccessible) {
        if (result >= 0) close(result);
        fprintf(stderr, "Cannot uniquely select a keyboard (%u accessible matches). "
                "Connect the microphone/check permissions; use --input /dev/input/eventN "
                "if several units are connected.\n", count);
        return -1;
    }
    printf("Using %s\n", chosen);
    return result;
}

static void close_pcm(struct app *a)
{
    if (a->pcm) {
        snd_pcm_drop(a->pcm);
        snd_pcm_close(a->pcm);
        a->pcm = NULL;
        ++a->generation;
    }
    a->mode = SILENT;
}

static void stop_playback(struct app *a)
{
    if (a->mode == PLAYBACK) close_pcm(a);
    if (a->player) { fclose(a->player); a->player = NULL; }
    a->draining = false;
    a->frames = a->offset = 0;
}

static void finish_recording(struct app *a)
{
    a->pending = false;
    if (a->mode == CAPTURE) close_pcm(a);
    if (!a->record) return;
    int bad = wav_header(a->record, a->bytes);
    if (fclose(a->record)) bad = -1;
    a->record = NULL;
    if (bad) {
        fprintf(stderr, "Cannot finalize %s: %s\n", a->path, strerror(errno));
        a->failed = a->quit = true;
    }
    printf("Saved %s: %.3f seconds (%u audio bytes)\n",
           a->path, a->bytes / 32000.0, a->bytes);
}

static void removed(struct app *a)
{
    fprintf(stderr, "Device removed; stopping audio and exiting.\n");
    a->quit = true;
}

/* All PCM transfers and drain calls are nonblocking. Set params explicitly:
 * no convenience helper that can silently enable resampling for capture. */
static int open_pcm(struct app *a, bool capture)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);
    unsigned latency = 100000;
    int err = snd_pcm_open(&pcm, capture ? a->capture : a->playback,
                          capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK,
                          SND_PCM_NONBLOCK);
    if (err < 0) return err;
#define TRY(call) do { err = (call); if (err < 0) goto fail; } while (0)
    TRY(snd_pcm_hw_params_any(pcm, hw));
    TRY(snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED));
    TRY(snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE));
    TRY(snd_pcm_hw_params_set_channels(pcm, hw, 1));
    TRY(snd_pcm_hw_params_set_rate(pcm, hw, RATE, 0));
    TRY(snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &latency, NULL));
    TRY(snd_pcm_hw_params(pcm, hw));
    TRY(snd_pcm_sw_params_current(pcm, sw));
    TRY(snd_pcm_sw_params_set_avail_min(pcm, sw, 1));
    TRY(snd_pcm_sw_params_set_start_threshold(pcm, sw, 1));
    TRY(snd_pcm_sw_params(pcm, sw));
    TRY(snd_pcm_prepare(pcm));
    if (capture) TRY(snd_pcm_start(pcm));
#undef TRY
    a->pcm = pcm;
    ++a->generation;
    a->mode = capture ? CAPTURE : PLAYBACK;
    return 0;
fail:
    snd_pcm_close(pcm);
    return err;
}

static void begin_recording(struct app *a)
{
    stop_playback(a);
    a->bytes = 0;
    a->path[0] = '\0';
    if (snprintf(a->path, sizeof a->path, "%s/recording-XXXXXX.wav", a->directory)
        >= (int)sizeof a->path) {
        fprintf(stderr, "Output directory path is too long.\n");
        a->failed = a->quit = true;
        return;
    }
    int fd = mkstemps(a->path, 4);
    if (fd < 0) { perror(a->path); a->failed = a->quit = true; return; }
    a->record = fdopen(fd, "w+b");
    if (!a->record) {
        perror("fdopen recording"); close(fd);
        a->failed = a->quit = true;
        return;
    }
    if (wav_header(a->record, 0)) {
        perror("Write WAV header"); a->failed = a->quit = true; return;
    }
    a->pending = true;
    a->attempts = 0;
    a->retry_at = now_ms();
    a->deadline = a->retry_at + (int64_t)a->seconds * 1000;
    printf("PTT down: recording %s\n", a->path);
}

static void try_capture(struct app *a)
{
    ++a->attempts;
    int err = open_pcm(a, true);
    if (!err) { a->pending = false; return; }
    if (err == -ENODEV) { removed(a); return; }
    /* EIO is deliberately not retried: the button may already be released. */
    if ((err == -EBUSY || err == -EAGAIN || err == -EINTR) && a->attempts < 4) {
        a->retry_at = now_ms() + 50;
        fprintf(stderr, "Capture startup: %s; retry %u/4 while held.\n",
                snd_strerror(err), a->attempts + 1);
        return;
    }
    fprintf(stderr, "Capture %s: %s; check PCM name/access and hold PTT. "
            "Waiting for release.\n", a->capture, snd_strerror(err));
    finish_recording(a);
}

static void begin_playback(struct app *a)
{
    if (!a->bytes || a->quit) { puts("Idle (no audio to play)."); return; }
    a->player = fopen(a->path, "rb");
    if (!a->player || fseek(a->player, 44, SEEK_SET)) {
        perror("Open recording for playback"); stop_playback(a); return;
    }
    int err = open_pcm(a, false);
    if (err < 0) {
        fprintf(stderr, "Playback %s: %s; check --playback PCM/access.\n",
                a->playback, snd_strerror(err));
        stop_playback(a);
        if (err == -ENODEV) removed(a);
        return;
    }
    printf("Playing %s (PTT interrupts playback)\n", a->path);
}

static void read_input(struct app *a)
{
    struct input_event events[32];
    for (;;) {
        ssize_t n = read(a->input, events, sizeof events);
        if (n < 0) {
            if (errno == EAGAIN) return;
            if (errno == EINTR) continue;
            if (errno == ENODEV) removed(a);
            else { perror("Read evdev"); a->failed = a->quit = true; }
            return;
        }
        if (!n) { removed(a); return; }
        if ((size_t)n % sizeof events[0]) {
            fprintf(stderr, "Incomplete evdev event; stopping safely.\n");
            a->failed = a->quit = true;
            return;
        }
        for (size_t i = 0; i < (size_t)n / sizeof events[0]; ++i) {
            struct input_event *e = &events[i];
            if (e->type == EV_SYN && e->code == SYN_DROPPED) {
                fprintf(stderr, "Evdev queue overflow: button state lost; stopping safely.\n");
                a->failed = a->quit = true;
                return;
            }
            if (e->type != EV_KEY || e->code != KEY_F2) continue;
            if (e->value == 1 && !a->held) {
                a->held = true;
                begin_recording(a);
            } else if (e->value == 0 && a->held) {
                a->held = false;
                puts("PTT up");
                finish_recording(a);
                begin_playback(a);
            }
            if (a->quit) return;
        }
    }
}

static void audio_error(struct app *a, int err)
{
    if (err == -EAGAIN || err == -EINTR) return;
    if (err == -ENODEV) { removed(a); return; }
    if (err == -EPIPE) {
        fprintf(stderr, "%s xrun; preparing PCM again.\n",
                a->mode == CAPTURE ? "Capture" : "Playback");
        err = snd_pcm_prepare(a->pcm);
        if (!err && a->mode == CAPTURE) err = snd_pcm_start(a->pcm);
        if (!err) return;
        if (err == -ENODEV) { removed(a); return; }
    }
    fprintf(stderr, "%s stopped: %s%s\n", a->mode == CAPTURE ? "Capture" : "Playback",
            snd_strerror(err), a->mode == CAPTURE ? "; waiting for PTT release." : ".");
    if (a->mode == CAPTURE) finish_recording(a);
    else stop_playback(a);
}

static void capture_step(struct app *a)
{
    uint32_t remaining = a->seconds * RATE - a->bytes / 2;
    snd_pcm_uframes_t want = remaining < CHUNK ? remaining : CHUNK;
    snd_pcm_sframes_t n = snd_pcm_readi(a->pcm, a->buffer, want);
    if (n < 0) { audio_error(a, (int)n); return; }
    if (n > 0) {
        size_t bytes = (size_t)n * 2;
        size_t written = fwrite(a->buffer, 1, bytes, a->record);
        a->bytes += (uint32_t)(written & ~(size_t)1);
        if (written != bytes) {
            perror("Write recording (check free disk space)");
            /* Remove an incomplete final sample, if any. */
            finish_recording(a);
            if (truncate(a->path, 44 + (off_t)a->bytes)) perror("Truncate recording");
            a->failed = a->quit = true;
            return;
        }
    }
    if (a->bytes / 2 == a->seconds * RATE) {
        puts("Recording limit reached; waiting for PTT release.");
        finish_recording(a);
    }
}

static void playback_step(struct app *a)
{
    if (a->draining) {
        int err = snd_pcm_drain(a->pcm);
        if (!err) { stop_playback(a); puts("Playback complete; idle."); }
        else if (err != -EAGAIN && err != -EINTR) audio_error(a, err);
        return;
    }
    if (a->offset == a->frames) {
        a->frames = fread(a->buffer, 2, CHUNK, a->player);
        a->offset = 0;
        if (!a->frames) {
            if (ferror(a->player)) {
                perror("Read playback file"); stop_playback(a); return;
            }
            a->draining = true;
            return;
        }
    }
    snd_pcm_sframes_t n = snd_pcm_writei(a->pcm, a->buffer + 2 * a->offset,
                                        a->frames - a->offset);
    if (n < 0) audio_error(a, (int)n);
    else a->offset += (size_t)n;
}

static void run(struct app *a)
{
    struct pollfd *fds = NULL;
    size_t capacity = 0;
    while (!a->quit) {
        int count = a->pcm && !a->draining ? snd_pcm_poll_descriptors_count(a->pcm) : 0;
        unsigned long generation = a->generation;
        if (count < 0) { audio_error(a, count); continue; }
        if ((size_t)count + 2 > capacity) {
            capacity = (size_t)count + 2;
            void *p = realloc(fds, capacity * sizeof *fds);
            if (!p) { perror("poll allocation"); a->failed = true; break; }
            fds = p;
        }
        fds[0] = (struct pollfd){.fd = a->input, .events = POLLIN};
        fds[1] = (struct pollfd){.fd = a->signals, .events = POLLIN};
        if (count) {
            int err = snd_pcm_poll_descriptors(a->pcm, fds + 2, (unsigned)count);
            if (err < 0) { audio_error(a, err); continue; }
        }
        /* A short heartbeat also covers plugin PCMs without poll descriptors,
         * nonblocking drain completion, retries and the wall-clock limit. */
        int ready = poll(fds, (nfds_t)count + 2, 20);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll"); a->failed = true; break;
        }
        if (fds[1].revents) { puts("Stopping on signal."); break; }
        if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) { removed(a); break; }
        /* Decode before input may close/reopen the PCM; only use these flags
         * if input has not changed the audio generation. Always drain input
         * first, including events arriving just after poll returned. */
        unsigned short audio_events = 0;
        int poll_error = 0;
        if (count)
            poll_error = snd_pcm_poll_descriptors_revents(a->pcm, fds + 2,
                                                         (unsigned)count, &audio_events);
        read_input(a);
        if (a->quit) break;
        int64_t now = now_ms();
        if (a->record && now >= a->deadline) {
            puts("Recording time limit reached; waiting for PTT release.");
            finish_recording(a);
        }
        if (a->held && a->pending && now >= a->retry_at) try_capture(a);
        if (a->quit) break;
        /* Discard readiness from a handle closed by an input event. A new
         * handle gets one nonblocking transfer to prime playback/capture. */
        bool changed = generation != a->generation;
        if (!changed && poll_error < 0) {
            audio_error(a, poll_error);
            continue;
        }
        if (!changed && (audio_events & (POLLHUP | POLLNVAL))) {
            removed(a);
            break;
        }
        if (changed || !count || (audio_events & (POLLIN | POLLOUT | POLLERR))) {
            if (a->mode == CAPTURE) capture_step(a);
            else if (a->mode == PLAYBACK) playback_step(a);
        }
    }
    free(fds);
}

static void usage(const char *name)
{
    printf("Usage: %s [options]\n"
           "Hold QX-18 PTT to record; release to play. Ctrl+C exits.\n"
           "  --input /dev/input/eventN  Select USB 7273:8378 KEY_F2 interface\n"
           "  --capture PCM             Default hw:CARD=UACDemoV10,DEV=0\n"
           "  --playback PCM            Default plughw:CARD=UACDemoV10,DEV=0\n"
           "  --output-dir DIR          Default ./recordings\n"
           "  --max-seconds N           Recording limit, 1..3600 (default 60)\n"
           "  --help                    Show this help\n"
           "Multiple units may need explicit ALSA card/PCM overrides.\n", name);
}

int main(int argc, char **argv)
{
    struct app a = {.capture = "hw:CARD=UACDemoV10,DEV=0",
                    .playback = "plughw:CARD=UACDemoV10,DEV=0",
                    .directory = "./recordings", .seconds = 60, .input = -1, .signals = -1};
    const char *selection = NULL;
    const struct option options[] = {
        {"input", required_argument, NULL, 'i'}, {"capture", required_argument, NULL, 'c'},
        {"playback", required_argument, NULL, 'p'}, {"output-dir", required_argument, NULL, 'o'},
        {"max-seconds", required_argument, NULL, 'm'}, {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (opt) {
        case 'i': selection = optarg; break;
        case 'c': a.capture = optarg; break;
        case 'p': a.playback = optarg; break;
        case 'o': a.directory = optarg; break;
        case 'm': {
            char *end;
            errno = 0;
            unsigned long n = strtoul(optarg, &end, 10);
            if (errno || !*optarg || *end || n < 1 || n > 3600) {
                fprintf(stderr, "--max-seconds must be 1..3600.\n"); return 1;
            }
            a.seconds = (unsigned)n;
            break;
        }
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }
    if (optind != argc) { usage(argv[0]); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, &oldmask)) { perror("sigprocmask"); return 1; }
    a.signals = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (a.signals < 0) { perror("signalfd"); return 1; }
    a.input = find_input(selection);
    if (a.input < 0) { close(a.signals); return 1; }
    if (ioctl(a.input, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "Cannot exclusively grab PTT input: %s. "
                "Close evtest/other grabbers and check access permissions.\n", strerror(errno));
        close(a.input); close(a.signals); return 1;
    }
    if (mkdir(a.directory, 0700) && errno != EEXIST) {
        perror(a.directory); a.failed = true;
    } else {
        unsigned long keys[KEY_MAX / BITS_PER_LONG + 1] = {0};
        if (ioctl(a.input, EVIOCGKEY(sizeof keys), keys) < 0) {
            perror("Read initial PTT state"); a.failed = true;
        } else {
            a.held = BIT_SET(keys, KEY_F2);
            puts(a.held ? "Idle: release PTT before the first recording." : "Idle: hold PTT to record.");
            run(&a);
        }
    }
    finish_recording(&a);
    stop_playback(&a);
    ioctl(a.input, EVIOCGRAB, 0);
    close(a.input);
    close(a.signals);
    /* Signals stay blocked until exit so a pending SIGINT cannot interrupt cleanup. */
    return a.failed ? 1 : 0;
}
