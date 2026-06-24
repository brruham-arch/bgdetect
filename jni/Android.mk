LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE     := bgdetect
LOCAL_SRC_FILES  := ../mod/main.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../include/AML
LOCAL_CPPFLAGS   := -std=c++17 -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fPIC
LOCAL_LDLIBS     := -llog -ldl
LOCAL_LDFLAGS    := -static-libstdc++ -Wl,--gc-sections
include $(BUILD_SHARED_LIBRARY)
