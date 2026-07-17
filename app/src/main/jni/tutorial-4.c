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
#include <gst/gstdevice.h>
#include <unistd.h>          /* for dup, close */

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
    GstElement *video_sink;
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
    GSource *bus_source;
    GSource *timeout_source;
} CustomData;

static void check_media_size (CustomData *data);
static gboolean refresh_ui (CustomData *data);
static void check_initialization_complete (CustomData *data);
static void cleanup_pipeline (CustomData *data);
static void start_pipeline_with_uri (CustomData *data, const gchar *uri);
static void start_pipeline_with_camera (CustomData *data, gint camera_index);
static void start_pipeline_with_v4l2 (CustomData *data, const gchar *device);
static void start_pipeline_with_fd (CustomData *data, int usb_fd);

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
static jmethodID on_camera_device_found_method_id;

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

static void set_current_ui_position (gint position, gint duration, CustomData *data) {
    JNIEnv *env = get_jni_env ();
    (*env)->CallVoidMethod (env, data->app, set_current_position_method_id, position, duration);
    if ((*env)->ExceptionCheck (env)) (*env)->ExceptionClear (env);
}

static gboolean delayed_seek_cb (CustomData *data);

static void execute_seek (gint64 desired_position, CustomData *data) {
    gint64 diff;
    if (desired_position == GST_CLOCK_TIME_NONE) return;
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
        gst_element_seek_simple (data->pipeline, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, desired_position);
        data->desired_position = GST_CLOCK_TIME_NONE;
    }
}

static gboolean delayed_seek_cb (CustomData *data) {
    execute_seek (data->desired_position, data);
    return FALSE;
}

static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    GError *err; gchar *debug_info, *message_string;
    gst_message_parse_error (msg, &err, &debug_info);
    message_string = g_strdup_printf ("Error from %s: %s", GST_OBJECT_NAME (msg->src), err->message);
    g_clear_error (&err); g_free (debug_info);
    set_ui_message (message_string, data); g_free (message_string);
    data->target_state = GST_STATE_NULL;
    gst_element_set_state (data->pipeline, GST_STATE_NULL);
}

static void eos_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    data->target_state = GST_STATE_PAUSED;
    data->is_live = (gst_element_set_state (data->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_NO_PREROLL);
    execute_seek (0, data);
}

static void duration_cb (GstBus *bus, GstMessage *msg, CustomData *data) { data->duration = GST_CLOCK_TIME_NONE; }

static void buffering_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    gint percent;
    if (data->is_live) return;
    gst_message_parse_buffering (msg, &percent);
    if (percent < 100 && data->target_state >= GST_STATE_PAUSED) {
        gchar * message_string = g_strdup_printf ("Buffering %d%%", percent);
        gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
        set_ui_message (message_string, data); g_free (message_string);
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

static void state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
    GstState old_state, new_state, pending_state;
    gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
    if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
        data->state = new_state;
        gchar *message = g_strdup_printf("State changed to %s", gst_element_state_get_name(new_state));
        set_ui_message(message, data); g_free (message);
        if (old_state == GST_STATE_READY && new_state == GST_STATE_PAUSED) {
            check_media_size (data);
            if (GST_CLOCK_TIME_IS_VALID (data->desired_position))
                execute_seek (data->desired_position, data);
        }
    }
}

static void check_media_size (CustomData *data) {
    JNIEnv *env = get_jni_env ();
    if (!data->video_sink) {
        g_object_get (data->pipeline, "video-sink", &data->video_sink, NULL);
        if (data->video_sink && data->native_window)
            gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->video_sink), (guintptr)data->native_window);
    }
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
    if (!data->initialized && data->native_window && data->main_loop) {
        (*env)->CallVoidMethod (env, data->app, on_gstreamer_initialized_method_id);
        if ((*env)->ExceptionCheck (env)) (*env)->ExceptionClear (env);
        data->initialized = TRUE;
        if (!data->video_sink) g_object_get (data->pipeline, "video-sink", &data->video_sink, NULL);
        if (data->video_sink)
            gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->video_sink), (guintptr)data->native_window);
    }
}

