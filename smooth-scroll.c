/*
 * smooth-scroll.c — Smooth scroll daemon for Linux VMs (UTM/QEMU/SPICE)
 *
 * Intercepts coarse REL_WHEEL ±1 events from a VM's virtual pointer device,
 * applies a non-linear velocity curve (slow input ≈ 1:1, fast input heavily
 * dampened), and emits fine-grained REL_WHEEL_HI_RES events via uinput for
 * buttery-smooth scrolling.
 *
 * Build:  gcc -O3 -Wall -Wextra -Wpedantic -std=c99 -o smooth-scroll smooth-scroll.c -levdev -lm
 * Run:    sudo ./smooth-scroll            # auto-detect SPICE/QEMU device
 *         sudo ./smooth-scroll /dev/input/event5   # explicit device
 *
 * License: Apache License Version 2.0 — do what you want.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <getopt.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/ioctl.h>

#include <linux/input.h>
#include <linux/uinput.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

/* ── Defaults ─────────────────────────────────────────────────────────── */

#define DEFAULT_FRICTION 0.078     /* per-tick friction factor (250 Hz)    */
#define DEFAULT_TICK_MS 4          /* timer interval (250 Hz)              */
#define DEFAULT_LOW_RATE 5.0       /* events/sec: below = no dampening     */
#define DEFAULT_HIGH_RATE 30.0     /* events/sec: above = max dampening    */
#define DEFAULT_MIN_SCALE 0.3      /* scale factor at high input rate      */
#define DEFAULT_STOP_THRESHOLD 0.5 /* velocity below which scrolling stops */
#define DEFAULT_MULTIPLIER 0.5     /* global scroll distance multiplier    */

/* Hi-res scroll unit: one REL_WHEEL tick = 120 hi-res units (kernel ABI). */
#define HIRES_PER_TICK 120

/* Ring buffer for input-rate tracking: stores timestamps over a window. */
#define RATE_RING_SIZE 128
#define RATE_WINDOW_NS 300000000LL /* 300 ms */

/* ── Global state for signal handler ──────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ── Configuration ────────────────────────────────────────────────────── */

struct config
{
    double friction;         /* per-tick friction factor (0.01-0.2)  */
    int tick_ms;             /* timer interval in milliseconds       */
    double low_rate;         /* events/sec threshold: no dampening   */
    double high_rate;        /* events/sec threshold: max dampening  */
    double min_scale;        /* scale factor at >= high_rate         */
    double stop_threshold;   /* velocity below which scrolling stops */
    double multiplier;       /* global scroll distance multiplier    */
    int verbose;             /* debug printing                       */
    const char *device_path; /* NULL = auto-detect                  */
};

/* ── Input-rate ring buffer ───────────────────────────────────────────── */

