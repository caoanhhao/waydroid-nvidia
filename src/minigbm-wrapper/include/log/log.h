/* NDK shim for the liblog macros used by gbm_mesa_wrapper.cpp. */
#pragma once
#include <android/log.h>

#ifndef LOG_TAG
#define LOG_TAG NULL
#endif

#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#if defined(LOG_NDEBUG) && LOG_NDEBUG == 0
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#else
#define ALOGV(...) ((void)0)
#endif