static void cleanup_pipeline (CustomData *data) {
    if (data->pipeline) {
        gst_element_set_state (data->pipeline, GST_STATE_NULL);
        if (data->bus_source) { g_source_destroy (data->bus_source); g_source_unref (data->bus_source); data->bus_source = NULL; }
        if (data->timeout_source) { g_source_destroy (data->timeout_source); g_source_unref (data->timeout_source); data->timeout_source = NULL; }
        if (data->video_sink) { gst_object_unref (data->video_sink); data->video_sink = NULL; }
        gst_object_unref (data->pipeline);
        data->pipeline = NULL;
    }
}

static void start_pipeline_with_uri (CustomData *data, const gchar *uri) {
    cleanup_pipeline (data);
    data->pipeline = gst_parse_launch("playbin", NULL);
    if (!data->pipeline) { set_ui_message("Failed to create playbin", data); return; }
    guint flags; g_object_get (data->pipeline, "flags", &flags, NULL);
    flags &= ~GST_PLAY_FLAG_TEXT; g_object_set (data->pipeline, "flags", flags, NULL);
    g_object_set (data->pipeline, "uri", uri, NULL);
    data->duration = GST_CLOCK_TIME_NONE; data->target_state = GST_STATE_PLAYING;
    GstBus *bus = gst_element_get_bus (data->pipeline);
    data->bus_source = gst_bus_create_watch (bus);
    g_source_set_callback (data->bus_source, (GSourceFunc) gst_bus_async_signal_func, NULL, NULL);
    g_source_attach (data->bus_source, data->context);
    g_signal_connect (bus, "message::error", (GCallback)error_cb, data);
    g_signal_connect (bus, "message::eos", (GCallback)eos_cb, data);
    g_signal_connect (bus, "message::state-changed", (GCallback)state_changed_cb, data);
    g_signal_connect (bus, "message::duration", (GCallback)duration_cb, data);
    g_signal_connect (bus, "message::buffering", (GCallback)buffering_cb, data);
    g_signal_connect (bus, "message::clock-lost", (GCallback)clock_lost_cb, data);
    gst_object_unref (bus);
    data->timeout_source = g_timeout_source_new (250);
    g_source_set_callback (data->timeout_source, (GSourceFunc)refresh_ui, data, NULL);
    g_source_attach (data->timeout_source, data->context);
    gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    data->is_live = FALSE;
}

/* ---------- Camera pipeline (ahcsrc by index) ---------- */
static void start_pipeline_with_camera (CustomData *data, gint camera_index) {
    cleanup_pipeline (data);
    gchar *desc = g_strdup_printf (
            "ahcsrc camera-index=%d ! "
            "video/x-raw,width=640,height=480,framerate=30/1 ! "
            "glimagesink name=video-sink",
            camera_index);
    data->pipeline = gst_parse_launch (desc, NULL);
    g_free (desc);
    if (!data->pipeline) { set_ui_message ("Failed to create camera pipeline", data); return; }
    data->video_sink = gst_bin_get_by_name (GST_BIN (data->pipeline), "video-sink");
    if (data->video_sink && data->native_window)
        gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->video_sink), (guintptr)data->native_window);
    GstBus *bus = gst_element_get_bus (data->pipeline);
    data->bus_source = gst_bus_create_watch (bus);
    g_source_set_callback (data->bus_source, (GSourceFunc) gst_bus_async_signal_func, NULL, NULL);
    g_source_attach (data->bus_source, data->context);
    g_signal_connect (bus, "message::error", (GCallback)error_cb, data);
    g_signal_connect (bus, "message::state-changed", (GCallback)state_changed_cb, data);
    gst_object_unref (bus);
    data->timeout_source = g_timeout_source_new (250);
    g_source_set_callback (data->timeout_source, (GSourceFunc)refresh_ui, data, NULL);
    g_source_attach (data->timeout_source, data->context);
    data->target_state = GST_STATE_PLAYING;
    gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    data->is_live = TRUE;
}