struct rate_tracker
{
    int64_t timestamps[RATE_RING_SIZE]; /* nanosecond timestamps */
    int head;
    int count;
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void rate_record(struct rate_tracker *rt, int64_t ts)
{
    rt->timestamps[rt->head] = ts;
    rt->head = (rt->head + 1) % RATE_RING_SIZE;
    if (rt->count < RATE_RING_SIZE)
        rt->count++;
}

/*
 * Compute events-per-second from the ring buffer, considering only
 * events within the tracking window.
 */
static double rate_compute(const struct rate_tracker *rt, int64_t now)
{
    int64_t cutoff = now - RATE_WINDOW_NS;
    int n = 0;
    int64_t oldest = now;

    for (int i = 0; i < rt->count; i++)
    {
        int idx = (rt->head - 1 - i + RATE_RING_SIZE) % RATE_RING_SIZE;
        if (rt->timestamps[idx] >= cutoff)
        {
            n++;
            if (rt->timestamps[idx] < oldest)
                oldest = rt->timestamps[idx];
        }
    }

    if (n < 2)
        return 0.0;

    double window_sec = (double)(now - oldest) / 1e9;
    if (window_sec < 1e-6)
        return 0.0;

    return (double)n / window_sec;
}

/* ── Velocity state (one per axis) ────────────────────────────────────── */

struct axis_state
{
    double velocity;
    double emit_accum; /* sub-pixel accumulator for fractional hi-res units */
    int lowres_accum;  /* hi-res units accumulated towards next REL_WHEEL   */
    struct rate_tracker rate;
};

/* ── Non-linear dampening ─────────────────────────────────────────────── */

/*
 * Given the current input rate (events/sec), compute a scale factor in
 * [min_scale, 1.0].  Below low_rate → 1.0 (full responsiveness).
 * Above high_rate → min_scale (maximum dampening).  Between: sqrt interp.
 */
static double compute_scale(double input_rate, const struct config *cfg)
{
    if (input_rate <= cfg->low_rate)
        return 1.0;
    if (input_rate >= cfg->high_rate)
        return cfg->min_scale;

    double t = (input_rate - cfg->low_rate) / (cfg->high_rate - cfg->low_rate);
    return 1.0 - (1.0 - cfg->min_scale) * sqrt(t);
}

/* ── Case-insensitive substring search ────────────────────────────────── */

static int strcasestr_any(const char *haystack, const char *const needles[],
                          int n)
{
    for (int i = 0; i < n; i++)
    {
        if (strcasestr(haystack, needles[i]) != NULL)
            return 1;
    }
    return 0;
}

/* ── Device auto-detection ────────────────────────────────────────────── */

/*
 * Scan /dev/input/event* for a device whose name contains "spice", "qemu",
 * or "virtio" (case-insensitive) and that supports REL_WHEEL.
 * Returns a newly-allocated path string or NULL.
 */
static char *find_scroll_device(void)
{
    static const char *keywords[] = {"spice", "qemu", "virtio"};
    DIR *dir = opendir("/dev/input");
    if (!dir)
    {
        perror("opendir /dev/input");
        return NULL;
    }

    struct dirent *ent;
    char path[280];
    char name[256];

    while ((ent = readdir(dir)) != NULL)
    {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        /* Read device name. */
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
        {
            close(fd);
            continue;
        }

        /* Check name matches one of the VM keywords. */
        if (!strcasestr_any(name, keywords, 3))
        {
            close(fd);
            continue;
        }

        /* Check for REL_WHEEL capability. */
        unsigned long rel_bits[((REL_MAX + 1) + 8 * sizeof(unsigned long) - 1) /
                               (8 * sizeof(unsigned long))];
        memset(rel_bits, 0, sizeof(rel_bits));

        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) < 0)
        {
            close(fd);
            continue;
        }

        /* Test if REL_WHEEL bit is set. */
        int has_wheel = (rel_bits[REL_WHEEL / (8 * sizeof(unsigned long))] >>
                         (REL_WHEEL % (8 * sizeof(unsigned long)))) &
                        1;
        close(fd);

        if (has_wheel)
        {
            fprintf(stderr, "Auto-detected device: %s (%s)\n", path, name);
            closedir(dir);
            return strdup(path);
        }
    }

    closedir(dir);
    return NULL;
}

/* ── uinput device creation ───────────────────────────────────────────── */

/*
 * Create a uinput virtual device that mirrors all capabilities of the
 * source libevdev device, plus REL_WHEEL_HI_RES and REL_HWHEEL_HI_RES.
 * Returns the uinput fd (>= 0) or -1 on error.
 */
