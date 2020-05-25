#include "zcamera.h"

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/timestamp.h>
#include <libavutil/pixdesc.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_STREAMS 4

struct ZCamera {
	void *user_ptr;
	pthread_t thread;

	ZCameraKeyframeTestFunc test_func;
	void *user;

	struct {
		char *url;
		AVFormatContext *fctx;
	} in;

	struct {
		AVFormatContext *fctx;
		int stream_mapping[MAX_STREAMS];
	} hls;

	struct {
		AVCodecContext *ctx;
		int skipped_frames;
	} decode;
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
	cam->in.fctx = NULL;

error:
	return averror;
}

static int openHls(ZCamera *cam) {
	int averror = avformat_alloc_output_context2(&cam->hls.fctx, NULL, "hls", "fixme.m3u8");
	if (0 != averror) {
		fprintf(stderr, "Cannot open HLS output: %s\n", cam->in.url, av_err2str(averror));
		goto error;
	}

	for (int i = 0; i < MAX_STREAMS; ++i)
		cam->hls.stream_mapping[i] = -1;

	int mapped_index = 0;
	for (int i = 0; i < cam->in.fctx->nb_streams; ++i) {
		if (i >= MAX_STREAMS) {
			fprintf(stderr, "WARNING: too many streams, stream %d for HLS output will be ignored\n", i);
			continue;
		}

		const AVCodecParameters *codecpar = cam->in.fctx->streams[i]->codecpar;
		if (codecpar->codec_type != AVMEDIA_TYPE_VIDEO && codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;

		cam->hls.stream_mapping[i] = mapped_index++;

		AVStream *stream = avformat_new_stream(cam->hls.fctx, NULL);
		if (!stream) {
			fprintf(stderr, "ERROR: cannot create new stream for stream %d for HLS output: %s\n", i, av_err2str(averror));
			goto error_close;
		}

		averror = avcodec_parameters_copy(stream->codecpar, codecpar);
		if (averror < 0) {
			fprintf(stderr, "ERROR: cannot copy codec parameters for stream %d for HLS output: %s\n", i, av_err2str(averror));
			goto error_close;
		}
	}

	AVDictionary *options = NULL;
	av_dict_set(&options, "hls_flags", "delete_segments+append_list", 0);
	av_dict_set(&options, "hls_time", "5", 0);
	av_dict_set(&options, "hls_list_size", "5", 0);
	av_dict_set(&options, "hls_base_url", "segments/", 0);
	av_dict_set(&options, "hls_segment_filename", "segments/fixme-%d.ts", 0);

	averror = avformat_write_header(cam->hls.fctx, &options);
	av_dict_free(&options);
	if (averror < 0) {
			fprintf(stderr, "ERROR: cannot write header for HLS output: %s\n", av_err2str(averror));
			goto error_close;
	}

	av_dump_format(cam->hls.fctx, 0, "fixme.m3u8", 1);
	return 0;

error_close:
	avformat_free_context(cam->hls.fctx);
	cam->hls.fctx = NULL;

error:
	return averror;
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt, const char *tag)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
    printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           tag,
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

static enum AVPixelFormat camNegotiateDecodePixelFormat(struct AVCodecContext *s, const enum AVPixelFormat * fmt) {
	int selected = -1;
	for (int i = 0; fmt[i] != -1; ++i) {
		fprintf(stderr, "\t%d %d %s\n", i, fmt[i], av_get_pix_fmt_name(fmt[i]));
		if (selected != -1 && (fmt[i] == AV_PIX_FMT_YUVJ420P || fmt[i] == AV_PIX_FMT_YUV420P))
			selected = i;
	}
	if (selected == -1) selected = 0;
	fprintf(stderr, "Selected: %d %s\n", fmt[selected], av_get_pix_fmt_name(fmt[selected]));
	return fmt[selected];
}

static void analyzePacket(ZCamera *cam, const AVPacket *pkt) {
	const AVStream *stream = cam->in.fctx->streams[pkt->stream_index];
	const int is_video = stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
	if (!is_video || ((pkt->flags & AV_PKT_FLAG_KEY) == 0)) {
		++cam->decode.skipped_frames;
		return;
	}

	if (cam->decode.ctx == NULL) {
		const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
		cam->decode.ctx = avcodec_alloc_context3(codec);
		cam->decode.ctx->get_format = camNegotiateDecodePixelFormat;
		int averr = avcodec_open2(cam->decode.ctx, codec, NULL);
		if (averr != 0) {
			fprintf(stderr, "Error creating decoder: %s", av_err2str(averr));
			goto error_close;
		}
	}

	printf("P[ptd:%lld dts:%lld S:%d F:%x sz:%d pos:%lld dur:%lld], skipped: %d\n",
		(long long)pkt->pts, (long long)pkt->dts,
		pkt->stream_index, pkt->flags, pkt->size, (long long)pkt->pos, (long long)pkt->duration, cam->decode.skipped_frames);
	cam->decode.skipped_frames = 0;

	int averr = avcodec_send_packet(cam->decode.ctx, pkt);
	if (averr < 0) {
		fprintf(stderr, "Error decoding frame: %s", av_err2str(averr));
		// return; ?
	}

	for (;;) {
		AVFrame frame;
		memset(&frame, 0, sizeof(frame));
		averr = avcodec_receive_frame(cam->decode.ctx, &frame);
		if (averr == AVERROR(EAGAIN) || averr == AVERROR_EOF)
			return;

		fprintf(stderr, "F[%d, type=%d, format=%s, %dx%d]\n", cam->decode.ctx->frame_number, frame.pict_type, av_get_pix_fmt_name(frame.format), frame.width, frame.height);

		cam->test_func(cam->user, frame.format, &frame);

		av_frame_unref(&frame);
	}

error_close:
	// FIXME stagger errors
	avcodec_free_context(&cam->decode.ctx);
	cam->decode.ctx = NULL;
}

static int readPacket(ZCamera *cam) {
	AVPacket pkt;
	av_init_packet(&pkt);
	const int result = av_read_frame(cam->in.fctx, &pkt);
	if (result != 0) {
		fprintf(stderr, "Error reading frame: %d", result);
		return result;
	}

	analyzePacket(cam, &pkt);

	if (!cam->hls.fctx) {
		openHls(cam);
	}

	if (cam->hls.fctx && pkt.stream_index < MAX_STREAMS && cam->hls.stream_mapping[pkt.stream_index] >= 0) {
		AVStream *in_stream = cam->in.fctx->streams[pkt.stream_index];
		pkt.stream_index = cam->hls.stream_mapping[pkt.stream_index];
		AVStream *out_stream = cam->hls.fctx->streams[pkt.stream_index];

		pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
		pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
		pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
		//pkt.pos = -1;

		//packetCallback(&pkt, cam->in.fctx->streams[pkt.stream_index]);
		//log_packet(cam->hls.fctx, &pkt, "out");
		int averror = av_interleaved_write_frame(cam->hls.fctx, &pkt);
		if (averror < 0) {
				fprintf(stderr, "Error muxing packet: %s\n", av_err2str(averror));
				// FIXME handle error
		}
	}

	av_packet_unref(&pkt);
	return 0;
}

static void *zCameraThreadFunc(void *param) {
	ZCamera *cam = param;

	for (;;) {
		if (0 == openCamera(cam)) {
			while (0 == readPacket(cam)) {}
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
	cam->test_func = params.test_func;
	cam->user = params.user;

	const int sync = 1;
	if (sync) {
		if (0 != openCamera(cam)) {
			zCameraDestroy(cam);
			return NULL;
		}
	} else {
		if (0 != pthread_create(&cam->thread, NULL, zCameraThreadFunc, cam)) {
			printf("ERROR: Cannot create packet reader thread\n");
			goto error;
		}
	}

	return cam;

error:
	free(cam);
	return NULL;
}

int zCameraPollPacket(ZCamera *cam) {
	return readPacket(cam);
}

void zCameraDestroy(ZCamera *cam) {
	// FIXME
}
