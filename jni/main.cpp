#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <android/log.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <sstream>
#include <random>
#include <errno.h>
#include <dobby.h>
#include "zygisk.hpp"

#define LOG_TAG "UptimeFaker"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef __NR_memfd_create
#if defined(__aarch64__)
#define __NR_memfd_create 279
#elif defined(__arm__)
#define __NR_memfd_create 385
#elif defined(__x86_64__)
#define __NR_memfd_create 319
#elif defined(__i386__)
#define __NR_memfd_create 356
#endif
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#define USER_HZ 100

// 랜덤 오프셋 생성 함수 (시간(hour) 단위 입력받아 초(second)로 변환)
static unsigned long get_random_offset(unsigned long min_h, unsigned long max_h) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned long> dis(min_h * 3600, max_h * 3600);
    return dis(gen);
}

// 부팅 시간 오프셋: 600 ~ 800 시간 사이 랜덤
// Monotonic 시간 오프셋: 300 ~ 400 시간 사이 랜덤
static unsigned long kBootTimeOffsetSec = get_random_offset(600, 800);
static unsigned long kMonotonicOffsetSec = get_random_offset(300, 400);

static int (*orig_clock_gettime)(clockid_t clk_id, struct timespec *tp) = nullptr;
static int (*orig___clock_gettime)(clockid_t clk_id, struct timespec *tp) = nullptr;

static int (*orig_openat)(int dirfd, const char *pathname, int flags, ...) = nullptr;
static int (*orig_openat64)(int dirfd, const char *pathname, int flags, ...) = nullptr;
static int (*orig_open)(const char *pathname, int flags, ...) = nullptr;
static int (*orig_open64)(const char *pathname, int flags, ...) = nullptr;

static int (*orig___system_property_get)(const char *key, char *value) = nullptr;

static bool read_all_from_fd(int fd, std::string &out_data) {
    out_data.clear();
    char buffer[4096];
    ssize_t bytes_read = 0;
    while ((bytes_read = read(fd, buffer, sizeof(buffer))) != 0) {
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        out_data.append(buffer, static_cast<size_t>(bytes_read));
    }
    return true;
}

static int create_virtual_fd(const std::string &content) {
    const char *data = content.data();
    size_t len = content.size();

#ifdef __NR_memfd_create
    int fd = static_cast<int>(syscall(__NR_memfd_create, "proc_fake", MFD_CLOEXEC));
    if (fd >= 0) {
        ssize_t written = write(fd, data, len);
        if (written == static_cast<ssize_t>(len) && lseek(fd, 0, SEEK_SET) == 0) {
            return fd;
        }
        close(fd);
    }
#endif

    int pipefds[2];
    if (pipe2(pipefds, O_CLOEXEC) == 0) {
        ssize_t written = write(pipefds[1], data, len);
        close(pipefds[1]);
        if (written == static_cast<ssize_t>(len)) {
            return pipefds[0];
        }
        close(pipefds[0]);
    }
    return -1;
}

static int generate_fake_uptime_fd() {
    struct timespec ts_boot_real, ts_mono_real;
    if (orig_clock_gettime) {
        orig_clock_gettime(CLOCK_BOOTTIME, &ts_boot_real);
        orig_clock_gettime(CLOCK_MONOTONIC, &ts_mono_real);
    } else {
        syscall(__NR_clock_gettime, CLOCK_BOOTTIME, &ts_boot_real);
        syscall(__NR_clock_gettime, CLOCK_MONOTONIC, &ts_mono_real);
    }

    double real_boot = static_cast<double>(ts_boot_real.tv_sec) + (static_cast<double>(ts_boot_real.tv_nsec) / 1e9);
    double real_mono = static_cast<double>(ts_mono_real.tv_sec) + (static_cast<double>(ts_mono_real.tv_nsec) / 1e9);
    
    // /proc/uptime 첫 번째 값은 boottime 기준 적용
    double fake_boot = real_boot + kBootTimeOffsetSec;

    long num_cores = sysconf(_SC_NPROCESSORS_CONF);
    if (num_cores < 1) num_cores = 1;

    double idle_ratio = (real_boot > 0) ? ((real_boot - real_mono) / real_boot) : 0.25;
    if (idle_ratio < 0.1) idle_ratio = 0.25;
    double fake_idle = fake_boot * idle_ratio * static_cast<double>(num_cores);

    char buf[128];
    int len = snprintf(buf, sizeof(buf), "%.2f %.2f\n", fake_boot, fake_idle);
    if (len <= 0) return -1;
    return create_virtual_fd(std::string(buf, len));
}

