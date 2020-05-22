#pragma once

typedef struct {
	const char *source_url;
} ZCameraParams;

typedef struct ZCamera ZCamera;

ZCamera *zCameraCreate(ZCameraParams);
void zCameraDestroy(ZCamera*);