static int create_uinput_device(struct libevdev *source_dev)
{
    int uifd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uifd < 0)
    {
        perror("open /dev/uinput");
        return -1;
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));

    /* Name the virtual device "<original> (smooth scroll)". */
    const char *src_name = libevdev_get_name(source_dev);
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s (smooth scroll)",
             src_name ? src_name : "Unknown");
    setup.id.bustype = libevdev_get_id_bustype(source_dev);
    setup.id.vendor = libevdev_get_id_vendor(source_dev);
    setup.id.product = libevdev_get_id_product(source_dev);
    setup.id.version = libevdev_get_id_version(source_dev);

    /*
     * Mirror every event type and code from the source device.
     * We iterate all known event types and their codes.
     */
    for (unsigned int type = 0; type <= EV_MAX; type++)
    {
        if (!libevdev_has_event_type(source_dev, type))
            continue;

        if (ioctl(uifd, UI_SET_EVBIT, type) < 0)
        {
            fprintf(stderr, "UI_SET_EVBIT %u: %s\n", type, strerror(errno));
            /* non-fatal: some types may not be supported by uinput */
            continue;
        }

        int max_code = libevdev_event_type_get_max(type);
        if (max_code < 0)
            continue;

        for (int code = 0; code <= max_code; code++)
        {
            if (!libevdev_has_event_code(source_dev, type, (unsigned int)code))
                continue;

            unsigned long req = 0;
            switch (type)
            {
            case EV_KEY:
                req = UI_SET_KEYBIT;
                break;
            case EV_REL:
                req = UI_SET_RELBIT;
                break;
            case EV_ABS:
                req = UI_SET_ABSBIT;
                break;
            case EV_MSC:
                req = UI_SET_MSCBIT;
                break;
            case EV_LED:
                req = UI_SET_LEDBIT;
                break;
            case EV_SND:
                req = UI_SET_SNDBIT;
                break;
            case EV_FF:
                req = UI_SET_FFBIT;
                break;
            case EV_SW:
                req = UI_SET_SWBIT;
                break;
            default:
                continue;
            }

            if (ioctl(uifd, req, code) < 0)
            {
                /* Silently skip unsupported codes. */
            }
        }
    }

    /*
     * Ensure hi-res scroll axes are present even if the source lacks them.
     * These are critical for smooth output.
     */
    if (!libevdev_has_event_type(source_dev, EV_REL))
        ioctl(uifd, UI_SET_EVBIT, EV_REL);

    ioctl(uifd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
    ioctl(uifd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);

    /* Configure EV_ABS axes with proper absinfo (range, fuzz, etc.). */
    if (libevdev_has_event_type(source_dev, EV_ABS))
    {
        int abs_max = libevdev_event_type_get_max(EV_ABS);
        for (int code = 0; code <= abs_max; code++)
        {
            if (!libevdev_has_event_code(source_dev, EV_ABS, (unsigned int)code))
                continue;

            const struct input_absinfo *ai =
                libevdev_get_abs_info(source_dev, (unsigned int)code);
            if (!ai)
                continue;

            struct uinput_abs_setup abs_setup;
            memset(&abs_setup, 0, sizeof(abs_setup));
            abs_setup.code = (unsigned short)code;
            abs_setup.absinfo = *ai;

            if (ioctl(uifd, UI_ABS_SETUP, &abs_setup) < 0)
            {
                fprintf(stderr, "UI_ABS_SETUP %d: %s\n", code,
                        strerror(errno));
            }
        }
    }

    /* Finalize the device. */
    if (ioctl(uifd, UI_DEV_SETUP, &setup) < 0)
    {
        perror("UI_DEV_SETUP");
        close(uifd);
        return -1;
    }

    if (ioctl(uifd, UI_DEV_CREATE) < 0)
    {
        perror("UI_DEV_CREATE");
        close(uifd);
        return -1;
    }

    fprintf(stderr, "Created virtual device: %s\n", setup.name);
    return uifd;
}

/* ── Event helpers ────────────────────────────────────────────────────── */

static int write_event(int uifd, unsigned short type, unsigned short code,
                       int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;

    ssize_t n = write(uifd, &ev, sizeof(ev));
    if (n < 0)
    {
        perror("write uinput event");
        return -1;
    }
    return 0;
}

static int write_syn(int uifd)
{
    return write_event(uifd, EV_SYN, SYN_REPORT, 0);
}

static int is_scroll_code(unsigned short code)
{
    return code == REL_WHEEL || code == REL_HWHEEL ||
           code == REL_WHEEL_HI_RES || code == REL_HWHEEL_HI_RES;
}

/*
 * Perform one emission step for a single axis: apply friction-based
 * exponential decay, accumulate into sub-pixel remainder, and emit
 * the integer part as a hi-res scroll event.
 *
 * Also emits the corresponding low-res event (REL_WHEEL / REL_HWHEEL)
 * every time the hi-res accumulator crosses a 120-unit boundary.  This
 * is the standard Linux kernel convention: devices that send
 * REL_WHEEL_HI_RES must also send REL_WHEEL for compatibility with
 * applications that only handle the low-res variant.
 *
 * Returns 1 if any event was written, 0 otherwise.
 */
