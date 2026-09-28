#include "thunk_manager.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_types.h"
#include "thunks_android.h"
#include "thunks_tegra.h"
#include "bl2_startup_profile.h"

#define LOG_TAG "Arm32Bridge_Thunks"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

uint32_t ThunkManager::s_screenWidth = 1213;
uint32_t ThunkManager::s_screenHeight = 540;
uint32_t ThunkManager::s_virtualWidth = 0;
uint32_t ThunkManager::s_virtualHeight = 0;
uint32_t ThunkManager::s_forced_dpi = 0;
uint32_t ThunkManager::s_ui_inset = 0;

ThunkManager::ThunkManager(DynarmicHost& host) : m_host(host) {
    // All built-in handlers are installed before guest execution begins.
    // Reserving also keeps handler storage stable if a late subsystem adds a
    // small number of additional thunks.
    m_id_to_handler.reserve(2048);
    m_id_to_name.reserve(2048);
    m_id_to_handler.emplace_back();
    m_id_to_name.emplace_back();
}

ThunkManager::~ThunkManager() = default;

#include "thunks_opensles.h"

void RegisterGles2CoreThunks(ThunkManager& manager);

bool ThunkManager::RegisterAllThunks() {
    RegisterGLThunks();
    RegisterGles2CoreThunks(*this);
    RegisterAndroidThunks(*this, m_host);
    RegisterUnavailableTegraThunks(*this);
    RegisterOpenSLESThunks(*this, m_host);
    RegisterLibcThunks();
    // The guest-visible JNIEnv/JavaVM tables live in FakeJniEnv; jni_bridge.cpp
    // calls FakeJniEnv::Instance().Init(host) once the host JavaVM is known.
    if (m_registration_failed) {
        LOGE("FATAL: duplicate thunk/symbol registration detected; refusing to initialize guest host");
        return false;
    }
    return true;
}

#include <jni.h>
#include <sys/system_properties.h>
#include <cmath>
#include <cctype>
#include <cstring>

JavaVM* GetHostJavaVM();

std::string QueryDeviceLanguage() {
    std::string lang = "en";
    JavaVM* jvm = GetHostJavaVM();
    if (jvm) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
        }
        if (env) {
            jclass locCls = env->FindClass("java/util/Locale");
            if (locCls) {
                jmethodID getDefault = env->GetStaticMethodID(locCls, "getDefault", "()Ljava/util/Locale;");
                if (getDefault) {
                    jobject locObj = env->CallStaticObjectMethod(locCls, getDefault);
                    if (locObj) {
                        jmethodID getLang = env->GetMethodID(locCls, "getLanguage", "()Ljava/lang/String;");
                        if (getLang) {
                            jstring jStr = (jstring)env->CallObjectMethod(locObj, getLang);
                            if (jStr) {
                                const char* utf = env->GetStringUTFChars(jStr, nullptr);
                                if (utf && strlen(utf) > 0) {
                                    lang = utf;
                                }
                                if (utf) env->ReleaseStringUTFChars(jStr, utf);
                            }
                        }
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (attached) jvm->DetachCurrentThread();
        }
    }
    if (lang.empty() || lang == "en") {
        char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("persist.sys.locale", prop) > 0 ||
            __system_property_get("ro.product.locale", prop) > 0 ||
            __system_property_get("persist.sys.language", prop) > 0) {
            char* dash = strchr(prop, '-');
            if (dash) *dash = '\0';
            char* under = strchr(prop, '_');
            if (under) *under = '\0';
            if (strlen(prop) > 0) lang = prop;
        }
    }
    for (char& c : lang) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return lang;
}

