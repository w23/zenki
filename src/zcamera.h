#pragma once

typedef struct AVFrame AVFrame;
typedef struct AVDictionary AVDictionary;

//typedef void (*ZCameraKeyframeTestFunc)(void *user, int format, const AVFrame *frame);

typedef struct {
	char *name;
	char *input_url;
	char *live_format;
	char *live_url;
	AVDictionary *live_options;
	float detect_threshold;
	char *detect_thumbnail;
	char *detect_output;
	char *detect_logfile;
} ConfigCamera;

typedef struct {
	// is expected to be static/outlive the camera
	const ConfigCamera *config;
} ZCameraParams;

typedef struct ZCamera ZCamera;

ZCamera *zCameraCreate(ZCameraParams);
void zCameraDestroy(ZCamera*);
