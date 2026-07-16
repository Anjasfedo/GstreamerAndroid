#include <jni.h>
#include <android/log.h>

static jstring native_hello (JNIEnv* env, jobject thiz) {
    return (*env)->NewStringUTF(env, "Hello from C Code via JNI wleo wleo!");
}

static JNINativeMethod native_methods[] = {
        { "getHelloFromC", "()Ljava/lang/String;", (void *) native_hello}
};

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = NULL;

    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) {
        __android_log_print (ANDROID_LOG_ERROR, "tutorial-1", "Could not retrieve JNIEnv");
        return 0;
    }

    jclass klass = (*env)->FindClass (env, "com/example/gstreamerandroid/MainActivity");
    (*env)->RegisterNatives (env, klass, native_methods, sizeof(native_methods) / sizeof(native_methods[0]));

    return JNI_VERSION_1_4;
}