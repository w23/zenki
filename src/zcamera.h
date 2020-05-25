#pragma once

typedef struct AVFrame AVFrame;

typedef void (*ZCameraKeyframeTestFunc)(void *user, int format, const AVFrame *frame);

typedef struct {
	const char *source_url;
	ZCameraKeyframeTestFunc test_func;
	void *user;
} ZCameraParams;

typedef struct ZCamera ZCamera;

ZCamera *zCameraCreate(ZCameraParams);
int zCameraPollPacket(ZCamera*);
void zCameraDestroy(ZCamera*);
