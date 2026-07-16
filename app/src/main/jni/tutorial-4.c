#include <string.h>
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <gst/video/video.h>
#include <pthread.h>

GST_DEBUG_CATEGORY_STATIC (debug_category);
#define GST_CAT_DEFAULT debug_category

#if GLIB_SIZEOF_VOID_P == 8
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)data)
#else
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(jint)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)(jint)data)
#endif

#define SEEK_MIN_DELAY (500 * GST_MSECOND)

typedef struct _CustomData {
    jobject app;
    GstElement *pipeline;
    GMainContext *context;
    GMainLoop *main_loop;
    gboolean initialized;
    ANativeWindow *native_window;
    GstState state;
    GstState target_state;
    gint64 duration;
    gint64 desired_position;
    GstClockTime last_seek_time;
    gboolean is_live;
} CustomData;

typedef enum {
    GST_PLAY_FLAG_TEXT = (1 << 2)
} GstPlayFlags;

static pthread_t gst_app_thread;
static pthread_key_t current_jni_env;
static JavaVM *java_vm;
static jfieldID custom_data_field_id;
static jmethodID set_message_method_id;
static jmethodID set_current_position_method_id;
static jmethodID on_gstreamer_initialized_method_id;
static jmethodID on_media_size_changed_method_id;

static JNIEnv *attach_current_thread (void) {
    JNIEnv *env;
    JavaVMAttachArgs args;

    args.version = JNI_VERSION_1_4;
    args.name = NULL;
    args.group = NULL;

    if ((*java_vm)->AttachCurrentThread (java_vm, &env, &args) < 0) {
        return NULL;
    }

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
    if ((*env)->ExceptionCheck (env)) {
        (*env)->ExceptionClear (env);
    }
    (*env)->DeleteLocalRef (env, jmessage);
}

static void set_current_ui_position (gint position, gint duration, CustomData *data) {
    JNIEnv *env = get_jni_env ();
    (*env)->CallVoidMethod (env, data->app, set_current_position_method_id, position, duration);
    if ((*env)->ExceptionCheck (env)) {
        (*env)->ExceptionClear (env);
    }
}

static gboolean refresh_ui (CustomData *data) {
    gint64 position;

    if (!data || !data->pipeline || data->state < GST_STATE_PAUSED)
        return TRUE;

    if (!GST_CLOCK_TIME_IS_VALID (data->duration)) {
        gst_element_query_duration (data->pipeline, GST_FORMAT_TIME, &data->duration);
    }

    if (gst_element_query_position (data->pipeline, GST_FORMAT_TIME, &position)) {
        set_current_ui_position (position / GST_MSECOND, data->duration / GST_MSECOND, data);
    }
    return TRUE;
}

static gboolean delayed_seek_cb (CustomData *data);

static void execute_seek (gint64 desired_position, CustomData *data) {
    gint64 diff;

    if (desired_position == GST_CLOCK_TIME_NONE)
        return;

    diff = gst_util_get_timestamp () - data->last_seek_time;

    if (GST_CLOCK_TIME_IS_VALID (data->last_seek_time) && diff < SEEK_MIN_DELAY) {
        GSource *timeout_source;

        if (data->desired_position == GST_CLOCK_TIME_NONE) {
            timeout_source = g_timeout_source_new ((SEEK_MIN_DELAY - diff) / GST_MSECOND);
            g_source_set_callback (timeout_source, (GSourceFunc)delayed_seek_cb, data, NULL);
            g_source_attach (timeout_source, data->context);
            g_source_unref (timeout_source);
        }
        data->desired_position = desired_position;
    } else {
        data->last_seek_time = gst_util_get_timestamp ();
        gst_element_seek_simple (data->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, desired_position);
        data->desired_position = GST_CLOCK_TIME_NONE;
    }
}

static gboolean delayed_seek_cb (CustomData *data) {
    execute_seek (data->desired_position, data);
    return FALSE;
}

static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    GError *err;
    gchar *debug_info;
    gchar *message_string;

    gst_message_parse_error (msg, &err, &debug_info);
    message_string = g_strdup_printf ("Error received from element %s: %s", GST_OBJECT_NAME (msg->src), err->message);
    g_clear_error (&err);
    g_free (debug_info);
    set_ui_message (message_string, data);
    g_free (message_string);
    data->target_state = GST_STATE_NULL;
    gst_element_set_state (data->pipeline, GST_STATE_NULL);
}

static void eos_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    data->target_state = GST_STATE_PAUSED;
    data->is_live = (gst_element_set_state (data->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_NO_PREROLL);
    execute_seek (0, data);
}

static void duration_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    data->duration = GST_CLOCK_TIME_NONE;
}

static void buffering_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    gint percent;

    if (data->is_live)
        return;

    gst_message_parse_buffering (msg, &percent);
    if (percent < 100 && data->target_state >= GST_STATE_PAUSED) {
        gchar * message_string = g_strdup_printf ("Buffering %d%%", percent);
        gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
        set_ui_message (message_string, data);
        g_free (message_string);
    } else if (data->target_state >= GST_STATE_PLAYING) {
        gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    } else if (data->target_state >= GST_STATE_PAUSED) {
        set_ui_message ("Buffering complete", data);
    }
}