/* ---------- V4L2 source pipeline ---------- */
static void start_pipeline_with_v4l2 (CustomData *data, const gchar *device) {
    cleanup_pipeline (data);
    gchar *desc = g_strdup_printf (
            "v4l2src device=%s ! "
            "video/x-raw,width=640,height=480,framerate=30/1 ! "
            "glimagesink name=video-sink",
            device);
    data->pipeline = gst_parse_launch (desc, NULL);
    g_free (desc);
    if (!data->pipeline) { set_ui_message("Failed to create v4l2 pipeline", data); return; }
    data->video_sink = gst_bin_get_by_name (GST_BIN (data->pipeline), "video-sink");
    if (data->video_sink && data->native_window)
        gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->video_sink), (guintptr)data->native_window);
    GstBus *bus = gst_element_get_bus (data->pipeline);
    data->bus_source = gst_bus_create_watch (bus);
    g_source_set_callback (data->bus_source, (GSourceFunc) gst_bus_async_signal_func, NULL, NULL);
    g_source_attach (data->bus_source, data->context);
    g_signal_connect (bus, "message::error", (GCallback)error_cb, data);
    g_signal_connect (bus, "message::state-changed", (GCallback)state_changed_cb, data);
    gst_object_unref (bus);
    data->timeout_source = g_timeout_source_new (250);
    g_source_set_callback (data->timeout_source, (GSourceFunc)refresh_ui, data, NULL);
    g_source_attach (data->timeout_source, data->context);
    data->target_state = GST_STATE_PLAYING;
    gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    data->is_live = TRUE;
}

/* ---------- Raw FD pipeline (USB camera via ahcsrc fd property) ---------- */
static void start_pipeline_with_fd (CustomData *data, int usb_fd) {
    cleanup_pipeline (data);

    int dup_fd = dup(usb_fd);
    if (dup_fd < 0) {
        set_ui_message("Failed to duplicate file descriptor", data);
        return;
    }

    GstElement *ahcsrc = gst_element_factory_make ("ahcsrc", "source");
    GstElement *capsfilter = gst_element_factory_make ("capsfilter", "filter");
    GstElement *videosink = gst_element_factory_make ("glimagesink", "video-sink");

    if (!ahcsrc || !capsfilter || !videosink) {
        set_ui_message("Failed to create FD pipeline elements (ahcsrc)", data);
        if (ahcsrc) gst_object_unref (ahcsrc);
        if (capsfilter) gst_object_unref (capsfilter);
        if (videosink) gst_object_unref (videosink);
        close(dup_fd);
        return;
    }

    GstCaps *caps = gst_caps_from_string ("video/x-raw,width=640,height=480,framerate=30/1");
    g_object_set (G_OBJECT (capsfilter), "caps", caps, NULL);
    gst_caps_unref (caps);

    /* Force ahcsrc to use the provided fd, not any built‑in camera */
    g_object_set (G_OBJECT (ahcsrc), "fd", dup_fd, NULL);
    g_object_set (G_OBJECT (ahcsrc), "camera-index", -1, NULL);

    data->pipeline = gst_pipeline_new ("usb-pipeline");
    gst_bin_add_many (GST_BIN (data->pipeline), ahcsrc, capsfilter, videosink, NULL);
    if (!gst_element_link_many (ahcsrc, capsfilter, videosink, NULL)) {
        set_ui_message ("Failed to link FD pipeline (ahcsrc)", data);
        cleanup_pipeline (data);
        close(dup_fd);
        return;
    }

    data->video_sink = videosink;
    if (data->video_sink && data->native_window)
        gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->video_sink), (guintptr)data->native_window);

    GstBus *bus = gst_element_get_bus (data->pipeline);
    data->bus_source = gst_bus_create_watch (bus);
    g_source_set_callback (data->bus_source, (GSourceFunc) gst_bus_async_signal_func, NULL, NULL);
    g_source_attach (data->bus_source, data->context);
    g_signal_connect (bus, "message::error", (GCallback)error_cb, data);
    g_signal_connect (bus, "message::state-changed", (GCallback)state_changed_cb, data);
    gst_object_unref (bus);

    data->timeout_source = g_timeout_source_new (250);
    g_source_set_callback (data->timeout_source, (GSourceFunc)refresh_ui, data, NULL);
    g_source_attach (data->timeout_source, data->context);

    data->target_state = GST_STATE_PLAYING;
    gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    data->is_live = TRUE;
}

/* Thread‑safe switch helpers */
typedef struct { CustomData *data; gchar *uri; } UriSwitchData;
static gboolean switch_to_uri_cb (gpointer user_data) {
    UriSwitchData *ud = (UriSwitchData *) user_data;
    start_pipeline_with_uri (ud->data, ud->uri);
    g_free (ud->uri); g_free (ud);
    return G_SOURCE_REMOVE;
}