static int emit_axis(int uifd, struct axis_state *as, unsigned short hires_code,
                     const struct config *cfg, const char *label)
{
    if (fabs(as->velocity) < cfg->stop_threshold)
    {
        as->velocity = 0.0;
        as->emit_accum = 0.0;
        as->lowres_accum = 0;
        return 0;
    }

    /* Exponential decay: friction removes a fraction each tick. */
    double old_vel = as->velocity;
    as->velocity *= (1.0 - cfg->friction);
    double emit = old_vel - as->velocity;

    /*
     * Sub-pixel accumulation: accumulate the fractional hi-res
     * units and only emit the integer part.  This prevents
     * uneven step sizes that appear as micro-stutter.
     */
    as->emit_accum += emit;
    int emit_int = (int)as->emit_accum;
    as->emit_accum -= (double)emit_int;

    if (emit_int != 0)
    {
        write_event(uifd, EV_REL, hires_code, emit_int);

        /*
         * Low-res compatibility: accumulate hi-res units and emit
         * REL_WHEEL/REL_HWHEEL every 120 units.  Many applications
         * (Firefox, Electron, older X11 toolkits) only handle the
         * low-res event codes.
         */
        unsigned short lowres_code =
            (hires_code == REL_WHEEL_HI_RES) ? REL_WHEEL : REL_HWHEEL;
        as->lowres_accum += emit_int;
        while (as->lowres_accum >= HIRES_PER_TICK)
        {
            write_event(uifd, EV_REL, lowres_code, 1);
            as->lowres_accum -= HIRES_PER_TICK;
        }
        while (as->lowres_accum <= -HIRES_PER_TICK)
        {
            write_event(uifd, EV_REL, lowres_code, -1);
            as->lowres_accum += HIRES_PER_TICK;
        }

        if (cfg->verbose)
        {
            fprintf(stderr,
                    "[emit] %s hires=%d vel=%.1f accum=%.3f "
                    "lowres_accum=%d\n",
                    label, emit_int, as->velocity, as->emit_accum,
                    as->lowres_accum);
        }
        return 1;
    }
    return 0;
}

/* ── Usage ────────────────────────────────────────────────────────────── */