static void clock_lost_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    if (data->target_state >= GST_STATE_PLAYING) {
        gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
        gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    }
}

static void check_media_size (CustomData *data) {
    JNIEnv *env = get_jni_env ();
    GstElement *video_sink;
    GstPad *video_sink_pad;
    GstCaps *caps;
    GstVideoFormat fmt;
    int width;
    int height;

    g_object_get (data->pipeline, "video-sink", &video_sink, NULL);
    video_sink_pad = gst_element_get_static_pad (video_sink, "sink");
    caps = gst_pad_get_negotiated_caps (video_sink_pad);

    if (gst_video_format_parse_caps(caps, &fmt, &width, &height)) {
        int par_n, par_d;
        if (gst_video_parse_caps_pixel_aspect_ratio (caps, &par_n, &par_d)) {
            width = width * par_n / par_d;
        }

        (*env)->CallVoidMethod (env, data->app, on_media_size_changed_method_id, (jint)width, (jint)height);
        if ((*env)->ExceptionCheck (env)) {
            (*env)->ExceptionClear (env);
        }
    }

    gst_caps_unref(caps);
    gst_object_unref (video_sink_pad);
    gst_object_unref(video_sink);
}

static void state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    GstState old_state, new_state, pending_state;
    gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);

    if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
        data->state = new_state;
        gchar *message = g_strdup_printf("State changed to %s", gst_element_state_get_name(new_state));
        set_ui_message(message, data);
        g_free (message);

        if (old_state == GST_STATE_READY && new_state == GST_STATE_PAUSED) {
            check_media_size(data);

            if (GST_CLOCK_TIME_IS_VALID (data->desired_position))
                execute_seek (data->desired_position, data);
        }
    }
}

static void check_initialization_complete (CustomData *data) {
    JNIEnv *env = get_jni_env ();
    if (!data->initialized && data->native_window && data->main_loop) {

        gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->pipeline), (guintptr)data->native_window);

        (*env)->CallVoidMethod (env, data->app, on_gstreamer_initialized_method_id);
        if ((*env)->ExceptionCheck (env)) {
            (*env)->ExceptionClear (env);
        }
        data->initialized = TRUE;
    }
}

static void *app_function (void *userdata) {
    GstBus *bus;
    CustomData *data = (CustomData *)userdata;
    GSource *timeout_source;
    GSource *bus_source;
    GError *error = NULL;
    guint flags;

    data->context = g_main_context_new ();
    g_main_context_push_thread_default(data->context);

    data->pipeline = gst_parse_launch("playbin", &error);
    if (error) {
        gchar *message = g_strdup_printf("Unable to build pipeline: %s", error->message);
        g_clear_error (&error);
        set_ui_message(message, data);
        g_free (message);
        return NULL;
    }

    g_object_get (data->pipeline, "flags", &flags, NULL);
    flags &= ~GST_PLAY_FLAG_TEXT;
    g_object_set (data->pipeline, "flags", flags, NULL);

    data->target_state = GST_STATE_READY;
    gst_element_set_state(data->pipeline, GST_STATE_READY);

    bus = gst_element_get_bus (data->pipeline);
    bus_source = gst_bus_create_watch (bus);
    g_source_set_callback (bus_source, (GSourceFunc) gst_bus_async_signal_func, NULL, NULL);
    g_source_attach (bus_source, data->context);
    g_source_unref (bus_source);
    g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, data);
    g_signal_connect (G_OBJECT (bus), "message::eos", (GCallback)eos_cb, data);
    g_signal_connect (G_OBJECT (bus), "message::state-changed", (GCallback)state_changed_cb, data);
    g_signal_connect (G_OBJECT (bus), "message::duration", (GCallback)duration_cb, data);
    g_signal_connect (G_OBJECT (bus), "message::buffering", (GCallback)buffering_cb, data);
    g_signal_connect (G_OBJECT (bus), "message::clock-lost", (GCallback)clock_lost_cb, data);
    gst_object_unref (bus);

    timeout_source = g_timeout_source_new (250);
    g_source_set_callback (timeout_source, (GSourceFunc)refresh_ui, data, NULL);
    g_source_attach (timeout_source, data->context);
    g_source_unref (timeout_source);

    data->main_loop = g_main_loop_new (data->context, FALSE);
    check_initialization_complete (data);
    g_main_loop_run (data->main_loop);
    g_main_loop_unref (data->main_loop);
    data->main_loop = NULL;

    g_main_context_pop_thread_default(data->context);
    g_main_context_unref (data->context);
    data->target_state = GST_STATE_NULL;
    gst_element_set_state (data->pipeline, GST_STATE_NULL);
    gst_object_unref (data->pipeline);

    return NULL;
}