static int generate_fake_stat_fd(int real_fd) {
    std::string raw_content;
    if (!read_all_from_fd(real_fd, raw_content)) {
        close(real_fd);
        return -1;
    }
    close(real_fd);

    std::istringstream stream(raw_content);
    std::string line;
    std::string fake_content;
    fake_content.reserve(raw_content.size() + 64);

    while (std::getline(stream, line)) {
        if (line.rfind("btime ", 0) == 0) {
            try {
                unsigned long real_btime = std::stoul(line.substr(6));
                unsigned long fake_btime = (real_btime > kBootTimeOffsetSec) ? (real_btime - kBootTimeOffsetSec) : real_btime;
                fake_content += "btime " + std::to_string(fake_btime) + "\n";
            } catch (...) {
                fake_content += line + "\n";
            }
        } else {
            fake_content += line + "\n";
        }
    }
    return create_virtual_fd(fake_content);
}

static int generate_fake_stat_self_fd(int real_fd) {
    std::string raw_content;
    if (!read_all_from_fd(real_fd, raw_content)) {
        close(real_fd);
        return -1;
    }
    close(real_fd);

    size_t rparen_pos = raw_content.rfind(')');
    if (rparen_pos == std::string::npos) return create_virtual_fd(raw_content);

    std::string prefix = raw_content.substr(0, rparen_pos + 1);
    std::string suffix = raw_content.substr(rparen_pos + 1);

    std::vector<std::string> tokens;
    std::istringstream iss(suffix);
    std::string token;
    while (iss >> token) tokens.push_back(token);

    if (tokens.size() > 19) {
        try {
            unsigned long long real_starttime = std::stoull(tokens[19]);
            unsigned long long offset_jiffies = static_cast<unsigned long long>(kBootTimeOffsetSec) * USER_HZ;
            tokens[19] = std::to_string(real_starttime + offset_jiffies);
        } catch (...) {}
    }

    std::string fake_content = prefix;
    for (const auto &t : tokens) fake_content += " " + t;
    fake_content += "\n";

    return create_virtual_fd(fake_content);
}

static bool is_path_match(const char *path, const char *target) {
    if (!path) return false;
    if (strcmp(path, target) == 0) return true;
    if (strcmp(path, "/proc/self/stat") == 0 && strcmp(target, "/proc/self/stat") == 0) return true;
    if (strncmp(path, "/proc/", 6) == 0 && strstr(path, "/stat") != nullptr) {
        pid_t pid = getpid();
        char self_path[64];
        snprintf(self_path, sizeof(self_path), "/proc/%d/stat", pid);
        if (strcmp(path, self_path) == 0) return true;
    }
    return false;
}

static int handle_target_openat(int dirfd, const char *pathname, int flags, mode_t mode, int (*orig_func)(int, const char*, int, ...), int syscall_num) {
    if (pathname) {
        if (strcmp(pathname, "/proc/uptime") == 0) {
            int fake_fd = generate_fake_uptime_fd();
            if (fake_fd >= 0) return fake_fd;
        } else if (strcmp(pathname, "/proc/stat") == 0) {
            int real_fd = orig_func ? orig_func(dirfd, pathname, flags, mode) : static_cast<int>(syscall(syscall_num, dirfd, pathname, flags, mode));
            if (real_fd >= 0) {
                int fake_fd = generate_fake_stat_fd(real_fd);
                if (fake_fd >= 0) return fake_fd;
            }
        } else if (is_path_match(pathname, "/proc/self/stat")) {
            int real_fd = orig_func ? orig_func(dirfd, pathname, flags, mode) : static_cast<int>(syscall(syscall_num, dirfd, pathname, flags, mode));
            if (real_fd >= 0) {
                int fake_fd = generate_fake_stat_self_fd(real_fd);
                if (fake_fd >= 0) return fake_fd;
            }
        }
    }

    if (orig_func) {
        return (flags & (O_CREAT | O_TMPFILE)) ? orig_func(dirfd, pathname, flags, mode) : orig_func(dirfd, pathname, flags);
    }
    return static_cast<int>(syscall(syscall_num, dirfd, pathname, flags, mode));
}

static int handle_target_open(const char *pathname, int flags, mode_t mode, int (*orig_func)(const char*, int, ...), int syscall_num) {
    if (pathname) {
        if (strcmp(pathname, "/proc/uptime") == 0) {
            int fake_fd = generate_fake_uptime_fd();
            if (fake_fd >= 0) return fake_fd;
        } else if (strcmp(pathname, "/proc/stat") == 0) {
            int real_fd = orig_func ? orig_func(pathname, flags, mode) : static_cast<int>(syscall(syscall_num, pathname, flags, mode));
            if (real_fd >= 0) {
                int fake_fd = generate_fake_stat_fd(real_fd);
                if (fake_fd >= 0) return fake_fd;
            }
        } else if (is_path_match(pathname, "/proc/self/stat")) {
            int real_fd = orig_func ? orig_func(pathname, flags, mode) : static_cast<int>(syscall(syscall_num, pathname, flags, mode));
            if (real_fd >= 0) {
                int fake_fd = generate_fake_stat_self_fd(real_fd);
                if (fake_fd >= 0) return fake_fd;
            }
        }
    }

    if (orig_func) {
        return (flags & (O_CREAT | O_TMPFILE)) ? orig_func(pathname, flags, mode) : orig_func(pathname, flags);
    }
    return static_cast<int>(syscall(syscall_num, pathname, flags, mode));
}

