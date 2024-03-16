//
// Copyright (c) 2019-2022 yanggaofeng
//
#include "YangCameraAndroid.h"

#include <assert.h>
#include <jni.h>
#include <thread>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <queue>

#include <android/log.h>

#include <android/native_window_jni.h>

#include <android/log.h>

#define LOG_ENABLE

#define LOG_TAG "JBIG_KIT"//这是tag的名字

#ifdef LOG_ENABLE

#undef LOG
#define ALOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)
#define ALOGI(...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)
#define LOGW(...)  __android_log_print(ANDROID_LOG_WARN,LOG_TAG,__VA_ARGS__)
#define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,__VA_ARGS__)
#define LOGF(...)  __android_log_print(ANDROID_LOG_FATAL,LOG_TAG,__VA_ARGS__)
#else
#define LOGD(...)
#define LOGI(...)
#define LOGW(...)
#define LOGE(...)
#define LOGF(...)
#endif

void printCamProps(ACameraManager *cameraManager, const char *id)
{
    // exposure range
    ACameraMetadata *metadataObj;
    ACameraManager_getCameraCharacteristics(cameraManager, id, &metadataObj);

    ACameraMetadata_const_entry entry = {0};

    // cam facing
    ACameraMetadata_getConstEntry(metadataObj,
                                  ACAMERA_SENSOR_ORIENTATION, &entry);

    int32_t orientation = entry.data.i32[0];
	LOGE("camProps: %d", orientation);
}
 std::string getBackFacingCamId(ACameraManager *cameraManager)
 {
     ACameraIdList *cameraIds = nullptr;
     ACameraManager_getCameraIdList(cameraManager, &cameraIds);

     std::string backId;

	 LOGE("found camera count %d", cameraIds->numCameras);

     for (int i = 0; i < cameraIds->numCameras; ++i)
     {
         const char *id = cameraIds->cameraIds[i];

         ACameraMetadata *metadataObj;
         ACameraManager_getCameraCharacteristics(cameraManager, id, &metadataObj);

         ACameraMetadata_const_entry lensInfo = {0};
         ACameraMetadata_getConstEntry(metadataObj, ACAMERA_LENS_FACING, &lensInfo);

         auto facing = static_cast<acamera_metadata_enum_android_lens_facing_t>(
                 lensInfo.data.u8[0]);

         // Found a back-facing camera?
         if (facing == ACAMERA_LENS_FACING_BACK)
         {
             backId = id;
             break;
         }
     }

     ACameraManager_deleteCameraIdList(cameraIds);

     return backId;
 }

#include <fcntl.h>

typedef void (*euroc_player)(int planeIdx,
        /*out*/uint8_t** data, /*out*/int* dataLength);

 void imageCallback(void* context, AImageReader* reader)
  {
	  static int cctime;
	  cctime++;
	 AImage *image = nullptr;
	 media_status_t status = AImageReader_acquireNextImage(reader, &image);

     uint8_t *data = nullptr;
     int len = 0;
     AImage_getPlaneData(image, 0, &data, &len);//yyyyyyyyyyyyyyyyyyyyyyyyyy

    // 转换回原来的函数指针类型，并调用：
     euroc_player func = reinterpret_cast<euroc_player>(context);
     func(0, &data, &len);

     LOGE("imageCallbacklen=%d  cctime=%d", len, cctime);

     if(cctime == 10){
         char buf[FILENAME_MAX] = "/storage/emulated/0/Android/data/org.freedesktop.monado.openxr_runtime.out_of_process/files/c906.yuv420888";

         int file_fd = open(buf, O_RDWR | O_CREAT, 0644);
         if (file_fd >= 0 && len > 0) {
             ssize_t written_len = write(file_fd, data, len);
             LOGE("written number of bytes %zd to %s", written_len, buf);
             close(file_fd);
         } else {
             LOGE("failed to open file %s to dump image", buf);
         }
     }


     AImage_delete(image);

	 // Try to process data without blocking the callback
//	 std::thread processor([=](){

//	 });
//	 processor.detach();
  }

  AImageReader* g_yang_createReader(void* user,int width,int height)
  {
	  AImageReader* reader = nullptr;
	  //AIMAGE_FORMAT_YUV_420_888 AIMAGE_FORMAT_JPEG
	  media_status_t status = AImageReader_new(width, height, AIMAGE_FORMAT_YUV_420_888,4, &reader);

	  AImageReader_ImageListener listener;
	  listener.context=user;
	  listener.onImageAvailable=imageCallback;
	  AImageReader_setImageListener(reader, &listener);

	  return reader;
  }

  ANativeWindow* g_yang_createSurface(AImageReader* reader)
  {
      ANativeWindow *nativeWindow;
      AImageReader_getWindow(reader, &nativeWindow);

      return nativeWindow;
  }


void camera_device_on_disconnected(void *context, ACameraDevice *device) {
	LOGE("Camera(id: %s) is diconnected.\n", ACameraDevice_getId(device));
}

void camera_device_on_error(void *context, ACameraDevice *device, int error) {
	LOGE("Error(code: %d) on Camera(id: %s).\n", error, ACameraDevice_getId(device));
}

void capture_session_on_ready(void *context, ACameraCaptureSession *session) {
	LOGE("Session is ready. %p\n", session);
}

void capture_session_on_active(void *context, ACameraCaptureSession *session) {
	LOGE("Session is activated. %p\n", session);
}

void capture_session_on_closed(void *context, ACameraCaptureSession *session) {
	LOGE("Session is closed. %p\n", session);
}

/**
 * Capture callbacks
 */

