/*
 * COMPREHENSIVE JNI STUBS FOR ZOMBIEWOOD
 * 
 * All ~100 JNI methods discovered in libZombieWood.so
 * Each logs its invocation and returns success/neutral values
 * 
 * Generated from ELF analysis of libZombieWood.so
 */

#ifndef JNI_STUBS_H
#define JNI_STUBS_H

#include <jni.h>
#include <stdio.h>

static FILE *g_jni_log_file = NULL;

#define JNI_LOG(fmt, ...) \
    do { \
        if (g_jni_log_file) { \
            fprintf(g_jni_log_file, "[JNI] " fmt "\n", ##__VA_ARGS__); \
            fflush(g_jni_log_file); \
        } \
    } while(0)

void jni_log_init(FILE *f) { g_jni_log_file = f; }

/* ========== INSTALLER / DRM (CRITICAL) ========== */
jint Java_com_gameloft_android_ANMP_GloftZOHM_installer_GameInstaller_initNative(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameInstaller_initNative()");
    return 1;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_installer_GameInstaller_nativeGetDevID(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameInstaller_nativeGetDevID()");
    return 12345; /* fake device ID */
}

jstring Java_com_gameloft_android_ANMP_GloftZOHM_installer_GameInstaller_getPublicKey(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameInstaller_getPublicKey()");
    return (jstring)"fake_key";  /* Return dummy pointer */
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_installer_GameInstaller_nativeStart(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameInstaller_nativeStart()");
    return 1;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_installer_GDRMPolicy_initNativeAP(JNIEnv *env, jobject thiz) {
    JNI_LOG("GDRMPolicy_initNativeAP()");
    return 1;
}

jstring Java_com_gameloft_android_ANMP_GloftZOHM_installer_GDRMPolicy_getConstString(JNIEnv *env, jobject thiz, jint index) {
    JNI_LOG("GDRMPolicy_getConstString(index=%d)", index);
    return (jstring)"const_value";  /* Return dummy pointer */
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_installer_GDRMPolicy_setConst(JNIEnv *env, jobject thiz, jint index, jstring value) {
    JNI_LOG("GDRMPolicy_setConst(index=%d)", index);
    return 1;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_installer_GDRMPolicy_setLongConst(JNIEnv *env, jobject thiz, jint index, jlong value) {
    JNI_LOG("GDRMPolicy_setLongConst(index=%d)", index);
    return 1;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_installer_GDRMPolicy_setTime(JNIEnv *env, jobject thiz, jlong time) {
    JNI_LOG("GDRMPolicy_setTime()");
    return 1;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_installer_GDRMPolicy_processServer(JNIEnv *env, jobject thiz, jstring response) {
    JNI_LOG("GDRMPolicy_processServer()");
    return 1;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_installer_GDRMPolicy_nativeAllow(JNIEnv *env, jobject thiz) {
    JNI_LOG("GDRMPolicy_nativeAllow() - DRM BYPASS");
    return 1;
}

/* ========== GAME LIFECYCLE ========== */
jint Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_init(JNIEnv *env, jobject thiz) {
    JNI_LOG("GL2JNILib_init()");
    return 1;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_initGL(JNIEnv *env, jobject thiz) {
    JNI_LOG("GL2JNILib_initGL()");
    return 1;
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_pause(JNIEnv *env, jobject thiz) {
    JNI_LOG("GL2JNILib_pause()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_resume(JNIEnv *env, jobject thiz) {
    JNI_LOG("GL2JNILib_resume()");
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_step(JNIEnv *env, jobject thiz) {
    JNI_LOG("GL2JNILib_step()");
    return 1;
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_destroy(JNIEnv *env, jobject thiz) {
    JNI_LOG("GL2JNILib_destroy()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_resize(JNIEnv *env, jobject thiz, jint w, jint h) {
    JNI_LOG("GL2JNILib_resize(%d, %d)", w, h);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_setPaths(JNIEnv *env, jobject thiz, jstring path) {
    JNI_LOG("GL2JNILib_setPaths()");
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_setNumExtraContext(JNIEnv *env, jobject thiz, jint num) {
    JNI_LOG("GL2JNILib_setNumExtraContext(%d)", num);
    return 1;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_getNumExtraContext(JNIEnv *env, jobject thiz) {
    JNI_LOG("GL2JNILib_getNumExtraContext()");
    return 1;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_getViewSettings(JNIEnv *env, jobject thiz) {
    JNI_LOG("GL2JNILib_getViewSettings()");
    return 0;
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_touchEvent(JNIEnv *env, jobject thiz, jint x, jint y, jint type) {
    JNI_LOG("GL2JNILib_touchEvent(%d, %d, %d)", x, y, type);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_stateChanged(JNIEnv *env, jobject thiz, jint state) {
    JNI_LOG("GL2JNILib_stateChanged(%d)", state);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_orientationChanged(JNIEnv *env, jobject thiz, jint orientation) {
    JNI_LOG("GL2JNILib_orientationChanged(%d)", orientation);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_accelerometerEvent(JNIEnv *env, jobject thiz, jfloat x, jfloat y, jfloat z) {
    JNI_LOG("GL2JNILib_accelerometerEvent(%.2f, %.2f, %.2f)", x, y, z);
}

/* ========== GAME EVENTS ========== */
void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("Game_nativeInit() - GAME ENGINE START");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeOnTouch(JNIEnv *env, jobject thiz, jint x, jint y, jint action) {
    JNI_LOG("Game_nativeOnTouch(%d, %d, %d)", x, y, action);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeOnKeyDown(JNIEnv *env, jobject thiz, jint keycode) {
    JNI_LOG("Game_nativeOnKeyDown(%d)", keycode);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeOnKeyUp(JNIEnv *env, jobject thiz, jint keycode) {
    JNI_LOG("Game_nativeOnKeyUp(%d)", keycode);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeOrientation(JNIEnv *env, jobject thiz, jint orientation) {
    JNI_LOG("Game_nativeOrientation(%d)", orientation);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeAccelerator(JNIEnv *env, jobject thiz, jfloat x, jfloat y, jfloat z) {
    JNI_LOG("Game_nativeAccelerator(%.2f, %.2f, %.2f)", x, y, z);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeConnectionLoss(JNIEnv *env, jobject thiz) {
    JNI_LOG("Game_nativeConnectionLoss()");
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeCanInterrupt(JNIEnv *env, jobject thiz) {
    JNI_LOG("Game_nativeCanInterrupt()");
    return 0;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeCanAutoOrientate(JNIEnv *env, jobject thiz) {
    JNI_LOG("Game_nativeCanAutoOrientate()");
    return 1;
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeVoxSuspend(JNIEnv *env, jobject thiz) {
    JNI_LOG("Game_nativeVoxSuspend()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeVoxResume(JNIEnv *env, jobject thiz) {
    JNI_LOG("Game_nativeVoxResume()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeVoxMute(JNIEnv *env, jobject thiz, jint mute) {
    JNI_LOG("Game_nativeVoxMute(%d)", mute);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeVoxTutorialIntro(JNIEnv *env, jobject thiz, jint intro) {
    JNI_LOG("Game_nativeVoxTutorialIntro(%d)", intro);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeShortcutToScreens(JNIEnv *env, jobject thiz, jint screen) {
    JNI_LOG("Game_nativeShortcutToScreens(%d)", screen);
}

jstring Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeGetInfo(JNIEnv *env, jobject thiz) {
    JNI_LOG("Game_nativeGetInfo()");
    return (jstring)"{\"status\":\"ok\"}";  /* Return dummy pointer */
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeAddCash(JNIEnv *env, jobject thiz, jint amount) {
    JNI_LOG("Game_nativeAddCash(%d)", amount);
}

/* ========== DEVICE INFO ========== */
void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeSetCPUInfo(JNIEnv *env, jobject thiz, jstring info) {
    JNI_LOG("Game_nativeSetCPUInfo()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeSetGPUInfo(JNIEnv *env, jobject thiz, jstring info) {
    JNI_LOG("Game_nativeSetGPUInfo()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeSetPhoneModel(JNIEnv *env, jobject thiz, jstring model) {
    JNI_LOG("Game_nativeSetPhoneModel()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeSetIMEI(JNIEnv *env, jobject thiz, jstring imei) {
    JNI_LOG("Game_nativeSetIMEI()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeXperiaToggleJoysticks(JNIEnv *env, jobject thiz, jint toggle) {
    JNI_LOG("Game_nativeXperiaToggleJoysticks(%d)", toggle);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_Game_processTouchpadAsPointer(JNIEnv *env, jobject thiz, jint process) {
    JNI_LOG("Game_processTouchpadAsPointer(%d)", process);
}

/* ========== RENDERER ========== */
jint Java_com_gameloft_android_ANMP_GloftZOHM_GameRenderer_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameRenderer_nativeInit()");
    return 1;
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GameRenderer_nativeRender(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameRenderer_nativeRender()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GameRenderer_nativeResize(JNIEnv *env, jobject thiz, jint w, jint h) {
    JNI_LOG("GameRenderer_nativeResize(%d, %d)", w, h);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GameRenderer_nativeDestroy(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameRenderer_nativeDestroy()");
}

/* ========== GLSURFACE ========== */
void Java_com_gameloft_android_ANMP_GloftZOHM_GameGLSurfaceView_nativeOnTouch(JNIEnv *env, jobject thiz, jint x, jint y, jint action) {
    JNI_LOG("GameGLSurfaceView_nativeOnTouch(%d, %d, %d)", x, y, action);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GameGLSurfaceView_nativePause(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameGLSurfaceView_nativePause()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GameGLSurfaceView_nativeResume(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameGLSurfaceView_nativeResume()");
}

/* ========== IN-APP BILLING ========== */
jint Java_com_gameloft_android_ANMP_GloftZOHM_iab_InAppBilling_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("InAppBilling_nativeInit()");
    return 1;
}

void Java_com_gameloft_android_ANMP_GloftZOHM_iab_InAppBilling_nativeSetContext(JNIEnv *env, jobject thiz, jobject context) {
    JNI_LOG("InAppBilling_nativeSetContext()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_iab_InAppBilling_nativeSetIABObject(JNIEnv *env, jobject thiz, jobject obj) {
    JNI_LOG("InAppBilling_nativeSetIABObject()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_iab_InAppBilling_nativeSendData(JNIEnv *env, jobject thiz, jstring data) {
    JNI_LOG("InAppBilling_nativeSendData()");
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_iab_s_bq(JNIEnv *env, jobject thiz) {
    JNI_LOG("iab_s_bq()");
    return 0;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_iab_s_gk(JNIEnv *env, jobject thiz) {
    JNI_LOG("iab_s_gk()");
    return 0;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_iab_s_gn(JNIEnv *env, jobject thiz) {
    JNI_LOG("iab_s_gn()");
    return 0;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_iab_s_in(JNIEnv *env, jobject thiz) {
    JNI_LOG("iab_s_in()");
    return 0;
}

jint Java_com_gameloft_android_ANMP_GloftZOHM_iab_s_rn(JNIEnv *env, jobject thiz) {
    JNI_LOG("iab_s_rn()");
    return 0;
}

/* ========== SOCIAL ========== */
jint Java_com_gameloft_GLSocialLib_PlatformAndroid_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("GLSocialLib_PlatformAndroid_nativeInit()");
    return 1;
}

jint Java_com_gameloft_GLSocialLib_facebook_FacebookAndroidGLSocialLib_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("FacebookAndroidGLSocialLib_nativeInit()");
    return 1;
}

void Java_com_gameloft_GLSocialLib_facebook_FacebookAndroidGLSocialLib_nativeOnFBDataLoad(JNIEnv *env, jobject thiz) {
    JNI_LOG("FacebookAndroidGLSocialLib_nativeOnFBDataLoad()");
}

void Java_com_gameloft_GLSocialLib_facebook_FacebookAndroidGLSocialLib_nativeOnFBDialogDidComplete(JNIEnv *env, jobject thiz) {
    JNI_LOG("FacebookAndroidGLSocialLib_nativeOnFBDialogDidComplete()");
}

void Java_com_gameloft_GLSocialLib_facebook_FacebookAndroidGLSocialLib_nativeOnFBDialogDidNotComplete(JNIEnv *env, jobject thiz) {
    JNI_LOG("FacebookAndroidGLSocialLib_nativeOnFBDialogDidNotComplete()");
}

void Java_com_gameloft_GLSocialLib_facebook_FacebookAndroidGLSocialLib_nativeOnFBFailWithError(JNIEnv *env, jobject thiz) {
    JNI_LOG("FacebookAndroidGLSocialLib_nativeOnFBFailWithError()");
}

jint Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameAPIAndroidGLSocialLib_nativeInit()");
    return 1;
}

void Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeGameAPIComplete(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameAPIAndroidGLSocialLib_nativeGameAPIComplete()");
}

void Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeGameAPICompleteWithData(JNIEnv *env, jobject thiz, jstring data) {
    JNI_LOG("GameAPIAndroidGLSocialLib_nativeGameAPICompleteWithData()");
}

void Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeGameAPIDidNotComplete(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameAPIAndroidGLSocialLib_nativeGameAPIDidNotComplete()");
}

void Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeGameAPIDidPlusOneButton(JNIEnv *env, jobject thiz) {
    JNI_LOG("GameAPIAndroidGLSocialLib_nativeGameAPIDidPlusOneButton()");
}

jint Java_com_gameloft_GLSocialLib_renren_RenrenAndroidGLSocialLib_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("RenrenAndroidGLSocialLib_nativeInit()");
    return 1;
}

void Java_com_gameloft_GLSocialLib_renren_RenrenAndroidGLSocialLib_nativeOnRRDataLoad(JNIEnv *env, jobject thiz) {
    JNI_LOG("RenrenAndroidGLSocialLib_nativeOnRRDataLoad()");
}

void Java_com_gameloft_GLSocialLib_renren_RenrenAndroidGLSocialLib_nativeOnRRDialogDidComplete(JNIEnv *env, jobject thiz) {
    JNI_LOG("RenrenAndroidGLSocialLib_nativeOnRRDialogDidComplete()");
}

void Java_com_gameloft_GLSocialLib_renren_RenrenAndroidGLSocialLib_nativeOnRRDialogDidNotComplete(JNIEnv *env, jobject thiz) {
    JNI_LOG("RenrenAndroidGLSocialLib_nativeOnRRDialogDidNotComplete()");
}

void Java_com_gameloft_GLSocialLib_renren_RenrenAndroidGLSocialLib_nativeOnRRFailWithError(JNIEnv *env, jobject thiz) {
    JNI_LOG("RenrenAndroidGLSocialLib_nativeOnRRFailWithError()");
}

/* ========== PUSH NOTIFICATIONS ========== */
jint Java_com_gameloft_android_ANMP_GloftZOHM_PushNotification_C2DMAndroidUtils_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("C2DMAndroidUtils_nativeInit()");
    return 1;
}

void Java_com_gameloft_android_ANMP_GloftZOHM_PushNotification_C2DMAndroidUtils_nativeCallBack(JNIEnv *env, jobject thiz, jstring data) {
    JNI_LOG("C2DMAndroidUtils_nativeCallBack()");
}

/* ========== DATA SHARING ========== */
jint Java_com_gameloft_android_ANMP_GloftZOHM_DataSharing_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("DataSharing_nativeInit()");
    return 1;
}

/* ========== UTILITIES ========== */
void Java_com_gameloft_android_ANMP_GloftZOHM_GLUtils_Device_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("GLUtils_Device_nativeInit()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GLUtils_SUtils_nativeInit(JNIEnv *env, jobject thiz) {
    JNI_LOG("GLUtils_SUtils_nativeInit()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GLUtils_SUtils_onVideoFinished(JNIEnv *env, jobject thiz) {
    JNI_LOG("GLUtils_SUtils_onVideoFinished()");
}

void Java_com_gameloft_android_ANMP_GloftZOHM_GLUtils_NetworkStateReceiver_setConnectionType(JNIEnv *env, jobject thiz, jint type) {
    JNI_LOG("GLUtils_NetworkStateReceiver_setConnectionType(%d)", type);
}

void Java_com_gameloft_android_ANMP_GloftZOHM_SendInfo_initMethods(JNIEnv *env, jobject thiz) {
    JNI_LOG("SendInfo_initMethods()");
}

#endif /* JNI_STUBS_H */