static void print_usage(const char *progname)
{
    fprintf(stderr,
            "Usage: %s [OPTIONS] [DEVICE_PATH]\n\n"
            "Smooth scroll daemon for Linux VMs (SPICE/QEMU/VirtIO).\n\n"
            "Options:\n"
            "  -f, --friction FLOAT       Per-tick friction factor, 0.01-0.2 (default: %.2f)\n"
            "                             Lower = longer glide after release, higher = stops faster.\n"
            "                             macOS feel is around 0.02-0.04.\n"
            "  -t, --tick-ms INT          Timer tick interval in ms (default: %d)\n"
            "      --low-rate FLOAT       Input rate (events/sec) below which no dampening\n"
            "                             is applied — full responsiveness (default: %.1f)\n"
            "      --high-rate FLOAT      Input rate (events/sec) above which maximum\n"
            "                             dampening is applied (default: %.1f)\n"
            "      --min-scale FLOAT      Scale factor at high input rate (default: %.2f)\n"
            "      --stop-threshold FLOAT Velocity below which scrolling stops (default: %.1f)\n"
            "  -m, --multiplier FLOAT     Global scroll distance multiplier (default: %.1f)\n"
            "                             Lower = less scroll per gesture. 0.3 for fine control,\n"
            "                             1.0 for full 1:1 passthrough.\n"
            "  -v, --verbose              Print debug info about intercepted/emitted events\n"
            "  -h, --help                 Show this help\n",
            progname, DEFAULT_FRICTION, DEFAULT_TICK_MS,
            DEFAULT_LOW_RATE, DEFAULT_HIGH_RATE, DEFAULT_MIN_SCALE,
            DEFAULT_STOP_THRESHOLD, DEFAULT_MULTIPLIER);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    struct config cfg = {
        .friction = DEFAULT_FRICTION,
        .tick_ms = DEFAULT_TICK_MS,
        .low_rate = DEFAULT_LOW_RATE,
        .high_rate = DEFAULT_HIGH_RATE,
        .min_scale = DEFAULT_MIN_SCALE,
        .stop_threshold = DEFAULT_STOP_THRESHOLD,
        .multiplier = DEFAULT_MULTIPLIER,
        .verbose = 0,
        .device_path = NULL,
    };

    /* ── Parse command-line arguments ──────────────────────────────── */

    static struct option long_opts[] = {
        {"friction", required_argument, NULL, 'f'},
        {"tick-ms", required_argument, NULL, 't'},
        {"low-rate", required_argument, NULL, 'L'},
        {"high-rate", required_argument, NULL, 'H'},
        {"min-scale", required_argument, NULL, 'S'},
        {"stop-threshold", required_argument, NULL, 'T'},
        {"multiplier", required_argument, NULL, 'm'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "f:t:m:vh", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
        case 'f':
            cfg.friction = atof(optarg);
            break;
        case 't':
            cfg.tick_ms = atoi(optarg);
            break;
        case 'L':
            cfg.low_rate = atof(optarg);
            break;
        case 'H':
            cfg.high_rate = atof(optarg);
            break;
        case 'S':
            cfg.min_scale = atof(optarg);
            break;
        case 'T':
            cfg.stop_threshold = atof(optarg);
            break;
        case 'm':
            cfg.multiplier = atof(optarg);
            break;
        case 'v':
            cfg.verbose = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Non-option argument is the device path. */
    if (optind < argc)
        cfg.device_path = argv[optind];

    /* Clamp configuration to sane ranges. */
    if (cfg.friction < 0.01)
        cfg.friction = 0.01;
    if (cfg.friction > 0.2)
        cfg.friction = 0.2;
    if (cfg.tick_ms < 1)
        cfg.tick_ms = 1;
    if (cfg.tick_ms > 50)
        cfg.tick_ms = 50;
    if (cfg.multiplier < 0.01)
        cfg.multiplier = 0.01;
    if (cfg.multiplier > 10.0)
        cfg.multiplier = 10.0;

    /* ── Install signal handlers ──────────────────────────────────── */

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* ── Open source device ───────────────────────────────────────── */

    char *auto_path = NULL;
    const char *dev_path = cfg.device_path;

    if (!dev_path)
    {
        auto_path = find_scroll_device();
        if (!auto_path)
        {
            fprintf(stderr,
                    "Error: No SPICE/QEMU/VirtIO scroll device found.\n"
                    "Provide a device path: %s /dev/input/eventN\n"
                    "List devices with: cat /proc/bus/input/devices\n",
                    argv[0]);
            return 1;
        }
        dev_path = auto_path;
    }

    int src_fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (src_fd < 0)
    {
        fprintf(stderr, "open %s: %s\n", dev_path, strerror(errno));
        free(auto_path);
        return 1;
    }

    /* Initialize libevdev from the source device fd. */
    struct libevdev *evdev = NULL;
    int rc = libevdev_new_from_fd(src_fd, &evdev);
    if (rc < 0)
    {
        fprintf(stderr, "libevdev_new_from_fd: %s\n", strerror(-rc));
        close(src_fd);
        free(auto_path);
        return 1;
    }

    fprintf(stderr, "Source device: %s (%s)\n",
            dev_path, libevdev_get_name(evdev));

    /* ── Create uinput virtual device ─────────────────────────────── */

    int uifd = create_uinput_device(evdev);
    if (uifd < 0)
    {
        libevdev_free(evdev);
        close(src_fd);
        free(auto_path);
        return 1;
    }

    /*
     * Small delay to let udev/libinput recognize the new virtual device
     * before we grab the source and it goes silent.
     */
    usleep(200000);

    /* ── Grab source device ───────────────────────────────────────── */

    if (ioctl(src_fd, EVIOCGRAB, 1) < 0)
    {
        perror("EVIOCGRAB");
        ioctl(uifd, UI_DEV_DESTROY);
        close(uifd);
        libevdev_free(evdev);
        close(src_fd);
        free(auto_path);
        return 1;
    }

    fprintf(stderr, "Grabbed source device. Scroll smoothing active.\n");

    /* ── Create timer fd ──────────────────────────────────────────── */

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd < 0)
    {
        perror("timerfd_create");
        goto cleanup;
    }

    /*
     * Use TFD_TIMER_ABSTIME with absolute scheduling to prevent timer drift.
     * Each tick is scheduled as an absolute time (previous + interval) rather
     * than relative, ensuring precise 125 Hz emission without drift accumulation.
     */
    long tick_ns = cfg.tick_ms * 1000000L;
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);

    /* next_tick tracks the absolute time of the next scheduled tick. */
    int64_t next_tick_ns = (int64_t)ts_now.tv_sec * 1000000000LL +
                           ts_now.tv_nsec + tick_ns;

    {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = (time_t)(next_tick_ns / 1000000000LL);
        its.it_value.tv_nsec = (long)(next_tick_ns % 1000000000LL);
        /* No interval — we reschedule each tick as an absolute time. */
        its.it_interval.tv_sec = 0;
        its.it_interval.tv_nsec = 0;
        if (timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL) < 0)
        {
            perror("timerfd_settime");
            close(tfd);
            goto cleanup;
        }
    }

    /* ── Set up epoll ─────────────────────────────────────────────── */

    int epfd = epoll_create1(0);
    if (epfd < 0)
    {
        perror("epoll_create1");
        close(tfd);
        goto cleanup;
    }

    {
        struct epoll_event ev;

        ev.events = EPOLLIN;
        ev.data.fd = src_fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, src_fd, &ev) < 0)
        {
            perror("epoll_ctl src_fd");
            close(epfd);
            close(tfd);
            goto cleanup;
        }

        ev.events = EPOLLIN;
        ev.data.fd = tfd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev) < 0)
        {
            perror("epoll_ctl tfd");
            close(epfd);
            close(tfd);
            goto cleanup;
        }
    }

    /* ── Scroll state ─────────────────────────────────────────────── */

    struct axis_state vert = {0};
    struct axis_state horiz = {0};

    /*
     * Track whether we forwarded any non-scroll events in the current
     * frame.  We suppress SYN_REPORT after scroll-only frames so we
     * don't emit empty syncs.
     */
    int had_non_scroll = 0;

    /* ── Main event loop ──────────────────────────────────────────── */

    struct epoll_event events[2];

    while (g_running)
    {
        int nfds = epoll_wait(epfd, events, 2, -1);
        if (nfds < 0)
        {
            if (errno == EINTR)
                continue; /* interrupted by signal — recheck g_running */
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;

            /* ── Source device readable ────────────────────────── */
            if (fd == src_fd)
            {
                struct input_event ev;
                while (1)
                {
                    ssize_t n = read(src_fd, &ev, sizeof(ev));
                    if (n < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        /* Device gone (hot-unplug). */
                        fprintf(stderr,
                                "Source device read error: %s\n",
                                strerror(errno));
                        g_running = 0;
                        break;
                    }
                    if (n == 0)
                    {
                        fprintf(stderr, "Source device EOF.\n");
                        g_running = 0;
                        break;
                    }
                    if (n != sizeof(ev))
                        continue;

                    /* SYN_REPORT: flush only if we forwarded non-scroll. */
                    if (ev.type == EV_SYN && ev.code == SYN_REPORT)
                    {
                        if (had_non_scroll)
                        {
                            write_syn(uifd);
                        }
                        had_non_scroll = 0;
                        continue;
                    }

                    /* Intercept scroll events. */
                    if (ev.type == EV_REL && is_scroll_code(ev.code))
                    {
                        int64_t ts = now_ns();
                        double raw = 0.0;
                        struct axis_state *axis = NULL;

                        switch (ev.code)
                        {
                        case REL_WHEEL:
                            raw = (double)ev.value * HIRES_PER_TICK;
                            axis = &vert;
                            break;
                        case REL_HWHEEL:
                            raw = (double)ev.value * HIRES_PER_TICK;
                            axis = &horiz;
                            break;
                        case REL_WHEEL_HI_RES:
                            raw = (double)ev.value;
                            axis = &vert;
                            break;
                        case REL_HWHEEL_HI_RES:
                            raw = (double)ev.value;
                            axis = &horiz;
                            break;
                        }

                        if (axis)
                        {
                            rate_record(&axis->rate, ts);
                            double rate = rate_compute(&axis->rate, ts);
                            double scale = compute_scale(rate, &cfg);
                            axis->velocity += raw * scale * cfg.multiplier;

                            if (cfg.verbose)
                            {
                                fprintf(stderr,
                                        "[in] code=%u val=%d raw=%.0f "
                                        "rate=%.1f/s scale=%.3f vel=%.1f\n",
                                        ev.code, ev.value, raw,
                                        rate, scale, axis->velocity);
                            }

                            /*
                             * Emit immediately on new input for sharp
                             * initial response.  Without this, the first
                             * scroll impulse waits up to one tick interval
                             * before anything appears on screen, making the
                             * start of a scroll feel soft/laggy compared to
                             * native macOS.  The timer continues handling
                             * the deceleration coast.
                             *
                             * If emit_axis returns 0 (friction extract < 1
                             * hi-res unit), force-emit ±1 so that every
                             * scroll input — no matter how small — produces
                             * immediate visible feedback.  This is critical
                             * for very slow, precise trackpad scrolling
                             * where the host sends tiny scroll deltas.
                             */
                            unsigned short hc =
                                (axis == &vert) ? REL_WHEEL_HI_RES
                                                : REL_HWHEEL_HI_RES;
                            const char *lbl =
                                (axis == &vert) ? "vert" : "horiz";
                            int did_emit =
                                emit_axis(uifd, axis, hc, &cfg, lbl);

                            /*
                             * If emit_axis produced nothing (friction
                             * extract < 1 hi-res unit), force-emit ±1 so
                             * every scroll input — no matter how small —
                             * produces immediate visible feedback.  Critical
                             * for very slow, precise trackpad scrolling.
                             */
                            if (!did_emit &&
                                fabs(axis->velocity) >= cfg.stop_threshold)
                            {
                                int dir =
                                    (axis->velocity > 0) ? 1 : -1;
                                write_event(uifd, EV_REL, hc, dir);
                                axis->lowres_accum += dir;
                                axis->velocity -= (double)dir;
                                axis->emit_accum = 0.0;
                                did_emit = 1;

                                if (cfg.verbose)
                                {
                                    fprintf(stderr,
                                            "[emit] %s hires=%d (min) "
                                            "vel=%.1f\n",
                                            lbl, dir, axis->velocity);
                                }
                            }

                            if (did_emit)
                                write_syn(uifd);
                        }

                        continue;
                    }

                    /* Forward all other events immediately. */
                    write_event(uifd, ev.type, ev.code, ev.value);
                    had_non_scroll = 1;
                }
            }

            /* ── Timer tick: emit smooth scroll ───────────────── */
            if (fd == tfd)
            {
                uint64_t expirations;
                ssize_t n = read(tfd, &expirations, sizeof(expirations));
                if (n < 0 && errno != EAGAIN)
                {
                    perror("read timerfd");
                    continue;
                }

                /* Reschedule the next tick as an absolute time. */
                next_tick_ns += tick_ns;
                {
                    struct itimerspec its;
                    memset(&its, 0, sizeof(its));
                    its.it_value.tv_sec =
                        (time_t)(next_tick_ns / 1000000000LL);
                    its.it_value.tv_nsec =
                        (long)(next_tick_ns % 1000000000LL);
                    timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);
                }

                int emitted = 0;
                emitted |= emit_axis(uifd, &vert, REL_WHEEL_HI_RES,
                                     &cfg, "vert");
                emitted |= emit_axis(uifd, &horiz, REL_HWHEEL_HI_RES,
                                     &cfg, "horiz");
                if (emitted)
                    write_syn(uifd);
            }
        }
    }

    /* ── Cleanup ──────────────────────────────────────────────────── */

    fprintf(stderr, "\nShutting down...\n");

    close(epfd);
    close(tfd);

cleanup:
    /* Ungrab source device so it becomes usable again. */
    ioctl(src_fd, EVIOCGRAB, 0);

    /* Destroy the virtual device. */
    ioctl(uifd, UI_DEV_DESTROY);
    close(uifd);

    libevdev_free(evdev);
    close(src_fd);
    free(auto_path);

    fprintf(stderr, "Cleanup complete.\n");
    return 0;
}
