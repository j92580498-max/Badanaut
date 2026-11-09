#include "ui_overlay.h"
#include "cpu.h"
#include "hle/ui.h"
#include <jni.h>
#include <mutex>
#include <vector>

namespace {
struct Result { uint64_t session; std::string text; bool cancelled; };
std::mutex result_mutex;
std::vector<Result> results;
uint64_t next_session = 0, open_session = 0;
uint32_t open_object = 0;
int open_generation = 0;

bool dialog(bool show, const hle::EditField* field = nullptr) {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return false;
    jclass type = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(type, show ? "showBadaTextInput" : "dismissBadaTextInput",
                                      show ? "(JLjava/lang/String;I)V" : "()V");
    if (method) {
        if (show) {
            jstring text = env->NewStringUTF(field->text.c_str());
            env->CallVoidMethod(activity, method, static_cast<jlong>(open_session), text, field->limit);
            env->DeleteLocalRef(text);
        } else env->CallVoidMethod(activity, method);
    }
    const bool success = method && !env->ExceptionCheck();
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return success;
}
}

// Android's UI thread only queues input. All guest listener calls happen in
// pump(), on the emulator thread and outside Unicorn execution.
extern "C" JNIEXPORT void JNICALL
Java_org_badanaut_emulator_BadaActivity_nativeTextInputResult(
    JNIEnv* env, jobject, jlong session, jstring text, jboolean cancelled) {
    const char* bytes = env->GetStringUTFChars(text, nullptr);
    if (!bytes) return;
    Result result{static_cast<uint64_t>(session), bytes, cancelled == JNI_TRUE};
    env->ReleaseStringUTFChars(text, bytes);
    std::lock_guard<std::mutex> lock(result_mutex);
    results.push_back(std::move(result));
}

namespace overlay {

void init(SDL_Window*) {}
void shutdown() {
    if (open_session) dialog(false);
    open_session = 0;
    open_object = 0;
    std::lock_guard<std::mutex> lock(result_mutex);
    results.clear();
}
bool active() { return hle::ui_active(); }
bool handle_event(SDL_Event& event) {
    if (!active()) return false;
    switch (event.type) {
    case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_TEXTEDITING:
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION:
    case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION: return true;
    default: return false;
    }
}
void draw(SDL_Window*) {}
void begin_frame() {}
void present_idle(SDL_Window*) {}
void pump(Cpu& cpu) {
    std::vector<Result> pending;
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        pending.swap(results);
    }
    for (const auto& result : pending) {
        if (!open_session || result.session != open_session) continue;
        const auto fields = hle::ui_fields();
        for (auto* field : fields) {
            if (field->obj != open_object || field->generation != open_generation || !field->visible) continue;
            open_session = 0;
            open_object = 0;
            const uint32_t object = field->obj;
            const int generation = field->generation;
            if (result.cancelled) hle::ui_cancel(cpu, *field);
            else {
                hle::ui_set_text(cpu, *field, result.text);
                // A text callback can destroy the field; look it up again.
                for (auto* current : hle::ui_fields()) {
                    if (current->obj == object && current->generation == generation && current->visible) {
                        hle::ui_commit(cpu, *current, current->has_command[0] ? 0 : 1);
                        break;
                    }
                }
            }
            break;
        }
    }
    hle::EditField* visible = nullptr;
    for (auto* field : hle::ui_fields()) if (field->visible) { visible = field; break; }
    if (visible && open_session && visible->obj == open_object && visible->generation == open_generation) return;
    if (open_session) dialog(false);
    open_session = 0;
    open_object = 0;
    if (visible) {
        open_session = ++next_session;
        open_object = visible->obj;
        open_generation = visible->generation;
        if (!dialog(true, visible)) { open_session = 0; open_object = 0; }
    }
}

}