typedef struct { CustomData *data; gint camera_index; } CameraSwitchData;
static gboolean switch_to_camera_cb (gpointer user_data) {
    CameraSwitchData *cs = (CameraSwitchData *) user_data;
    start_pipeline_with_camera (cs->data, cs->camera_index);
    g_free (cs);
    return G_SOURCE_REMOVE;
}

typedef struct { CustomData *data; gchar *device; } V4L2SwitchData;
static gboolean switch_to_v4l2_cb (gpointer user_data) {
    V4L2SwitchData *vs = (V4L2SwitchData *) user_data;
    start_pipeline_with_v4l2 (vs->data, vs->device);
    g_free (vs->device);
    g_free (vs);
    return G_SOURCE_REMOVE;
}

typedef struct { CustomData *data; int fd; } FDSwitchData;
static gboolean switch_to_fd_cb (gpointer user_data) {
    FDSwitchData *fs = (FDSwitchData *) user_data;
    start_pipeline_with_fd (fs->data, fs->fd);
    close(fs->fd);
    g_free (fs);
    return G_SOURCE_REMOVE;
}

/* ---------- Main GStreamer thread ---------- */
static void *app_function (void *userdata) {
    CustomData *data = (CustomData *)userdata;
    data->context = g_main_context_new (); g_main_context_push_thread_default(data->context);
    data->pipeline = gst_parse_launch("playbin", NULL);
    if (!data->pipeline) { set_ui_message ("Could not create initial pipeline", data); return NULL; }
    gst_element_set_state (data->pipeline, GST_STATE_READY);
    GstBus *bus = gst_element_get_bus (data->pipeline);
    data->bus_source = gst_bus_create_watch (bus);
    g_source_set_callback (data->bus_source, (GSourceFunc) gst_bus_async_signal_func, NULL, NULL);
    g_source_attach (data->bus_source, data->context);
    g_signal_connect (bus, "message::error", (GCallback)error_cb, data);
    g_signal_connect (bus, "message::eos", (GCallback)eos_cb, data);
    g_signal_connect (bus, "message::state-changed", (GCallback)state_changed_cb, data);
    g_signal_connect (bus, "message::duration", (GCallback)duration_cb, data);
    g_signal_connect (bus, "message::buffering", (GCallback)buffering_cb, data);
    g_signal_connect (bus, "message::clock-lost", (GCallback)clock_lost_cb, data);
    gst_object_unref (bus);
    data->timeout_source = g_timeout_source_new (250);
    g_source_set_callback (data->timeout_source, (GSourceFunc)refresh_ui, data, NULL);
    g_source_attach (data->timeout_source, data->context);
    data->main_loop = g_main_loop_new (data->context, FALSE);
    check_initialization_complete (data);
    g_main_loop_run (data->main_loop);
    cleanup_pipeline (data);
    g_main_loop_unref (data->main_loop); data->main_loop = NULL;
    g_main_context_pop_thread_default(data->context); g_main_context_unref (data->context);
    return NULL;
}

static gboolean refresh_ui (CustomData *data) {
    gint64 position;
    if (!data || !data->pipeline || data->state < GST_STATE_PAUSED) return TRUE;
    if (!data->is_live && !GST_CLOCK_TIME_IS_VALID (data->duration))
        gst_element_query_duration (data->pipeline, GST_FORMAT_TIME, &data->duration);
    if (gst_element_query_position (data->pipeline, GST_FORMAT_TIME, &position))
        set_current_ui_position (position / GST_MSECOND, data->duration / GST_MSECOND, data);
    return TRUE;
}

