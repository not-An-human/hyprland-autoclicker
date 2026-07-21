#define _POSIX_C_SOURCE 200809L

/*
 * hypr-autoclicker — native Wayland/Hyprland autoclicker via uinput
 *
 * Requires: /dev/uinput write access (run as root, or add yourself to the
 * 'input' group and add a udev rule — see README or -h).
 *
 * Build:  gcc -O2 -o autoclicker autoclicker.c
 * Usage:  sudo ./autoclicker [OPTIONS]
 *
 * Options:
 *   -i <ms>        Click interval in milliseconds (default: 100)
 *   -b <button>    Button: left | right | middle (default: left)
 *   -d <ms>        Hold duration per click in milliseconds (default: 10, <= interval)
 *   -c <count>     Stop after N clicks (default: 0 = infinite)
 *   -t <seconds>   Stop after T seconds (default: 0 = infinite)
 *   -p <seconds>   Capture the cursor after a countdown and lock clicks there
 *   -P <X,Y>       Lock clicks to the specified global-layout position
 *   -g             Print the current cursor position and exit
 *   -h             Print this help
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ERRBUF_SIZE 128
#define MAX_EINTR_RETRIES 100

/* ── globals ──────────────────────────────────────────────────────────────── */
static volatile sig_atomic_t running = 1;
static int uinput_fd = -1;
static int uinput_created = 0;

/* ── helpers ──────────────────────────────────────────────────────────────── */
static const char *errno_message(int errnum, char *buf, size_t size) {
    if (size == 0)
        return "unknown error";

    if (strerror_r(errnum, buf, size) == 0)
        return buf;

    snprintf(buf, size, "errno %d", errnum);
    return buf;
}

static int retry_ioctl0(int fd, unsigned long request) {
    int retries = 0;
    int rc;

    for (;;) {
        rc = ioctl(fd, request);
        if (rc >= 0 || errno != EINTR)
            return rc;

        retries++;
        if (retries >= MAX_EINTR_RETRIES)
            return -1;
    }
}

static int retry_ioctl_ulong(int fd, unsigned long request, unsigned long arg) {
    int retries = 0;
    int rc;

    for (;;) {
        rc = ioctl(fd, request, arg);
        if (rc >= 0 || errno != EINTR)
            return rc;

        retries++;
        if (retries >= MAX_EINTR_RETRIES)
            return -1;
    }
}

static int retry_ioctl_ptr(int fd, unsigned long request, void *arg) {
    int retries = 0;
    int rc;

    for (;;) {
        rc = ioctl(fd, request, arg);
        if (rc >= 0 || errno != EINTR)
            return rc;

        retries++;
        if (retries >= MAX_EINTR_RETRIES)
            return -1;
    }
}

static void cleanup_uinput(void) {
    if (uinput_fd < 0)
        return;

    if (uinput_created) {
        retry_ioctl0(uinput_fd, UI_DEV_DESTROY);
        uinput_created = 0;
    }

    close(uinput_fd);
    uinput_fd = -1;
}

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    cleanup_uinput();
    exit(EXIT_FAILURE);
}

static void die_errno(const char *message, int errnum) {
    char errbuf[ERRBUF_SIZE];

    die("%s: %s", message, errno_message(errnum, errbuf, sizeof(errbuf)));
}

static void die_open_uinput(int errnum) {
    char errbuf[ERRBUF_SIZE];

    die("Cannot open /dev/uinput: %s\n"
        "  → run as root, or add yourself to the 'input' group and\n"
        "    create /etc/udev/rules.d/99-uinput.rules containing:\n"
        "      KERNEL==\"uinput\", GROUP=\"input\", MODE=\"0660\"",
        errno_message(errnum, errbuf, sizeof(errbuf)));
}

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static long parse_long(const char *value, const char *name) {
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0')
        die("Invalid %s: '%s'", name, value);

    return parsed;
}

static int long_fits_time_t(long value) {
    time_t converted = (time_t)value;

    return (long)converted == value;
}

static void parse_position(const char *value, long *x, long *y) {
    char *end = NULL;

    errno = 0;
    *x = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != ',')
        die("Invalid position '%s' (expected X,Y)", value);

    value = end + 1;
    while (*value == ' ' || *value == '\t')
        value++;

    errno = 0;
    *y = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0')
        die("Invalid position '%s' (expected X,Y)", value);
}