void onCaptureFailed(void* context, ACameraCaptureSession* session,
                     ACaptureRequest* request, ACameraCaptureFailure* failure)
{
	LOGE("onCaptureFailed ");
}

void onCaptureSequenceCompleted(void* context, ACameraCaptureSession* session,
                                int sequenceId, int64_t frameNumber)
{
	LOGE("onCaptureSequenceCompleted ");

}


void onCaptureSequenceAborted(void* context, ACameraCaptureSession* session,
                              int sequenceId)
{
	LOGE("Capture Aborted");

}

void on_captureCallback_start(
		void* context, ACameraCaptureSession* session,
		const ACaptureRequest* request, int64_t timestamp){
	LOGE("Capture start");

}

void onCaptureCompleted (
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, const ACameraMetadata* result)
{
    LOGE("Capture completed");
}


YangCameraAndroid::YangCameraAndroid(/*ANativeWindow* pwindow*/){
//	m_window=pwindow;

	m_cameraManager=NULL;
	m_cameraDevice=NULL;
	m_captureRequest=NULL;
	m_cameraOutputTarget=NULL;
//	m_sessionOutput=NULL;
	m_captureSessionOutputContainer=NULL;
	m_captureSession=NULL;

	m_imageReader=NULL;
	m_imageWindow=NULL;
	m_imageTarget=NULL;
	m_imageOutput=NULL;

	m_width=640;
	m_height=480;
	m_user=NULL;
}
YangCameraAndroid::~YangCameraAndroid(){
	closeCamera();
}

void YangCameraAndroid::setSize(int width,int height){
	m_width=width;
	m_height=height;
}
void YangCameraAndroid::setUser(void* user){
	m_user=user;
}

void YangCameraAndroid::initCamera(){

	ACameraManager *cameraManager = ACameraManager_create();

	std::string id = getBackFacingCamId(cameraManager);

	m_deviceStateCallbacks.onDisconnected = camera_device_on_disconnected;
	m_deviceStateCallbacks.onError = camera_device_on_error;
	m_deviceStateCallbacks.context=this;

	ACameraManager_openCamera(cameraManager, id.c_str(), &m_deviceStateCallbacks, &m_cameraDevice);
	printCamProps(cameraManager, id.c_str());

	ACameraDevice_createCaptureRequest(m_cameraDevice, TEMPLATE_PREVIEW, &m_captureRequest);


	// Prepare outputs for session
//	ACaptureSessionOutput_create(m_window, &m_sessionOutput);
	ACaptureSessionOutputContainer_create(&m_captureSessionOutputContainer);
//	ACaptureSessionOutputContainer_add(m_captureSessionOutputContainer, m_sessionOutput);

	m_imageReader = g_yang_createReader(m_user,m_width,m_height);
	m_imageWindow = g_yang_createSurface(m_imageReader);
	ANativeWindow_acquire(m_imageWindow);
	ACameraOutputTarget_create(m_imageWindow, &m_imageTarget);
	ACaptureRequest_addTarget(m_captureRequest, m_imageTarget);
	ACaptureSessionOutput_create(m_imageWindow, &m_imageOutput);
	ACaptureSessionOutputContainer_add(m_captureSessionOutputContainer, m_imageOutput);


	// Prepare target surface
//	ANativeWindow_acquire(m_window);
//	ACameraOutputTarget_create(m_window, &m_cameraOutputTarget);
//	ACaptureRequest_addTarget(m_captureRequest, m_cameraOutputTarget);

	m_captureSessionStateCallbacks.onReady = capture_session_on_ready;
	m_captureSessionStateCallbacks.onActive = capture_session_on_active;
	m_captureSessionStateCallbacks.onClosed = capture_session_on_closed;

	// Create the session
	ACameraDevice_createCaptureSession(m_cameraDevice, m_captureSessionOutputContainer, &m_captureSessionStateCallbacks, &m_captureSession);


	m_captureSessionCaptureCallbacks.context = nullptr,
	m_captureSessionCaptureCallbacks.onCaptureStarted = on_captureCallback_start,
	m_captureSessionCaptureCallbacks.onCaptureProgressed = nullptr,
	m_captureSessionCaptureCallbacks.onCaptureCompleted = onCaptureCompleted,
	m_captureSessionCaptureCallbacks.onCaptureFailed = onCaptureFailed,
	m_captureSessionCaptureCallbacks.onCaptureSequenceCompleted = onCaptureSequenceCompleted,
	m_captureSessionCaptureCallbacks.onCaptureSequenceAborted = onCaptureSequenceAborted,
	m_captureSessionCaptureCallbacks.onCaptureBufferLost = nullptr,
	// Start capturing continuously
	ACameraCaptureSession_setRepeatingRequest(m_captureSession, &m_captureSessionCaptureCallbacks, 1, &m_captureRequest, nullptr);



}

 void YangCameraAndroid::closeCamera(void)
{
	 if (m_cameraManager)
	    {
	        // Stop recording to SurfaceTexture and do some cleanup
	        ACameraCaptureSession_stopRepeating(m_captureSession);
	        ACameraCaptureSession_close(m_captureSession);
	        ACaptureSessionOutputContainer_free(m_captureSessionOutputContainer);
//	        ACaptureSessionOutput_free(m_sessionOutput);

	        ACameraDevice_close(m_cameraDevice);
	        ACameraManager_delete(m_cameraManager);
	        m_cameraManager = nullptr;


	        AImageReader_delete(m_imageReader);
	        m_imageReader = nullptr;

	        // Capture request for SurfaceTexture
//	        ANativeWindow_release(m_window);
	        ACaptureRequest_free(m_captureRequest);

	}
	LOGE("Close Camera\n");
}


