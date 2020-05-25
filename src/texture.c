#include "texture.h"
#include "opengl.h"

#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>

void texInit(Texture *t) {
	memset(t, 0, sizeof(*t));
}

static void uploadPlane(const TexturePlane *p, int stride, const void *pixels) {
	glBindTexture(GL_TEXTURE_2D, p->texture);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p->width, p->height, GL_RED, GL_UNSIGNED_BYTE, pixels);
}

static void createPlane(TexturePlane *p, int width, int height, int stride, const void *pixels) {
	if (p->texture == 0)
		glGenTextures(1, &p->texture);
	p->width = width;
	p->height = height;
	glBindTexture(GL_TEXTURE_2D, p->texture);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, p->width, p->height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

int texUploadFrame(Texture *t, int format, const AVFrame *frame) {
	int planes = 0;
	switch (format) {
		case AV_PIX_FMT_YUVJ420P:
		case AV_PIX_FMT_YUV420P:
			{
				const int tformat = YUV420;
				if (tformat == t->format && t->width == frame->width && t->height == frame->height) {
					uploadPlane(t->plane + 0, frame->linesize[0], frame->data[0]);
					uploadPlane(t->plane + 1, frame->linesize[1], frame->data[1]);
					uploadPlane(t->plane + 2, frame->linesize[2], frame->data[2]);
				} else {
					t->planes = 3;
					t->format = tformat;
					t->width = frame->width;
					t->height = frame->height;
					createPlane(t->plane + 0, t->width, t->height, frame->linesize[0], frame->data[0]);
					createPlane(t->plane + 1, t->width/2, t->height/2, frame->linesize[1], frame->data[1]);
					createPlane(t->plane + 2, t->width/2, t->height/2, frame->linesize[2], frame->data[2]);
				}
				return 3;
			}

		default:
			fprintf(stderr, "ERROR: unsupported pixel format for texture upload: %s\n", av_get_pix_fmt_name(format));
			return -1;
	}
}

int texBind(const Texture *t, int start_slot) { // returns number of occupied slots
	for (int i = 0; i < t->planes; ++i) {
		glActiveTexture(GL_TEXTURE0 + start_slot + i);
		glBindTexture(GL_TEXTURE_2D, t->plane[i].texture);
	}
	return t->planes;
}

void texDeinit(Texture *t) {
	for (int i = 0; i < t->planes; ++i)
		glDeleteTextures(1, &t->plane[i].texture);
	memset(t, 0, sizeof(*t));
}