/* Read Hyprland's global-layout coordinates from its command-line client. */
static int get_cursor_position(long *x, long *y) {
    char line[128];
    FILE *pipe;
    int status;
    char *newline;
    char *end = NULL;
    char *y_text;

    pipe = popen("hyprctl cursorpos", "r");
    if (pipe == NULL) {
        fprintf(stderr, "Cannot run hyprctl cursorpos: %s\n", strerror(errno));
        return -1;
    }

    if (fgets(line, sizeof(line), pipe) == NULL) {
        status = pclose(pipe);
        if (running)
            fprintf(stderr, "hyprctl cursorpos returned no position (status %d)\n", status);
        return -1;
    }

    status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (running)
            fprintf(stderr, "hyprctl cursorpos failed\n");
        return -1;
    }

    newline = strchr(line, '\n');
    if (newline != NULL)
        *newline = '\0';

    errno = 0;
    *x = strtol(line, &end, 10);
    if (errno != 0 || end == line || *end != ',') {
        fprintf(stderr, "Unexpected hyprctl cursorpos output: '%s'\n", line);
        return -1;
    }

    y_text = end + 1;
    while (*y_text == ' ' || *y_text == '\t')
        y_text++;
    errno = 0;
    *y = strtol(y_text, &end, 10);
    if (errno != 0 || end == y_text || (*end != '\0' && *end != '\r')) {
        fprintf(stderr, "Unexpected hyprctl cursorpos output: '%s'\n", line);
        return -1;
    }

    return 0;
}

/* Hyprland accepts its cursorpos global-layout coordinates directly. */
static int move_cursor_to(long x, long y) {
    char x_text[32];
    char y_text[32];
    pid_t pid;
    int status;

    snprintf(x_text, sizeof(x_text), "%ld", x);
    snprintf(y_text, sizeof(y_text), "%ld", y);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork for hyprctl failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execlp("hyprctl", "hyprctl", "--quiet", "dispatch", "movecursor",
               x_text, y_text, (char *)NULL);
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            if (!running) {
                /* The child shares Ctrl-C, but also terminate it if needed. */
                kill(pid, SIGTERM);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                    ;
                return -1;
            }
            continue;
        }
        fprintf(stderr, "waitpid for hyprctl failed: %s\n", strerror(errno));
        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (running)
            fprintf(stderr, "hyprctl could not move the cursor to %ld,%ld\n", x, y);
        return -1;
    }

    return 0;
}

/* sleep for `ms` milliseconds unless a termination signal arrives */
static void sleep_ms(long ms) {
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };

    while (running && nanosleep(&ts, &ts) < 0) {
        if (errno != EINTR)
            die_errno("nanosleep failed", errno);
    }
}

static struct timespec monotonic_now(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        die_errno("clock_gettime failed", errno);

    return ts;
}

static int elapsed_at_least(const struct timespec *start, long seconds) {
    struct timespec now = monotonic_now();
    time_t elapsed_sec = now.tv_sec - start->tv_sec;
    long elapsed_nsec = now.tv_nsec - start->tv_nsec;
    time_t limit_sec = (time_t)seconds;

    if (elapsed_nsec < 0) {
        elapsed_sec--;
        elapsed_nsec += 1000000000L;
    }

    if (elapsed_sec < 0)
        return 0;

    if (elapsed_sec > limit_sec)
        return 1;
    if (elapsed_sec < limit_sec)
        return 0;

    return 1;
}

/* emit a single input_event to the uinput fd */
static void emit(int fd, unsigned short type, unsigned short code, int value) {
    /* Timestamp fields are intentionally zeroed; uinput timestamps events. */
    struct input_event ev = {0};
    const char *buf = (const char *)&ev;
    size_t written = 0;

    ev.type  = type;
    ev.code  = code;
    ev.value = value;

    while (written < sizeof(ev)) {
        ssize_t n = write(fd, buf + written, sizeof(ev) - written);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            die_errno("write to uinput failed", errno);
        }

        if (n == 0)
            die("write to uinput failed: wrote 0 bytes");

        written += (size_t)n;
    }
}

/* press + release one mouse button */
static void click(int fd, unsigned short btn_code, long hold_ms) {
    emit(fd, EV_KEY, btn_code, 1);          /* press   */
    emit(fd, EV_SYN, SYN_REPORT, 0);
    sleep_ms(hold_ms);
    /* Always release after an interrupted hold to avoid a stuck button. */
    emit(fd, EV_KEY, btn_code, 0);          /* release */
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int open_uinput(void) {
    int retries = 0;
    int fd;

    for (;;) {
        fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0 || errno != EINTR)
            return fd;

        retries++;
        if (retries >= MAX_EINTR_RETRIES)
            return -1;
    }
}

