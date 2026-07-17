#include <string.h>
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <gst/video/video.h>
#include <pthread.h>
#include <gst/video/video-info.h>
#include <gst/gstpad.h>

GST_DEBUG_CATEGORY_STATIC (debug_category);
#define GST_CAT_DEFAULT debug_category

#if GLIB_SIZEOF_VOID_P == 8
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)data)
#else
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(jint)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)(jint)data)
#endif

typedef struct _CustomData {
    jobject app;
    GstElement *pipeline;
    GstElement *video_sink;        // cache the sink element
    GMainContext *context;
    GMainLoop *main_loop;
    gboolean initialized;
    ANativeWindow *native_window;
    GstState state;
    GstState target_state;
} CustomData;

static pthread_t gst_app_thread;
static pthread_key_t current_jni_env;
static JavaVM *java_vm;
static jfieldID custom_data_field_id;
static jmethodID set_message_method_id;
static jmethodID on_gstreamer_initialized_method_id;
static jmethodID on_media_size_changed_method_id;

/* ---------- JNI helpers ---------- */
static JNIEnv *attach_current_thread (void) {
    JNIEnv *env;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_4;
    args.name = NULL;
    args.group = NULL;
    if ((*java_vm)->AttachCurrentThread (java_vm, &env, &args) < 0) return NULL;
    return env;
}

static void detach_current_thread (void *env) {
    (*java_vm)->DetachCurrentThread (java_vm);
}

static JNIEnv *get_jni_env (void) {
    JNIEnv *env;
    if ((env = pthread_getspecific (current_jni_env)) == NULL) {
        env = attach_current_thread ();
        pthread_setspecific (current_jni_env, env);
    }
    return env;
}

static void set_ui_message (const gchar *message, CustomData *data) {
    JNIEnv *env = get_jni_env ();
    jstring jmessage = (*env)->NewStringUTF(env, message);
    (*env)->CallVoidMethod (env, data->app, set_message_method_id, jmessage);
    if ((*env)->ExceptionCheck (env)) (*env)->ExceptionClear (env);
    (*env)->DeleteLocalRef (env, jmessage);
}

/* ---------- Bus callbacks ---------- */
static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    GError *err;
    gchar *debug_info, *message_string;
    gst_message_parse_error (msg, &err, &debug_info);
    message_string = g_strdup_printf ("Error from %s: %s", GST_OBJECT_NAME (msg->src), err->message);
    g_clear_error (&err);
    g_free (debug_info);
    set_ui_message (message_string, data);
    g_free (message_string);
    data->target_state = GST_STATE_NULL;
    gst_element_set_state (data->pipeline, GST_STATE_NULL);
}

static void state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    GstState old_state, new_state, pending_state;
    gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
    if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
        data->state = new_state;
        gchar *message = g_strdup_printf("State changed to %s", gst_element_state_get_name(new_state));
        set_ui_message(message, data);
        g_free (message);
    }
}

/* ---------- Media size detection (after negotiation) ---------- */
static void check_media_size (CustomData *data) {
    JNIEnv *env = get_jni_env ();
    if (!data->video_sink) return;

    GstPad *pad = gst_element_get_static_pad (data->video_sink, "sink");
    if (!pad) return;

    GstCaps *caps = gst_pad_get_current_caps (pad);
    GstVideoInfo vinfo;
    if (caps && gst_video_info_from_caps (&vinfo, caps)) {
        int w = GST_VIDEO_INFO_WIDTH (&vinfo) * vinfo.par_n / vinfo.par_d;
        int h = GST_VIDEO_INFO_HEIGHT (&vinfo);
        (*env)->CallVoidMethod (env, data->app, on_media_size_changed_method_id, (jint)w, (jint)h);
        if ((*env)->ExceptionCheck (env)) (*env)->ExceptionClear (env);
    }
    if (caps) gst_caps_unref (caps);
    gst_object_unref (pad);
}

static void check_initialization_complete (CustomData *data) {
    JNIEnv *env = get_jni_env ();
    if (!data->initialized && data->native_window && data->main_loop && data->video_sink) {
        // Set window handle on the video sink (glimagesink), which implements GstVideoOverlay
        gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->video_sink), (guintptr)data->native_window);
        (*env)->CallVoidMethod (env, data->app, on_gstreamer_initialized_method_id);
        if ((*env)->ExceptionCheck (env)) (*env)->ExceptionClear (env);
        data->initialized = TRUE;
    }
}

