#include "display.h"
#include <SDL2/SDL_system.h>
#include <jni.h>
#include <cstdio>

namespace display {
void android_set_display_size(int width, int height) {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    auto cls = env->GetObjectClass(activity);
    auto method = env->GetMethodID(cls, "onNativeDisplayChanged", "(II)V");
    if (method) env->CallVoidMethod(activity, method, width, height);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        printf("[display] Could not apply Android orientation/surface size %dx%d\n", width, height);
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}
} // namespace display
