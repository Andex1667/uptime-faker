#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <android/log.h>
#include <atomic>
#include <mutex>
#include <random>
#include <unwind.h>
#include <dlfcn.h>
#include <dobby.h>
#include "zygisk.hpp"

#define LOG_TAG "UptimeFaker"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 랜덤 오프셋 생성 함수 (시간 단위 -> 초 단위 변환)
static unsigned long get_random_offset(unsigned long min_h, unsigned long max_h) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned long> dis(min_h * 3600, max_h * 3600);
    return dis(gen);
}

// 부팅 횟수 랜덤 생성 (9 ~ 13회)
static int get_random_boot_count() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(9, 13);
    return dis(gen);
}

static unsigned long kBootTimeOffsetSec = get_random_offset(600, 800);
static unsigned long kMonotonicOffsetSec = get_random_offset(300, 400);
static int kBootCount = get_random_boot_count();

static int (*orig_clock_gettime)(clockid_t clk_id, struct timespec *tp) = nullptr;
static int (*orig___clock_gettime)(clockid_t clk_id, struct timespec *tp) = nullptr;
static int (*orig___system_property_get)(const char *key, char *value) = nullptr;
static int (*orig_open)(const char *pathname, int flags, mode_t mode) = nullptr;
static int (*orig_openat)(int dirfd, const char *pathname, int flags, mode_t mode) = nullptr;

// 스택 추적용 구조체
struct StackState {
    bool is_wifi_related;
};

// 백트레이스 콜백 함수 (호출 주소의 심볼을 분석하여 와이파이 관련 로직인지 판별)
static _Unwind_Reason_Code unwind_callback(struct _Unwind_Context *context, void *arg) {
    StackState *state = static_cast<StackState *>(arg);
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc == 0) return _Unwind_Reason_Code::_URC_END_OF_STACK;

    Dl_info info;
    if (dladdr(reinterpret_cast<void *>(pc), &info) && info.dli_sname) {
        // 와이파이, 스캔, 타임아웃, 네트워크 관련 키워드가 스택에 감지되면 true 설정
        if (strstr(info.dli_sname, "Wifi") ||
            strstr(info.dli_sname, "Scan") ||
            strstr(info.dli_sname, "Network") ||
            strstr(info.dli_sname, "Timeout") ||
            strstr(info.dli_sname, "Connectivity")) {
            state->is_wifi_related = true;
            return _Unwind_Reason_Code::_URC_END_OF_STACK;
        }
    }
    return _Unwind_Reason_Code::_URC_NO_REASON;
}

// 설정 앱 내부일 때, 와이파이 관련 호출인지 검사
static bool should_bypass_settings_fake() {
    StackState state = { false };
    _Unwind_Backtrace(unwind_callback, &state);
    return state.is_wifi_related;
}

// 일반 앱 및 시스템 프로세스 예외 처리
static bool should_fake_time() {
    const char *prog = getprogname();
    if (!prog) return true;
    
    if (strstr(prog, "systemui") ||
        strstr(prog, "launcher") ||
        strstr(prog, "wifi") ||
        strstr(prog, "netd") ||
        strstr(prog, "network_stack") ||
        strstr(prog, "dnsmasq") ||
        strstr(prog, "dhcpcd") ||
        strstr(prog, "connectivity") ||
        strstr(prog, "telecom") ||
        strstr(prog, "telephony") ||
        strstr(prog, "audio") ||
        strstr(prog, "surfaceflinger") ||
        strstr(prog, "mediaserver") ||
        strstr(prog, "bluetooth") ||
        strstr(prog, "nfc") ||
        strstr(prog, "chrome") ||
        strstr(prog, "naver") ||
        strstr(prog, "youtube") ||
        strstr(prog, "gms") ||
        strstr(prog, "shell") ||
        strcmp(prog, "zygote") == 0 ||
        strcmp(prog, "zygote64") == 0) {
        return false;
    }
    return true;
}

