#include "zcamera.h"

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct ZCamera {
	void *user_ptr;
	pthread_t thread;

	struct {
		char *url;
		AVFormatContext *fctx;
	} in;

	struct {
	} hls;
};

static int openCamera(ZCamera *cam) {
	// TODO: stimeout for rtsp
	int averror = avformat_open_input(&cam->in.fctx, cam->in.url, NULL, NULL);
	if (0 != averror) {
		fprintf(stderr, "Cannot open input \"%s\": %s\n", cam->in.url, av_err2str(averror));
		goto error;
	}

	averror = avformat_find_stream_info(cam->in.fctx, NULL);
	if (0 > averror) {
		fprintf(stderr, "Cannot find streams in \"%s\": %s\n", cam->in.url, av_err2str(averror));
		goto error_close;
	}

	av_dump_format(cam->in.fctx, 0, cam->in.url, 0);
	// TODO rtsp errors 5xx on this av_read_play(cam->in.fctx);
	return 0;

error_close:
	avformat_close_input(&cam->in.fctx);
error:
	return averror;
}

static int skipped_frames = 0;
static void packetCallback(const AVPacket *pkt, const AVStream *stream) {
	const int is_video = stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;

	if (!is_video || ((pkt->flags & AV_PKT_FLAG_KEY) == 0)) {
		++skipped_frames;
		return;
	}

	printf("P[ptd:%lld dts:%lld S:%d F:%x sz:%d pos:%lld dur:%lld], skipped: %d\n",
		(long long)pkt->pts, (long long)pkt->dts,
		pkt->stream_index, pkt->flags, pkt->size, (long long)pkt->pos, (long long)pkt->duration, skipped_frames);

	skipped_frames = 0;
}

static void readPackets(ZCamera *cam) {
	AVPacket pkt;
	for (;;) {
		av_init_packet(&pkt);
		const int result = av_read_frame(cam->in.fctx, &pkt);
		if (result != 0) {
			fprintf(stderr, "Error reading frame: %d", result);
			break;
		}

		// TODO treat the packet
		//packet_callback(param);
		packetCallback(&pkt, cam->in.fctx->streams[pkt.stream_index]);

		av_packet_unref(&pkt);
	}
}

static void *zCameraThreadFunc(void *param) {
	ZCamera *cam = param;

	for (;;) {
		if (0 == openCamera(cam)) {
			readPackets(cam);
		}

		// FIXME exit condition

		fprintf(stderr, "Retrying camera %p in 1 sec\n", cam);
		sleep(1);
	}

	// FIXME deinit strings
	return NULL;
}

char *stringCopy(const char* src, int len) {
	const int length = len >= 0 ? len : strlen(src);
	char *str = malloc(length + 1);
	strncpy(str, src, length);
	str[length] = '\0';
	return str;
}

ZCamera *zCameraCreate(ZCameraParams params) {
	ZCamera *cam = malloc(sizeof(*cam));
	memset(cam, 0, sizeof(*cam));

	cam->in.url = stringCopy(params.source_url, -1);

	if (0 != pthread_create(&cam->thread, NULL, zCameraThreadFunc, cam)) {
		printf("ERROR: Cannot create packet reader thread\n");
		goto error;
	}

	return cam;

error:
	free(cam);
	return NULL;
}

void zCameraDestroy(ZCamera *cam) {
	// FIXME
}