int my_openat(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    return handle_target_openat(dirfd, pathname, flags, mode, orig_openat, __NR_openat);
}

int my_openat64(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    return handle_target_openat(dirfd, pathname, flags, mode, orig_openat64, __NR_openat);
}

int my_open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
#ifdef __NR_open
    return handle_target_open(pathname, flags, mode, orig_open, __NR_open);
#else
    return handle_target_openat(AT_FDCWD, pathname, flags, mode, orig_openat, __NR_openat);
#endif
}

int my_open64(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
#ifdef __NR_open
    return handle_target_open(pathname, flags, mode, orig_open64, __NR_open);
#else
    return handle_target_openat(AT_FDCWD, pathname, flags, mode, orig_openat64, __NR_openat);
#endif
}

// 시간 종류에 따라 다른 오프셋 적용 (BOOTTIME vs MONOTONIC)
static void apply_time_offset(clockid_t clk_id, struct timespec *tp) {
    if (clk_id == CLOCK_BOOTTIME) {
        tp->tv_sec += kBootTimeOffsetSec;
    } else if (clk_id == CLOCK_MONOTONIC) {
        tp->tv_sec += kMonotonicOffsetSec;
    }
}

int my_clock_gettime(clockid_t clk_id, struct timespec *tp) {
    int ret = orig_clock_gettime ? orig_clock_gettime(clk_id, tp) : static_cast<int>(syscall(__NR_clock_gettime, clk_id, tp));
    if (ret == 0 && tp) apply_time_offset(clk_id, tp);
    return ret;
}

int my___clock_gettime(clockid_t clk_id, struct timespec *tp) {
    int ret = orig___clock_gettime ? orig___clock_gettime(clk_id, tp) :
              (orig_clock_gettime ? orig_clock_gettime(clk_id, tp) : static_cast<int>(syscall(__NR_clock_gettime, clk_id, tp)));
    if (ret == 0 && tp) apply_time_offset(clk_id, tp);
    return ret;
}

// 부트 카운트 속성 조작 (13회 고정)
int my___system_property_get(const char *key, char *value) {
    if (key && strcmp(key, "sys.boot_count") == 0) {
        if (value) {
            snprintf(value, 92, "13");
        }
        return 2;
    }
    if (orig___system_property_get) {
        return orig___system_property_get(key, value);
    }
    return -1;
}

static bool safe_hook(const char *symbol, void *new_func, void **orig_func, bool required) {
    void *sym_ptr = DobbySymbolResolver("libc.so", symbol);
    if (sym_ptr) {
        dobby_dummy_func_t orig_temp = nullptr;
        if (DobbyHook(sym_ptr, reinterpret_cast<dobby_dummy_func_t>(new_func), &orig_temp) == 0) {
            *orig_func = reinterpret_cast<void *>(orig_temp);
            LOGI("Successfully hooked %s", symbol);
            return true;
        }
    }
    if (required) {
        LOGE("CRITICAL: Mandatory hook failed for %s", symbol);
        return false;
    }
    LOGW("Optional symbol %s not found or hook skipped", symbol);
    return true;
}

class UptimeFakerModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override { this->api = api; }
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        std::call_once(init_flag, &UptimeFakerModule::apply_all_hooks_atomic, this);
    }
    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {}

private:
    zygisk::Api *api;
    static std::once_flag init_flag;

    void apply_all_hooks_atomic() {
        bool success = true;
        success &= safe_hook("clock_gettime", reinterpret_cast<void *>(my_clock_gettime), reinterpret_cast<void **>(&orig_clock_gettime), true);
        success &= safe_hook("openat", reinterpret_cast<void *>(my_openat), reinterpret_cast<void **>(&orig_openat), true);
        if (!success) return;

        safe_hook("__clock_gettime", reinterpret_cast<void *>(my___clock_gettime), reinterpret_cast<void **>(&orig___clock_gettime), false);
        safe_hook("openat64", reinterpret_cast<void *>(my_openat64), reinterpret_cast<void **>(&orig_openat64), false);
        safe_hook("open", reinterpret_cast<void *>(my_open), reinterpret_cast<void **>(&orig_open), false);
        safe_hook("open64", reinterpret_cast<void *>(my_open64), reinterpret_cast<void **>(&orig_open64), false);
        safe_hook("__system_property_get", reinterpret_cast<void *>(my___system_property_get), reinterpret_cast<void **>(&orig___system_property_get), false);
    }
};

std::once_flag UptimeFakerModule::init_flag;
REGISTER_ZYGISK_MODULE(UptimeFakerModule)
