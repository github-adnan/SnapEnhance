#include <jni.h>
#include <string>
#include <dobby.h>
#include <vector>

#include "logger.h"
#include "config.h"
#include "util.h"
#include "grpc.h"

static native_config_t *native_config;
static JavaVM *java_vm;

static auto fstat_original = (int (*)(int, struct stat *)) nullptr;

static int fstat_hook(int fd, struct stat *buf) {
    char name[256];
    memset(name, 0, 256);
    snprintf(name, sizeof(name), "/proc/self/fd/%d", fd);
    readlink(name, name, sizeof(name));

    auto fileName = std::string(name);

    //prevent blizzardv2 metrics
    if (native_config->disable_metrics &&
        fileName.find("files/blizzardv2/queues") != std::string::npos) {
        unlink(name);
        return -1;
    }

    //prevent bitmoji to load
    if (native_config->disable_bitmoji &&
        fileName.find("com.snap.file_manager_4_SCContent") != std::string::npos) {
        return -1;
    }

    return fstat_original(fd, buf);
}


static jobject native_lib_object;
static jmethodID native_lib_on_unary_call_method;

static auto unaryCall_original = (void *(*)(void *, const char *, grpc::grpc_byte_buffer **, void *,
                                            void *, void *)) nullptr;

static void *
unaryCall_hook(void *unk1, const char *uri, grpc::grpc_byte_buffer **buffer_ptr, void *unk4,
               void *unk5, void *unk6) {
    auto slice_buffer = (*buffer_ptr)->slice_buffer;
    // request without reference counter can be hooked using xposed ig
    if (slice_buffer->ref_counter == 0) {
        return unaryCall_original(unk1, uri, buffer_ptr, unk4, unk5, unk6);
    }

    auto env = (JNIEnv *) nullptr;
    java_vm->GetEnv((void **) &env, JNI_VERSION_1_6);

    auto jni_buffer_array = env->NewByteArray(slice_buffer->length);
    env->SetByteArrayRegion(jni_buffer_array, 0, slice_buffer->length,
                            (jbyte *) slice_buffer->data);

    auto native_request_data_object = env->CallObjectMethod(native_lib_object,
                                                            native_lib_on_unary_call_method,
                                                            env->NewStringUTF(uri),
                                                            jni_buffer_array);

    if (native_request_data_object != nullptr) {
        auto native_request_data_class = env->GetObjectClass(native_request_data_object);
        auto is_canceled = env->GetBooleanField(native_request_data_object,
                                                env->GetFieldID(native_request_data_class,
                                                                "canceled", "Z"));
        if (is_canceled) {
            LOGD("canceled request for %s", uri);
            return nullptr;
        }

        auto new_buffer = env->GetObjectField(native_request_data_object,
                                              env->GetFieldID(native_request_data_class, "buffer",
                                                              "[B"));
        auto new_buffer_length = env->GetArrayLength((jbyteArray) new_buffer);
        auto new_buffer_data = env->GetByteArrayElements((jbyteArray) new_buffer, nullptr);

        LOGD("rewrote request for %s (length: %d)", uri, new_buffer_length);
        //we need to allocate a new ref_counter struct and copy the old ref_counter and the new_buffer to it
        const static auto ref_counter_struct_size =
                (uintptr_t) slice_buffer->data - (uintptr_t) slice_buffer->ref_counter;

        auto new_ref_counter = malloc(ref_counter_struct_size + new_buffer_length);
        //copy the old ref_counter and the native_request_data_object
        memcpy(new_ref_counter, slice_buffer->ref_counter, ref_counter_struct_size);
        memcpy((void *) ((uintptr_t) new_ref_counter + ref_counter_struct_size), new_buffer_data,
               new_buffer_length);

        //free the old ref_counter
        free(slice_buffer->ref_counter);

        //update the slice_buffer
        slice_buffer->ref_counter = new_ref_counter;
        slice_buffer->length = new_buffer_length;
        slice_buffer->data = (uint8_t *) ((uintptr_t) new_ref_counter + ref_counter_struct_size);
    }

    return unaryCall_original(unk1, uri, buffer_ptr, unk4, unk5, unk6);
}


void JNICALL init(JNIEnv *env, jobject clazz, jobject classloader) {
    LOGD("Initializing native");
    // config
    native_config = new native_config_t;

    // native lib object
    native_lib_object = env->NewGlobalRef(clazz);
    native_lib_on_unary_call_method = env->GetMethodID(env->GetObjectClass(clazz),
                                                       "onNativeUnaryCall",
                                                       "(Ljava/lang/String;[B)Lme/rhunk/snapenhance/nativelib/NativeRequestData;");

    // load libclient.so
    util::load_library(env, classloader, "client");
    auto client_module = util::get_module("libclient.so");
    if (client_module.base == 0) {
        LOGE("libclient not found");
        return;
    }
    //client_module.base -= 0x1000; // debugging purposes
    LOGD("libclient.so offset=%u, size=%u", client_module.base, client_module.size);

    // hooks
    DobbyHook((void *) DobbySymbolResolver("libc.so", "fstat"), (void *) fstat_hook,
              (void **) &fstat_original);

    //signature might change in the future (unstable for now)
    auto unaryCall_func = util::find_signature(client_module.base, client_module.size,
                                               "FD 7B BA A9 FC 6F 01 A9 FA 67 02 A9 F8 5F 03 A9 F6 57 04 A9 F4 4F 05 A9 FD 03 00 91 FF 43 13 D1");
    if (unaryCall_func != 0) {
        DobbyHook((void *) unaryCall_func, (void *) unaryCall_hook, (void **) &unaryCall_original);
    } else {
        LOGE("can't find unaryCall signature");
    }

    LOGD("Native initialized");
}

void JNICALL load_config(JNIEnv *env, jobject _, jobject config_object) {
    auto native_config_clazz = env->GetObjectClass(config_object);
#define GET_CONFIG_BOOL(name) env->GetBooleanField(config_object, env->GetFieldID(native_config_clazz, name, "Z"))

    native_config->disable_bitmoji = GET_CONFIG_BOOL("disableBitmoji");
    native_config->disable_metrics = GET_CONFIG_BOOL("disableMetrics");
}

extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *_) {
    java_vm = vm;
    // register native methods
    JNIEnv *env = nullptr;
    vm->GetEnv((void **) &env, JNI_VERSION_1_6);

    auto methods = std::vector<JNINativeMethod>();
    methods.push_back({"init", "(Ljava/lang/ClassLoader;)V", (void *) init});
    methods.push_back({"loadConfig", "(Lme/rhunk/snapenhance/nativelib/NativeConfig;)V",
                       (void *) load_config});

    env->RegisterNatives(
            env->FindClass("me/rhunk/snapenhance/nativelib/NativeLib"),
            methods.data(),
            methods.size()
    );
    return JNI_VERSION_1_6;
}