#pragma once

typedef struct AVFrame AVFrame;
enum AVPixelFormat;

typedef enum { YUV420 } Format;

typedef struct {
	unsigned int texture;
	int width, height;
} TexturePlane;

typedef struct Texture {
	int width, height;
	int planes;
	Format format;
	TexturePlane plane[3]; // YUV
} Texture;

void texInit(Texture *t);
int texUploadFrame(Texture *t, enum AVPixelFormat format, const AVFrame *frame);
int texBind(const Texture *t, int start_slot); // returns number of occupied slots
void texDeinit(Texture *t);