std::string QueryDeviceCountryCode() {
    std::string country = "US";
    JavaVM* jvm = GetHostJavaVM();
    if (jvm) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
        }
        if (env) {
            jclass locCls = env->FindClass("java/util/Locale");
            if (locCls) {
                jmethodID getDefault = env->GetStaticMethodID(locCls, "getDefault", "()Ljava/util/Locale;");
                if (getDefault) {
                    jobject locObj = env->CallStaticObjectMethod(locCls, getDefault);
                    if (locObj) {
                        jmethodID getCountry = env->GetMethodID(locCls, "getCountry", "()Ljava/lang/String;");
                        if (getCountry) {
                            jstring jStr = (jstring)env->CallObjectMethod(locObj, getCountry);
                            if (jStr) {
                                const char* utf = env->GetStringUTFChars(jStr, nullptr);
                                if (utf && strlen(utf) > 0) {
                                    country = utf;
                                }
                                if (utf) env->ReleaseStringUTFChars(jStr, utf);
                            }
                        }
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (attached) jvm->DetachCurrentThread();
        }
    }
    if (country.empty() || country == "US") {
        char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("persist.sys.country", prop) > 0 ||
            __system_property_get("ro.product.locale.region", prop) > 0) {
            if (strlen(prop) > 0) country = prop;
        } else if (__system_property_get("persist.sys.locale", prop) > 0 ||
                   __system_property_get("ro.product.locale", prop) > 0) {
            char* dash = strchr(prop, '-');
            if (dash && strlen(dash + 1) >= 2) {
                country = std::string(dash + 1, 2);
            } else {
                char* under = strchr(prop, '_');
                if (under && strlen(under + 1) >= 2) {
                    country = std::string(under + 1, 2);
                }
            }
        }
    }
    for (char& c : country) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return country;
}

