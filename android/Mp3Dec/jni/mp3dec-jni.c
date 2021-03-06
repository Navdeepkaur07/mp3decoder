/******************************************************************************
*
* JNI wrapper for calling the C functions of the decoder
*
******************************************************************************/

#include <string.h>
#include <jni.h>
#include <android/log.h>

#include <stdio.h>
#include <wchar.h>
#include "app.h"

#define MP3DEC_TAG    "[MP3DEC-JNI]"

jstring Java_com_wb_mp3dec_Mp3Dec_Version( JNIEnv* env, jobject thiz ) {
    __android_log_write(ANDROID_LOG_INFO, MP3DEC_TAG, "-- Mp3Decoder 1.0 --");
    return (*env)->NewStringUTF(env, "Mp3Decoder 1.0");
}

//jint Java_com_wb_mp3dec_Mp3Dec_Decode( JNIEnv* env, jobject thiz, jstring srcpath, jstring despath) {
void Java_com_wb_mp3dec_SoundAnalysisService_Decode( JNIEnv* env, jobject thiz, jstring srcpath, jstring despath) {
//void Java_com_wb_mp3dec_Mp3Dec_Decode( JNIEnv* env, jobject thiz, jstring srcpath, jstring despath) {

    int ret = 0;
    // compose native strings
    const char* native_srcpath = (*env)->GetStringUTFChars(env, srcpath, 0);
    const char* native_despath = (*env)->GetStringUTFChars(env, despath, 0);

    extern double avg_jitter, avg_weak_note, avg_excess_note;
    extern double avg_phi_rels_cnt, avg_oct_rels_cnt, avg_fourth_rels_cnt, avg_fifth_rels_cnt;


    jclass cls = (*env)->GetObjectClass(env, thiz);
    jmethodID mid = (*env)->GetMethodID(env, cls, "gotSoundAnalysisResults", "(IDDDDDDD)V");

    // msg
    char msg[1024];
    strcpy(msg, " -- decoding: ");
    strcat(msg, native_srcpath);
    strcat(msg, " --> ");
    strcat(msg, native_despath);

    __android_log_write(ANDROID_LOG_INFO, MP3DEC_TAG, msg);

    ret = decoder_lib_main(native_srcpath, native_despath);
    if(ret < 0) {
    __android_log_write(ANDROID_LOG_INFO, MP3DEC_TAG, "-- ERROR WHILE DECODING --");
    }

    // release allocated string
    (*env)->ReleaseStringUTFChars(env, srcpath, native_srcpath);
    (*env)->ReleaseStringUTFChars(env, despath, native_despath);


    /*ideally this should never happen*/
    if (mid == 0) {
        __android_log_write(ANDROID_LOG_INFO, MP3DEC_TAG, "--  failed to get java callback -> gotSoundAnalysisResults --");
        return;
    }

    /*call java method*/
    (*env)->CallVoidMethod(env, thiz, mid, ret, avg_jitter, avg_weak_note, avg_excess_note,  avg_phi_rels_cnt, 
            avg_oct_rels_cnt, avg_fourth_rels_cnt, avg_fifth_rels_cnt);
}
