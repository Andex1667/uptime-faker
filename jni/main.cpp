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

// 랜덤 오프셋 생성 함수 (시간 단위 -> 초 단위 변환)
static unsigned long get_random_offset(unsigned long min_h, unsigned long max_h) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned long> dis(min_h * 3600, max_h * 3600);
    return dis(gen);
}

// 부팅 시간 오프셋: 600 ~ 800 시간 랜덤
// Monotonic 시간 오프셋: 300 ~ 400 시간 랜덤
static unsigned long kBootTimeOffsetSec = get_random_offset(600, 800);
static unsigned long kMonotonicOffsetSec = get_random_offset(300, 400);

static int (*orig_clock_gettime)(clockid_t clk_id, struct timespec *tp) = nullptr;
static int (*orig___clock_gettime)(clockid_t clk_id, struct timespec *tp) = nullptr;
static int (*orig_openat)(int dirfd, const char *pathname, int flags, ...) = nullptr;
static int (*orig_open)(const char *pathname, int flags, ...) = nullptr;
static int (*orig___system_property_get)(const char *key, char *value) = nullptr;

// 가상 파일 생성 (가벼운 memfd 또는 pipe 활용)
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

// /proc/uptime 전용 가상 파일 생성
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

// openat 후킹 (/proc/uptime만 안전하게 가로챔)
int my_openat(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    if (pathname && strcmp(pathname, "/proc/uptime") == 0) {
        int fake_fd = generate_fake_uptime_fd();
        if (fake_fd >= 0) return fake_fd;
    }

    if (orig_openat) {
        return (flags & (O_CREAT | O_TMPFILE)) ? orig_openat(dirfd, pathname, flags, mode) : orig_openat(dirfd, pathname, flags);
    }
    return static_cast<int>(syscall(__NR_openat, dirfd, pathname, flags, mode));
}

// open 후킹
int my_open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    if (pathname && strcmp(pathname, "/proc/uptime") == 0) {
        int fake_fd = generate_fake_uptime_fd();
        if (fake_fd >= 0) return fake_fd;
    }

#ifdef __NR_open
    if (orig_open) {
        return (flags & (O_CREAT | O_TMPFILE)) ? orig_open(pathname, flags, mode) : orig_open(pathname, flags);
    }
    return static_cast<int>(syscall(__NR_open, pathname, flags, mode));
#else
    return my_openat(AT_FDCWD, pathname, flags, mode);
#endif
}

// 시간 오프셋 적용 (BOOTTIME과 MONOTONIC 분기)
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

// 부트 카운트 조작 (13회 고정)
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
        // 현재 실행 중인 프로세스(앱) 이름 가져오기
        const char *process = args->nice_name;
        if (process) {
            // 바탕화면 런처, 시스템 UI, 네트워크/와이파이 관리 프로세스 등은 예외 처리하여 오류 방지
            if (strstr(process, "com.android.systemui") ||
                strstr(process, "launcher") ||
                strstr(process, "com.android.providers") ||
                strstr(process, "com.android.phone") ||
                strstr(process, "android.process.media")) {
                return;
            }
        }

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
        safe_hook("open", reinterpret_cast<void *>(my_open), reinterpret_cast<void **>(&orig_open), false);
        safe_hook("__system_property_get", reinterpret_cast<void *>(my___system_property_get), reinterpret_cast<void **>(&orig___system_property_get), false);
    }
};

std::once_flag UptimeFakerModule::init_flag;
REGISTER_ZYGISK_MODULE(UptimeFakerModule)