uint32_t QueryDeviceDensity() {
    if (ThunkManager::s_forced_dpi > 0) {
        return ThunkManager::s_forced_dpi;
    }
    uint32_t density = 0;
    JavaVM* jvm = GetHostJavaVM();
    if (jvm) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
        }
        if (env) {
            jclass resCls = env->FindClass("android/content/res/Resources");
            if (resCls) {
                jmethodID getSys = env->GetStaticMethodID(resCls, "getSystem", "()Landroid/content/res/Resources;");
                if (getSys) {
                    jobject resObj = env->CallStaticObjectMethod(resCls, getSys);
                    if (resObj) {
                        jmethodID getDM = env->GetMethodID(resCls, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
                        if (getDM) {
                            jobject dmObj = env->CallObjectMethod(resObj, getDM);
                            if (dmObj) {
                                jclass dmCls = env->FindClass("android/util/DisplayMetrics");
                                jfieldID fid = env->GetFieldID(dmCls, "densityDpi", "I");
                                if (fid) {
                                    density = static_cast<uint32_t>(env->GetIntField(dmObj, fid));
                                }
                            }
                        }
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (attached) jvm->DetachCurrentThread();
        }
    }
    if (density == 0) {
        char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("ro.sf.lcd_density", prop) > 0 ||
            __system_property_get("qemu.sf.lcd_density", prop) > 0) {
            density = static_cast<uint32_t>(atoi(prop));
        }
    }
    if (density == 0) {
        density = 320;
    }
    return density;
}

uint32_t QueryDeviceIsPad() {
    JavaVM* jvm = GetHostJavaVM();
    if (jvm) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
        }
        if (env) {
            // Configuration.screenLayout is the platform answer; the diagonal
            // fallback below covers devices that report it wrong.
            jclass resCls = env->FindClass("android/content/res/Resources");
            if (resCls) {
                jmethodID getSys = env->GetStaticMethodID(resCls, "getSystem", "()Landroid/content/res/Resources;");
                if (getSys) {
                    jobject resObj = env->CallStaticObjectMethod(resCls, getSys);
                    if (resObj) {
                        jmethodID getCfg = env->GetMethodID(resCls, "getConfiguration", "()Landroid/content/res/Configuration;");
                        if (getCfg) {
                            jobject cfgObj = env->CallObjectMethod(resObj, getCfg);
                            if (cfgObj) {
                                jclass cfgCls = env->FindClass("android/content/res/Configuration");
                                jfieldID slFid = env->GetFieldID(cfgCls, "screenLayout", "I");
                                if (slFid) {
                                    jint screenLayout = env->GetIntField(cfgObj, slFid);
                                    if ((screenLayout & 0x0F) >= 3) { // SCREENLAYOUT_SIZE_LARGE or XLARGE
                                        if (env->ExceptionCheck()) env->ExceptionClear();
                                        if (attached) jvm->DetachCurrentThread();
                                        return 1;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (attached) jvm->DetachCurrentThread();
        }
    }

    uint32_t w = ThunkManager::s_screenWidth;
    uint32_t h = ThunkManager::s_screenHeight;
    uint32_t dpi = QueryDeviceDensity();
    if (w > 0 && h > 0 && dpi > 0) {
        double w_in = static_cast<double>(w) / static_cast<double>(dpi);
        double h_in = static_cast<double>(h) / static_cast<double>(dpi);
        double diag = sqrt(w_in * w_in + h_in * h_in);
        if (diag >= 6.5) return 1;
    }
    return 0;
}


uint32_t ThunkManager::RegisterThunk(const std::string& name, ThunkHandler handler) {
    auto existing = m_symbol_to_address.find(name);
    if (existing != m_symbol_to_address.end()) {
        LOGE("FATAL: duplicate thunk registration for '%s' (existing guest address 0x%08X)",
             name.c_str(), existing->second);
        m_registration_failed = true;
        return 0;
    }

    uint32_t thunk_id = static_cast<uint32_t>(m_id_to_handler.size());
    uint32_t thunk_addr = m_next_thunk_addr;
    m_next_thunk_addr += kThunkStride; // 2 instructions per trampoline

    // Write ARM32 trampoline instructions into guest memory:
    // 0: SVC #thunk_id (0xEF000000 | thunk_id)
    // 4: BX LR (0xE12FFF1E)
    uint32_t svc_instr = 0xEF000000 | (thunk_id & 0x00FFFFFF);
    uint32_t bx_lr_instr = 0xE12FFF1E;

    m_host.MemoryWrite32(thunk_addr, svc_instr);
    m_host.MemoryWrite32(thunk_addr + 4, bx_lr_instr);
    // Trampolines are also created at runtime (dlsym, OpenSL ES vtables), so the JIT may
    // already hold a translation of this address range.
    m_host.InvalidateGuestCode(thunk_addr, 8);

    m_symbol_to_address[name] = thunk_addr;
    m_id_to_handler.push_back(std::move(handler));
    m_id_to_name.push_back(name);
    return thunk_addr;
}

uint32_t ThunkManager::RegisterThunkIfAbsent(const std::string& name,
                                              ThunkHandler handler) {
    const auto existing = m_symbol_to_address.find(name);
    return existing != m_symbol_to_address.end()
        ? existing->second
        : RegisterThunk(name, std::move(handler));
}

void ThunkManager::RegisterSymbol(const std::string& name, uint32_t address) {
    auto existing = m_symbol_to_address.find(name);
    if (existing != m_symbol_to_address.end()) {
        LOGE("FATAL: duplicate symbol registration for '%s' (existing 0x%08X, new 0x%08X)",
             name.c_str(), existing->second, address);
        m_registration_failed = true;
        return;
    }
    m_symbol_to_address[name] = address;
}

uint32_t ThunkManager::FindRegisteredSymbol(const std::string& name) const {
    const auto it = m_symbol_to_address.find(name);
    return it == m_symbol_to_address.end() ? 0 : it->second;
}

uint32_t ThunkManager::GetOrCreateThunkAddress(const std::string& name) {
    auto it = m_symbol_to_address.find(name);
    if (it != m_symbol_to_address.end()) {
        return it->second;
    }
    LOGE("FATAL: no explicit thunk/data implementation for '%s'", name.c_str());
    return 0;
}

void ThunkManager::DispatchThunk(uint32_t thunk_id, Dynarmic::A32::Jit& jit) {
    if (thunk_id < m_id_to_handler.size() && m_id_to_handler[thunk_id]) {
        Bl2StartupThunkScope startup_scope(thunk_id,
                                           m_id_to_name[thunk_id].c_str(), jit);
        m_id_to_handler[thunk_id](jit, m_host);
    } else {
        const char* name = thunk_id < m_id_to_name.size()
            ? m_id_to_name[thunk_id].c_str() : "unknown";
        LOGE("[THUNK] Unknown thunk ID: %u (%s) at PC: 0x%08X", thunk_id, name, jit.Regs()[15]);
        jit.Regs()[0] = 0;
    }
}

bool ThunkManager::DispatchNamedThunk(const std::string& name, Dynarmic::A32::Jit& jit) {
    auto symbol = m_symbol_to_address.find(name);
    if (symbol == m_symbol_to_address.end()) {
        LOGE("FATAL: named thunk not registered: %s", name.c_str());
        return false;
    }
    uint32_t instruction = m_host.MemoryRead32(symbol->second);
    if ((instruction & 0xFF000000u) != 0xEF000000u) {
        LOGE("FATAL: symbol '%s' is not an SVC thunk (addr=0x%08X insn=0x%08X)",
             name.c_str(), symbol->second, instruction);
        return false;
    }
    DispatchThunk(instruction & 0x00FFFFFFu, jit);
    return true;
}