static void gst_native_init (JNIEnv* env, jobject thiz) {
    CustomData *data = g_new0 (CustomData, 1);
    data->desired_position = GST_CLOCK_TIME_NONE;
    data->last_seek_time = GST_CLOCK_TIME_NONE;
    SET_CUSTOM_DATA (env, thiz, custom_data_field_id, data);
    GST_DEBUG_CATEGORY_INIT (debug_category, "tutorial-4", 0, "Android tutorial 4");
    gst_debug_set_threshold_for_name("tutorial-4", GST_LEVEL_DEBUG);
    data->app = (*env)->NewGlobalRef (env, thiz);
    pthread_create (&gst_app_thread, NULL, &app_function, data);
}

static void gst_native_finalize (JNIEnv* env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    g_main_loop_quit (data->main_loop);
    pthread_join (gst_app_thread, NULL);
    (*env)->DeleteGlobalRef (env, data->app);
    g_free (data);
    SET_CUSTOM_DATA (env, thiz, custom_data_field_id, NULL);
}

void gst_native_set_uri (JNIEnv* env, jobject thiz, jstring uri) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data || !data->pipeline) return;
    const gchar *char_uri = (*env)->GetStringUTFChars (env, uri, NULL);
    if (data->target_state >= GST_STATE_READY)
        gst_element_set_state (data->pipeline, GST_STATE_READY);
    g_object_set(data->pipeline, "uri", char_uri, NULL);
    (*env)->ReleaseStringUTFChars (env, uri, char_uri);
    data->duration = GST_CLOCK_TIME_NONE;
    data->is_live = (gst_element_set_state (data->pipeline, data->target_state) == GST_STATE_CHANGE_NO_PREROLL);
}

static void gst_native_play (JNIEnv* env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    data->target_state = GST_STATE_PLAYING;
    data->is_live = (gst_element_set_state (data->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_NO_PREROLL);
}

static void gst_native_pause (JNIEnv* env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    data->target_state = GST_STATE_PAUSED;
    data->is_live = (gst_element_set_state (data->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_NO_PREROLL);
}

void gst_native_set_position (JNIEnv* env, jobject thiz, int milliseconds) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    gint64 desired_position = (gint64)(milliseconds * GST_MSECOND);
    if (data->state >= GST_STATE_PAUSED) {
        execute_seek(desired_position, data);
    } else {
        data->desired_position = desired_position;
    }
}

static jboolean gst_native_class_init (JNIEnv* env, jclass klass) {
    custom_data_field_id = (*env)->GetFieldID (env, klass, "native_custom_data", "J");
    set_message_method_id = (*env)->GetMethodID (env, klass, "setMessage", "(Ljava/lang/String;)V");
    set_current_position_method_id = (*env)->GetMethodID (env, klass, "setCurrentPosition", "(II)V");
    on_gstreamer_initialized_method_id = (*env)->GetMethodID (env, klass, "onGStreamerInitialized", "()V");
    on_media_size_changed_method_id = (*env)->GetMethodID (env, klass, "onMediaSizeChanged", "(II)V");

    if (!custom_data_field_id || !set_message_method_id || !on_gstreamer_initialized_method_id ||
        !on_media_size_changed_method_id || !set_current_position_method_id) {
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

static void gst_native_surface_init (JNIEnv *env, jobject thiz, jobject surface) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    ANativeWindow *new_native_window = ANativeWindow_fromSurface(env, surface);

    if (data->native_window) {
        ANativeWindow_release (data->native_window);
        if (data->native_window == new_native_window) {
            if (data->pipeline) {
                gst_video_overlay_expose(GST_VIDEO_OVERLAY (data->pipeline));
                gst_video_overlay_expose(GST_VIDEO_OVERLAY (data->pipeline));
            }
            return;
        } else {
            data->initialized = FALSE;
        }
    }
    data->native_window = new_native_window;

    check_initialization_complete (data);
}

static void gst_native_surface_finalize (JNIEnv *env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;

    if (data->pipeline) {
        gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->pipeline), (guintptr)NULL);
        gst_element_set_state (data->pipeline, GST_STATE_READY);
    }

    ANativeWindow_release (data->native_window);
    data->native_window = NULL;
    data->initialized = FALSE;
}

static JNINativeMethod native_methods[] = {
        { "nativeInit", "()V", (void *) gst_native_init},
        { "nativeFinalize", "()V", (void *) gst_native_finalize},
        { "nativeSetUri", "(Ljava/lang/String;)V", (void *) gst_native_set_uri},
        { "nativePlay", "()V", (void *) gst_native_play},
        { "nativePause", "()V", (void *) gst_native_pause},
        { "nativeSetPosition", "(I)V", (void*) gst_native_set_position},
        { "nativeSurfaceInit", "(Ljava/lang/Object;)V", (void *) gst_native_surface_init},
        { "nativeSurfaceFinalize", "()V", (void *) gst_native_surface_finalize},
        { "nativeClassInit", "()Z", (void *) gst_native_class_init}
};

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = NULL;

    java_vm = vm;

    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) {
        return 0;
    }

    jclass klass = (*env)->FindClass (env, "com/example/gstreamerandroid/MainActivity");
    (*env)->RegisterNatives (env, klass, native_methods, G_N_ELEMENTS(native_methods));

    pthread_key_create (&current_jni_env, detach_current_thread);

    return JNI_VERSION_1_4;
}