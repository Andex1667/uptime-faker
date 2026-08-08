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

// 프로세스별 판별 결과를 캐싱하기 위한 변수 (-1: 미판별, 0: 예외대상, 1: 변조대상)
// 매번 문자열 검사(strstr)를 하지 않아 CPU 부하와 와이파이/네트워크 렉을 완벽하게 없애줍니다.
static int g_should_fake = -1;

// 시스템 앱, 네트워크 데몬 및 런처 예외 처리 함수 (캐싱 적용)
static bool should_fake_time() {
    if (g_should_fake != -1) {
        return g_should_fake == 1;
    }

    const char *prog = getprogname();
    if (!prog) {
        g_should_fake = 0;
        return false;
    }
    
    if (strstr(prog, "systemui") ||
        strstr(prog, "launcher") ||
        strstr(prog, "wifi") ||
        strstr(prog, "netd") ||          // 네트워크 데몬 (와이파이 속도 저하 해결)
        strstr(prog, "network_stack") || // 안드로이드 네트워크 스택
        strstr(prog, "dnsmasq") ||       // DNS 관련
        strstr(prog, "dhcpcd") ||        // IP 할당 및 DHCP
        strstr(prog, "connectivity") ||  // 연결 관리 서비스
        strstr(prog, "telecom") ||
        strstr(prog, "telephony") ||     // 전화 및 통신 관련 서비스
        strstr(prog, "audio") ||         // 오디오/미디어 재생 관련 (사운드 딜레이/렉 방지)
        strstr(prog, "surfaceflinger") ||// 화면 렌더링 및 UI 합성 (전체적인 화면 버벅임 방지)
        strstr(prog, "mediaserver") ||   // 미디어 처리 백그라운드
        strstr(prog, "bluetooth") ||
        strstr(prog, "nfc") ||
        strstr(prog, "shell") ||
        strstr(prog, "chrome") ||
        strstr(prog, "gms") ||
        strstr(prog, "naver") ||
        strcmp(prog, "zygote") == 0 ||
        strcmp(prog, "zygote64") == 0) {
        g_should_fake = 0; // 예외 처리 (시간 조작 안 함)
        return false;
    }

    g_should_fake = 1; // 변조 대상 (타겟 앱)
    return true;
}

// 시간 오프셋 적용
static void apply_time_offset(clockid_t clk_id, struct timespec *tp) {
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