/* ---------- Main GStreamer thread ---------- */
static void *app_function (void *userdata) {
    CustomData *data = (CustomData *)userdata;
    GstBus *bus;
    GError *error = NULL;

    data->context = g_main_context_new ();
    g_main_context_push_thread_default(data->context);

    /* Local test pipeline: videotestsrc ! glimagesink (named "video-sink") */
    data->pipeline = gst_parse_launch (
            "videotestsrc ! "
            "video/x-raw,width=640,height=480,framerate=30/1 ! "
            "queue ! glimagesink name=video-sink",
            &error);
    if (error) {
        gchar *msg = g_strdup_printf("Pipeline error: %s", error->message);
        g_clear_error (&error);
        set_ui_message(msg, data);
        g_free (msg);
        return NULL;
    }

    // Retrieve the video sink element (we'll use it for overlay and size queries)
    data->video_sink = gst_bin_get_by_name (GST_BIN (data->pipeline), "video-sink");
    if (!data->video_sink) {
        set_ui_message("Could not find video-sink element", data);
        gst_object_unref (data->pipeline);
        return NULL;
    }

    data->target_state = GST_STATE_READY;
    gst_element_set_state(data->pipeline, GST_STATE_READY);

    bus = gst_element_get_bus (data->pipeline);
    GSource *bus_source = gst_bus_create_watch (bus);
    g_source_set_callback (bus_source, (GSourceFunc) gst_bus_async_signal_func, NULL, NULL);
    g_source_attach (bus_source, data->context);
    g_source_unref (bus_source);
    g_signal_connect (bus, "message::error", (GCallback)error_cb, data);
    g_signal_connect (bus, "message::eos", (GCallback)NULL, data);   // unused
    g_signal_connect (bus, "message::state-changed", (GCallback)state_changed_cb, data);
    gst_object_unref (bus);

    // Timer to check media size periodically (will succeed after negotiation)
    GSource *size_source = g_timeout_source_new (500);
    g_source_set_callback (size_source, (GSourceFunc)check_media_size, data, NULL);
    g_source_attach (size_source, data->context);
    g_source_unref (size_source);

    data->main_loop = g_main_loop_new (data->context, FALSE);
    check_initialization_complete (data);
    g_main_loop_run (data->main_loop);

    g_main_loop_unref (data->main_loop);
    data->main_loop = NULL;

    if (data->video_sink) {
        gst_object_unref (data->video_sink);
        data->video_sink = NULL;
    }
    g_main_context_pop_thread_default(data->context);
    g_main_context_unref (data->context);

    data->target_state = GST_STATE_NULL;
    gst_element_set_state (data->pipeline, GST_STATE_NULL);
    gst_object_unref (data->pipeline);
    return NULL;
}

/* ---------- JNI exported functions ---------- */
static void gst_native_init (JNIEnv* env, jobject thiz) {
    CustomData *data = g_new0 (CustomData, 1);
    SET_CUSTOM_DATA (env, thiz, custom_data_field_id, data);
    GST_DEBUG_CATEGORY_INIT (debug_category, "tutorial-4", 0, "Android test");
    data->app = (*env)->NewGlobalRef (env, thiz);
    pthread_create (&gst_app_thread, NULL, &app_function, data);
}

static void gst_native_finalize (JNIEnv* env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    if (data->main_loop) g_main_loop_quit (data->main_loop);
    pthread_join (gst_app_thread, NULL);
    (*env)->DeleteGlobalRef (env, data->app);
    g_free (data);
    SET_CUSTOM_DATA (env, thiz, custom_data_field_id, NULL);
}

static void gst_native_play (JNIEnv* env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    data->target_state = GST_STATE_PLAYING;
    gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
}

static void gst_native_pause (JNIEnv* env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    data->target_state = GST_STATE_PAUSED;
    gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
}

static void gst_native_surface_init (JNIEnv *env, jobject thiz, jobject surface) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    ANativeWindow *new_win = ANativeWindow_fromSurface(env, surface);
    if (data->native_window) {
        ANativeWindow_release (data->native_window);
        if (data->native_window == new_win) {
            if (data->video_sink) {
                gst_video_overlay_expose (GST_VIDEO_OVERLAY (data->video_sink));
            }
            return;
        } else {
            data->initialized = FALSE;
        }
    }
    data->native_window = new_win;
    check_initialization_complete (data);
}

static void gst_native_surface_finalize (JNIEnv *env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    if (data->video_sink) {
        gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->video_sink), (guintptr)NULL);
        gst_element_set_state (data->pipeline, GST_STATE_READY);
    }
    ANativeWindow_release (data->native_window);
    data->native_window = NULL;
    data->initialized = FALSE;
}

static jstring gst_native_get_version (JNIEnv* env, jobject thiz) {
    char *ver = gst_version_string ();
    jstring jver = (*env)->NewStringUTF (env, ver);
    g_free (ver);
    return jver;
}

static jboolean gst_native_class_init (JNIEnv* env, jclass klass) {
    custom_data_field_id = (*env)->GetFieldID (env, klass, "native_custom_data", "J");
    set_message_method_id = (*env)->GetMethodID (env, klass, "setMessage", "(Ljava/lang/String;)V");
    on_gstreamer_initialized_method_id = (*env)->GetMethodID (env, klass, "onGStreamerInitialized", "()V");
    on_media_size_changed_method_id = (*env)->GetMethodID (env, klass, "onMediaSizeChanged", "(II)V");

    if (!custom_data_field_id || !set_message_method_id ||
        !on_gstreamer_initialized_method_id || !on_media_size_changed_method_id)
        return JNI_FALSE;
    return JNI_TRUE;
}

static JNINativeMethod native_methods[] = {
        { "nativeInit", "()V", (void *) gst_native_init},
        { "nativeFinalize", "()V", (void *) gst_native_finalize},
        { "nativePlay", "()V", (void *) gst_native_play},
        { "nativePause", "()V", (void *) gst_native_pause},
        { "nativeSurfaceInit", "(Ljava/lang/Object;)V", (void *) gst_native_surface_init},
        { "nativeSurfaceFinalize", "()V", (void *) gst_native_surface_finalize},
        { "nativeGetVersion", "()Ljava/lang/String;", (void *) gst_native_get_version},
        { "nativeClassInit", "()Z", (void *) gst_native_class_init}
};

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = NULL;
    java_vm = vm;
    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) return 0;
    jclass klass = (*env)->FindClass (env, "com/example/gstreamerandroid/MainActivity");
    if ((*env)->RegisterNatives(env, klass, native_methods, G_N_ELEMENTS(native_methods)) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "JNI", "Failed to register natives");
        return 0;
    }
    pthread_key_create (&current_jni_env, detach_current_thread);
    return JNI_VERSION_1_4;
}