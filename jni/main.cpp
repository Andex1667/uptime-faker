#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <android/log.h>
#include <atomic>
#include <mutex>
#include <random>
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

// 부팅 시간 오프셋: 600 ~ 800 시간 랜덤
// Monotonic 시간 오프셋: 300 ~ 400 시간 랜덤
static unsigned long kBootTimeOffsetSec = get_random_offset(600, 800);
static unsigned long kMonotonicOffsetSec = get_random_offset(300, 400);
static int kBootCount = get_random_boot_count();

static int (*orig_clock_gettime)(clockid_t clk_id, struct timespec *tp) = nullptr;
static int (*orig___clock_gettime)(clockid_t clk_id, struct timespec *tp) = nullptr;
static int (*orig___system_property_get)(const char *key, char *value) = nullptr;

// 현재 프로세스가 설정 앱인지 확인
static bool is_settings_app() {
    const char *prog = getprogname();
    return prog && strstr(prog, "settings") != nullptr;
}

// 시스템 코어 및 네트워크 데몬 예외 처리 (설정 앱은 통과시켜 BOOTTIME만 조작 가능하게 함)
static bool should_fake_time() {
    const char *prog = getprogname();
    if (!prog) return true;
    
    if (strstr(prog, "launcher") ||      // 바탕화면 런처
        strstr(prog, "wifi") ||
        strstr(prog, "netd") ||          // 네트워크 데몬
        strstr(prog, "network_stack") || // 안드로이드 네트워크 스택
        strstr(prog, "dnsmasq") ||       // DNS 관련
        strstr(prog, "dhcpcd") ||        // IP 할당 및 DHCP
        strstr(prog, "connectivity") ||  // 연결 관리 서비스
        strstr(prog, "telecom") ||
        strstr(prog, "telephony") ||     // 전화 및 통신 관련 서비스
        strstr(prog, "audio") ||         // 오디오/미디어 재생 관련
        strstr(prog, "surfaceflinger") ||// 화면 렌더링 및 UI 합성
        strstr(prog, "mediaserver") ||   // 미디어 처리 백그라운드
        strstr(prog, "bluetooth") ||
        strstr(prog, "nfc") ||
        strcmp(prog, "zygote") == 0 ||
        strcmp(prog, "zygote64") == 0) {
        return false;
    }
    return true;
}

// 시간 오프셋 정밀 적용 (설정 앱은 타임아웃 렉 방지를 위해 BOOTTIME만 조작)
static void apply_time_offset(clockid_t clk_id, struct timespec *tp) {
    if (is_settings_app()) {
        // 설정 앱은 가동시간 표시를 위해 BOOTTIME만 변조하고, 모노토닉은 건드리지 않음
        if (clk_id == CLOCK_BOOTTIME) {
            tp->tv_sec += kBootTimeOffsetSec;
        }
        return;
    }

    if (!should_fake_time()) return;

    // 일반 타겟 앱 (틱톡 라이트 등)은 모두 적용
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

// 부트 카운트 조작 (9 ~ 13회 랜덤 고정)
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
    }
};

std::once_flag UptimeFakerModule::init_flag;
REGISTER_ZYGISK_MODULE(UptimeFakerModule)