/* ---------- Camera discovery ---------- */
typedef struct { jobject thiz; } DiscoveryData;
static gboolean do_camera_discovery (gpointer user_data) {
    DiscoveryData *ddata = (DiscoveryData *) user_data;
    JNIEnv *env = get_jni_env();
    GstDeviceMonitor *monitor = gst_device_monitor_new();
    GstCaps *caps = gst_caps_new_empty_simple("Video/Source");
    gst_device_monitor_add_filter(monitor, "Video/Source", caps); gst_caps_unref(caps);
    GList *devices, *l;
    if (gst_device_monitor_start(monitor)) {
        devices = gst_device_monitor_get_devices(monitor);
        for (l = devices; l != NULL; l = l->next) {
            GstDevice *device = GST_DEVICE(l->data);
            gchar *name = gst_device_get_display_name(device);
            gchar *path = NULL;
            GstStructure *props = gst_device_get_properties(device);
            if (props) {
                path = g_strdup(gst_structure_get_string(props, "device.path"));
                if (!path) path = g_strdup(gst_structure_get_string(props, "v4l2.device"));
                gst_structure_free(props);
            }
            if (!path) path = g_strdup("unknown");
            jstring jname = (*env)->NewStringUTF(env, name);
            jstring jpath = (*env)->NewStringUTF(env, path);
            (*env)->CallVoidMethod(env, ddata->thiz, on_camera_device_found_method_id, jname, jpath);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, jname); (*env)->DeleteLocalRef(env, jpath);
            g_free(name); g_free(path);
        }
        g_list_free_full(devices, gst_object_unref);
        gst_device_monitor_stop(monitor);
    }
    gst_object_unref(monitor);
    (*env)->DeleteGlobalRef(env, ddata->thiz); g_free(ddata);
    return G_SOURCE_REMOVE;
}

static void gst_native_start_camera_discovery (JNIEnv* env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA(env, thiz, custom_data_field_id);
    if (!data) return;
    DiscoveryData *ddata = g_new0(DiscoveryData, 1);
    ddata->thiz = (*env)->NewGlobalRef(env, thiz);
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, do_camera_discovery, ddata, NULL);
    g_source_attach(source, data->context); g_source_unref(source);
}

/* ---------- JNI exports ---------- */
static void gst_native_init (JNIEnv* env, jobject thiz) {
    CustomData *data = g_new0 (CustomData, 1);
    data->desired_position = GST_CLOCK_TIME_NONE; data->last_seek_time = GST_CLOCK_TIME_NONE;
    SET_CUSTOM_DATA (env, thiz, custom_data_field_id, data);
    GST_DEBUG_CATEGORY_INIT (debug_category, "tutorial-4", 0, "Android tutorial 4");
    data->app = (*env)->NewGlobalRef (env, thiz);
    pthread_create (&gst_app_thread, NULL, &app_function, data);
}

static void gst_native_finalize (JNIEnv* env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    if (data->main_loop) g_main_loop_quit (data->main_loop);
    pthread_join (gst_app_thread, NULL);
    (*env)->DeleteGlobalRef (env, data->app); g_free (data);
    SET_CUSTOM_DATA (env, thiz, custom_data_field_id, NULL);
}

static void gst_native_set_uri (JNIEnv* env, jobject thiz, jstring uri) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    const gchar *char_uri = (*env)->GetStringUTFChars (env, uri, NULL);
    UriSwitchData *ud = g_new (UriSwitchData, 1);
    ud->data = data; ud->uri = g_strdup (char_uri);
    (*env)->ReleaseStringUTFChars (env, uri, char_uri);
    GSource *source = g_idle_source_new ();
    g_source_set_callback (source, switch_to_uri_cb, ud, NULL);
    g_source_attach (source, data->context); g_source_unref (source);
}

static void gst_native_play_camera (JNIEnv* env, jobject thiz, jint cameraIndex) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    CameraSwitchData *cs = g_new (CameraSwitchData, 1);
    cs->data = data;
    cs->camera_index = cameraIndex;
    GSource *source = g_idle_source_new ();
    g_source_set_callback (source, switch_to_camera_cb, cs, NULL);
    g_source_attach (source, data->context);
    g_source_unref (source);
}

static void gst_native_play_camera_v4l2 (JNIEnv* env, jobject thiz, jstring device) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    const gchar *char_device = (*env)->GetStringUTFChars (env, device, NULL);
    V4L2SwitchData *vs = g_new (V4L2SwitchData, 1);
    vs->data = data;
    vs->device = g_strdup (char_device);
    (*env)->ReleaseStringUTFChars (env, device, char_device);
    GSource *source = g_idle_source_new ();
    g_source_set_callback (source, switch_to_v4l2_cb, vs, NULL);
    g_source_attach (source, data->context);
    g_source_unref (source);
}

