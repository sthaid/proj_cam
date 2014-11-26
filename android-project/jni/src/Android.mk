LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := main

SDL_PATH := ../SDL2-2.0.3
SDL_TTF_PATH := ../SDL2_ttf-2.0.12
JPEG_PATH := ../jpeg8d

LOCAL_CFLAGS += -DANDROID

LOCAL_C_INCLUDES := $(LOCAL_PATH)/$(SDL_PATH)/include \
                    $(LOCAL_PATH)/$(SDL_TTF_PATH) \
                    $(LOCAL_PATH)/$(JPEG_PATH)

LOCAL_SRC_FILES := $(SDL_PATH)/src/main/android/SDL_android_main.c \
	           test.c util.c jpeg_decode.c

LOCAL_SHARED_LIBRARIES := SDL2 SDL2_ttf jpeg

LOCAL_LDLIBS := -lGLESv1_CM -lGLESv2 -llog

include $(BUILD_SHARED_LIBRARY)
