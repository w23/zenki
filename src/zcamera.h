#pragma once

typedef struct AVFrame AVFrame;

//typedef void (*ZCameraKeyframeTestFunc)(void *user, int format, const AVFrame *frame);

typedef struct {
	//int sync; // this camera will be synchronous
	const char *name_m3u8;
	const char *source_url;
	//ZCameraKeyframeTestFunc test_func;
	//void *user;
	struct {
		const char *m3u8_path;
		const char *hls_time;
		const char *hls_list_size;
		const char *hls_base_url;
		const char *hls_segment_filename;
	} hls;
} ZCameraParams;

typedef struct ZCamera ZCamera;

ZCamera *zCameraCreate(ZCameraParams);
//int zCameraPollPacket(ZCamera*);
void zCameraDestroy(ZCamera*);