/* ── uinput device setup ──────────────────────────────────────────────────── */
static void setup_uinput(void) {
    int name_len;
    int fd = open_uinput();

    if (fd < 0)
        die_open_uinput(errno);

    uinput_fd = fd;

    /* enable button events */
    if (retry_ioctl_ulong(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        retry_ioctl_ulong(fd, UI_SET_KEYBIT, BTN_LEFT)   < 0 ||
        retry_ioctl_ulong(fd, UI_SET_KEYBIT, BTN_RIGHT)  < 0 ||
        retry_ioctl_ulong(fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0)
        die_errno("ioctl UI_SET_EVBIT/KEYBIT failed", errno);

    /* enable sync events */
    if (retry_ioctl_ulong(fd, UI_SET_EVBIT, EV_SYN) < 0)
        die_errno("ioctl UI_SET_EVBIT EV_SYN failed", errno);

    struct uinput_setup usetup = {0};
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0x1234;
    usetup.id.product = 0x5678;
    name_len = snprintf(usetup.name, sizeof(usetup.name), "%s", "hypr-autoclicker");
    if (name_len < 0 || (size_t)name_len >= sizeof(usetup.name))
        die("uinput device name is too long");

    if (retry_ioctl_ptr(fd, UI_DEV_SETUP, &usetup) < 0)
        die_errno("UI_DEV_SETUP failed", errno);

    if (retry_ioctl0(fd, UI_DEV_CREATE) < 0)
        die_errno("UI_DEV_CREATE failed", errno);

    uinput_created = 1;

    /* give the kernel a moment to register the device */
    sleep_ms(100);
}

/* ── usage ────────────────────────────────────────────────────────────────── */
static void usage(const char *prog, int status) {
    FILE *stream = status == EXIT_SUCCESS ? stdout : stderr;

    fprintf(stream,
        "hypr-autoclicker — native Wayland/Hyprland autoclicker\n\n"
        "Usage: sudo %s [OPTIONS]\n\n"
        "Options:\n"
        "  -i <ms>       Interval between clicks in ms     (default: 100)\n"
        "  -b <button>   Button: left | right | middle     (default: left)\n"
        "  -d <ms>       Hold duration per click in ms     (default: 10, <= interval)\n"
        "  -c <count>    Stop after N clicks  (0 = infinite, default: 0)\n"
        "  -t <seconds>  Stop after T seconds (0 = infinite, default: 0)\n"
        "  -p <seconds>  Capture cursor after a countdown, then lock target\n"
        "  -P <X,Y>      Lock clicks to a global-layout position\n"
        "  -g            Print current cursor position and exit\n"
        "  -h            Show this help\n\n"
        "Fixed-position modes require hyprctl.\n"
        "Press Ctrl-C at any time to cancel.\n\n"
        "Keyboard shortcut to stop: Ctrl-C\n\n"
        "Permissions:\n"
        "  Either run as root, or:\n"
        "  1. Add yourself to the 'input' group:\n"
        "       sudo usermod -aG input $USER   (re-login after)\n"
        "  2. Create /etc/udev/rules.d/99-uinput.rules:\n"
        "       KERNEL==\"uinput\", GROUP=\"input\", MODE=\"0660\"\n"
        "  3. Reload rules:  sudo udevadm trigger\n",
        prog);
    exit(status);
}

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    long interval_ms = 100;
    long hold_ms     = 10;
    long max_clicks  = 0;   /* 0 = unlimited */
    long max_secs    = 0;   /* 0 = unlimited */
    long capture_secs = 0;
    long target_x = 0;
    long target_y = 0;
    int fixed_position = 0;
    int capture_requested = 0;
    int show_cursor_position = 0;
    unsigned short btn = BTN_LEFT;

    struct sigaction sa = {0};
    static const struct option long_options[] = {
        {"capture", required_argument, NULL, 'p'},
        {"position", required_argument, NULL, 'P'},
        {"cursorpos", no_argument, NULL, 'g'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int opt;

    if (atexit(cleanup_uinput) != 0)
        die("atexit cleanup registration failed");

    while ((opt = getopt_long(argc, argv, "i:b:d:c:t:p:P:gh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'i': interval_ms = parse_long(optarg, "interval"); break;
        case 'd': hold_ms     = parse_long(optarg, "hold duration"); break;
        case 'c': max_clicks  = parse_long(optarg, "click count"); break;
        case 't': max_secs    = parse_long(optarg, "duration"); break;
        case 'p': capture_secs = parse_long(optarg, "capture countdown"); capture_requested = 1; break;
        case 'P': parse_position(optarg, &target_x, &target_y); fixed_position = 1; break;
        case 'g': show_cursor_position = 1; break;
        case 'b':
            if      (strcmp(optarg, "left")   == 0) btn = BTN_LEFT;
            else if (strcmp(optarg, "right")  == 0) btn = BTN_RIGHT;
            else if (strcmp(optarg, "middle") == 0) btn = BTN_MIDDLE;
            else { fprintf(stderr, "Unknown button '%s'\n", optarg); usage(argv[0], EXIT_FAILURE); }
            break;
        case 'h': usage(argv[0], EXIT_SUCCESS); break;
        default:  usage(argv[0], EXIT_FAILURE); break;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Unexpected argument: '%s'\n", argv[optind]);
        usage(argv[0], EXIT_FAILURE);
    }

    if (interval_ms <= 0) die("Interval must be > 0 ms");
    if (hold_ms < 0)      die("Hold duration must be >= 0 ms");
    if (hold_ms > interval_ms) die("Hold duration must be <= interval");
    if (max_clicks < 0)   die("Click count must be >= 0");
    if (max_secs < 0)     die("Duration must be >= 0 seconds");
    if (capture_requested && capture_secs <= 0)
        die("Capture countdown must be > 0 seconds");
    if (!long_fits_time_t(max_secs)) die("Duration is too large");

    /* install signal handlers */
    sa.sa_handler = handle_signal;
    if (sigemptyset(&sa.sa_mask) < 0)
        die_errno("sigemptyset failed", errno);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        die_errno("sigaction failed", errno);

    if (show_cursor_position) {
        if (get_cursor_position(&target_x, &target_y) < 0)
            return EXIT_FAILURE;
        printf("%ld,%ld\n", target_x, target_y);
        return EXIT_SUCCESS;
    }

    if (capture_requested) {
        if (fixed_position)
            die("Use either --capture or --position, not both");

        fprintf(stderr, "Move the pointer to the target. Capturing in");
        for (long seconds = capture_secs; running && seconds > 0; seconds--) {
            fprintf(stderr, " %ld", seconds);
            fflush(stderr);
            sleep_ms(1000);
        }
        fputc('\n', stderr);

        if (!running)
            return EXIT_SUCCESS;
        if (get_cursor_position(&target_x, &target_y) < 0)
            return EXIT_FAILURE;

        fixed_position = 1;
        fprintf(stderr, "Captured position: %ld,%ld\n", target_x, target_y);
    }

    setup_uinput();

    const char *btn_name =
        (btn == BTN_LEFT) ? "left" : (btn == BTN_RIGHT) ? "right" : "middle";
    char click_limit[32];
    char duration_limit[32];

    if (max_clicks > 0)
        snprintf(click_limit, sizeof(click_limit), "%ld clicks", max_clicks);
    else
        snprintf(click_limit, sizeof(click_limit), "unlimited");

    if (max_secs > 0)
        snprintf(duration_limit, sizeof(duration_limit), "%ld seconds", max_secs);
    else
        snprintf(duration_limit, sizeof(duration_limit), "unlimited");

    fprintf(stderr,
        "hypr-autoclicker started\n"
        "  button   : %s\n"
        "  interval : %ld ms\n"
        "  hold     : %ld ms\n"
        "  position : %s\n"
        "  limit    : %s\n"
        "  duration : %s\n"
        "Press Ctrl-C to stop.\n\n",
        btn_name, interval_ms, hold_ms,
        fixed_position ? "fixed" : "current cursor",
        click_limit,
        duration_limit);

    struct timespec start = monotonic_now();
    long   count = 0;

    while (running) {
        /* time limit */
        if (max_secs > 0 && elapsed_at_least(&start, max_secs)) break;

        if (fixed_position && move_cursor_to(target_x, target_y) < 0) {
            if (!running)
                break;
            die("Cannot move cursor; ensure this runs in your Hyprland session");
        }
        click(uinput_fd, btn, hold_ms);
        if (count == LONG_MAX)
            die("Click counter overflow");
        count++;
        fprintf(stderr, "\r  clicks: %-10ld", count);
        fflush(stderr);

        /* count limit */
        if (max_clicks > 0 && count >= max_clicks) break;

        /* wait remainder of interval */
        long wait = interval_ms - hold_ms;
        if (wait > 0) sleep_ms(wait);
    }

    fprintf(stderr, "\nStopped after %ld click%s.\n", count, count == 1 ? "" : "s");

    cleanup_uinput();
    return EXIT_SUCCESS;
}