// 시간 오프셋 적용 (설정 앱은 와이파이 함수가 아닐 때만 BOOTTIME 조작)
static void apply_time_offset(clockid_t clk_id, struct timespec *tp) {
    const char *prog = getprogname();
    bool is_settings = prog && (strstr(prog, "settings") != nullptr);

    if (is_settings) {
        // 설정 앱이지만 와이파이 스캔/타임아웃 쪽에서 부른 거라면 순정 시간 유지 (렉 방지)
        if (should_bypass_settings_fake()) {
            return;
        }
        // 그 외(가동시간 UI 렌더링 등)는 부팅 시간 뻥튀기 적용
        if (clk_id == CLOCK_BOOTTIME) {
            tp->tv_sec += kBootTimeOffsetSec;
        }
        return;
    }

    if (!should_fake_time()) return;

    if (clk_id == CLOCK_BOOTTIME) {
        tp->tv_sec += kBootTimeOffsetSec;
    } else if (clk_id == CLOCK_MONOTONIC) {
        tp->tv_sec += kMonotonicOffsetSec;
    }
}

int my_clock_gettime(clockid_t clk_id, struct timespec *tp) {
    int ret = orig_clock_gettime ? orig_clock_gettime(clk_id, tp) : static_cast<int>(syscall(__NR_clock_gettime, clk_id, tp));
    if (ret == 0 && tp) {
        apply_time_offset(clk_id, tp);
    }
    return ret;
}

int my___clock_gettime(clockid_t clk_id, struct timespec *tp) {
    int ret = orig___clock_gettime ? orig___clock_gettime(clk_id, tp) :
              (orig_clock_gettime ? orig_clock_gettime(clk_id, tp) : static_cast<int>(syscall(__NR_clock_gettime, clk_id, tp)));
    if (ret == 0 && tp) {
        apply_time_offset(clk_id, tp);
    }
    return ret;
}

int my___system_property_get(const char *key, char *value) {
    if (key && strcmp(key, "sys.boot_count") == 0) {
        if (value) {
            snprintf(value, 92, "%d", kBootCount);
        }
        return 2;
    }
    if (orig___system_property_get) {
        return orig___system_property_get(key, value);
    }
    return -1;
}

static int create_fake_uptime_fd() {
    struct timespec tp;
    if (orig_clock_gettime) {
        orig_clock_gettime(CLOCK_BOOTTIME, &tp);
    } else {
        syscall(__NR_clock_gettime, CLOCK_BOOTTIME, &tp);
    }
    
    if (should_fake_time()) {
        tp.tv_sec += kBootTimeOffsetSec;
    }

    char buf[128];
    int len = snprintf(buf, sizeof(buf), "%lu.00 %lu.00\n", tp.tv_sec, tp.tv_sec);

    int fd = syscall(__NR_memfd_create, "uptime_fake", MFD_CLOEXEC);
    if (fd != -1) {
        write(fd, buf, len);
        lseek(fd, 0, SEEK_SET);
    }
    return fd;
}

int my_open(const char *pathname, int flags, mode_t mode) {
    if (pathname && strstr(pathname, "proc/uptime")) {
        int fake_fd = create_fake_uptime_fd();
        if (fake_fd != -1) return fake_fd;
    }
    return orig_open ? orig_open(pathname, flags, mode) : open(pathname, flags, mode);
}

int my_openat(int dirfd, const char *pathname, int flags, mode_t mode) {
    if (pathname && strstr(pathname, "proc/uptime")) {
        int fake_fd = create_fake_uptime_fd();
        if (fake_fd != -1) return fake_fd;
    }
    return orig_openat ? orig_openat(dirfd, pathname, flags, mode) : openat(dirfd, pathname, flags, mode);
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
        if (!success) return;

        safe_hook("__clock_gettime", reinterpret_cast<void *>(my___clock_gettime), reinterpret_cast<void **>(&orig___clock_gettime), false);
        safe_hook("__system_property_get", reinterpret_cast<void *>(my___system_property_get), reinterpret_cast<void **>(&orig___system_property_get), false);
        safe_hook("open", reinterpret_cast<void *>(my_open), reinterpret_cast<void **>(&orig_open), false);
        safe_hook("openat", reinterpret_cast<void *>(my_openat), reinterpret_cast<void **>(&orig_openat), false);
    }
};

std::once_flag UptimeFakerModule::init_flag;
REGISTER_ZYGISK_MODULE(UptimeFakerModule)
