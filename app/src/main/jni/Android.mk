LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := tutorial-1
LOCAL_SRC_FILES := tutorial-1.c
LOCAL_LDLIBS    := -llog

include $(BUILD_SHARED_LIBRARY)