static void gst_native_play_camera_fd (JNIEnv* env, jobject thiz, jint fd) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    FDSwitchData *fs = g_new (FDSwitchData, 1);
    fs->data = data;
    fs->fd = fd;
    GSource *source = g_idle_source_new ();
    g_source_set_callback (source, switch_to_fd_cb, fs, NULL);
    g_source_attach (source, data->context);
    g_source_unref (source);
}

static void gst_native_play (JNIEnv* env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    data->target_state = GST_STATE_PLAYING;
    gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    data->is_live = FALSE;
}

static void gst_native_pause (JNIEnv* env, jobject thiz) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    data->target_state = GST_STATE_PAUSED;
    gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
    data->is_live = FALSE;
}

static void gst_native_set_position (JNIEnv* env, jobject thiz, int milliseconds) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    gint64 desired_position = (gint64)(milliseconds * GST_MSECOND);
    if (data->state >= GST_STATE_PAUSED) execute_seek(desired_position, data);
    else data->desired_position = desired_position;
}

static void gst_native_surface_init (JNIEnv *env, jobject thiz, jobject surface) {
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data) return;
    ANativeWindow *new_win = ANativeWindow_fromSurface(env, surface);
    if (data->native_window) {
        ANativeWindow_release (data->native_window);
        if (data->native_window == new_win) {
            if (data->video_sink) gst_video_overlay_expose (GST_VIDEO_OVERLAY (data->video_sink));
            return;
        } else data->initialized = FALSE;
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
    data->native_window = NULL; data->initialized = FALSE;
}

/* ---------- v4l2src availability check (returns string) ---------- */
static jstring gst_native_check_v4l2 (JNIEnv* env, jobject thiz) {
    GstElement *v4l2src = gst_element_factory_make ("v4l2src", NULL);
    const gchar *result;
    if (v4l2src) {
        result = "v4l2src available";
        gst_object_unref(v4l2src);
    } else {
        result = "v4l2src NOT available";
    }
    return (*env)->NewStringUTF(env, result);
}

static jstring gst_native_get_version (JNIEnv* env, jobject thiz) {
    char *ver = gst_version_string ();
    jstring jver = (*env)->NewStringUTF (env, ver);
    g_free (ver); return jver;
}

static jboolean gst_native_class_init (JNIEnv* env, jclass klass) {
    custom_data_field_id = (*env)->GetFieldID (env, klass, "native_custom_data", "J");
    set_message_method_id = (*env)->GetMethodID (env, klass, "setMessage", "(Ljava/lang/String;)V");
    set_current_position_method_id = (*env)->GetMethodID (env, klass, "setCurrentPosition", "(II)V");
    on_gstreamer_initialized_method_id = (*env)->GetMethodID (env, klass, "onGStreamerInitialized", "()V");
    on_media_size_changed_method_id = (*env)->GetMethodID (env, klass, "onMediaSizeChanged", "(II)V");
    on_camera_device_found_method_id = (*env)->GetMethodID (env, klass, "onCameraDeviceFound", "(Ljava/lang/String;Ljava/lang/String;)V");

    if (!custom_data_field_id || !set_message_method_id || !on_gstreamer_initialized_method_id ||
        !on_media_size_changed_method_id || !set_current_position_method_id || !on_camera_device_found_method_id)
        return JNI_FALSE;
    return JNI_TRUE;
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
        { "nativeGetVersion", "()Ljava/lang/String;", (void *) gst_native_get_version},
        { "nativeCheckV4L2", "()Ljava/lang/String;", (void *) gst_native_check_v4l2},
        { "nativeStartCameraDiscovery", "()V", (void *) gst_native_start_camera_discovery},
        { "nativePlayCamera", "(I)V", (void *) gst_native_play_camera},
        { "nativePlayCameraV4L2", "(Ljava/lang/String;)V", (void *) gst_native_play_camera_v4l2},
        { "nativePlayCameraFD", "(I)V", (void *) gst_native_play_camera_fd},
        { "nativeClassInit", "()Z", (void *) gst_native_class_init}
};

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = NULL; java_vm = vm;
    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) return 0;
    jclass klass = (*env)->FindClass (env, "com/example/gstreamerandroid/MainActivity");
    if ((*env)->RegisterNatives(env, klass, native_methods, G_N_ELEMENTS(native_methods)) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "JNI", "Failed to register natives");
        return 0;
    }
    pthread_key_create (&current_jni_env, detach_current_thread);
    return JNI_VERSION_1_4;
}