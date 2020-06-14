#include "zcamera.h"

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/timestamp.h>
#include <libavutil/pixdesc.h>

#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_STREAMS 4

typedef struct {
	AVFormatContext *fctx;
	int stream_mapping[MAX_STREAMS];
} Output;

struct ZCamera {
	const ConfigCamera *config;
	pthread_t thread;
	int close;

	struct {
		AVFormatContext *fctx;
	} in;

	Output live;

	struct {
		AVCodecContext *ctx;
	} decode;

#define PACKET_QUEUE_LEN 512
	struct {
		FILE *log;
		int active;
		AVFrame *prev;
		int cursor_read, cursor_write;
		AVPacket queue[PACKET_QUEUE_LEN];
	} detect;
};


static int openCamera(ZCamera *cam) {
	// TODO: stimeout for rtsp
	int averror = avformat_open_input(&cam->in.fctx, cam->config->input_url, NULL, NULL);
	if (0 != averror) {
		fprintf(stderr, "Cannot open input \"%s\": %s\n", cam->config->input_url, av_err2str(averror));
		goto error;
	}

	averror = avformat_find_stream_info(cam->in.fctx, NULL);
	if (0 > averror) {
		fprintf(stderr, "Cannot find streams in \"%s\": %s\n", cam->config->input_url, av_err2str(averror));
		goto error_close;
	}

	av_dump_format(cam->in.fctx, 0, cam->config->input_url, 0);

	// TODO rtsp errors 5xx on this av_read_play(cam->in.fctx);
	return 0;

error_close:
	avformat_close_input(&cam->in.fctx);
	cam->in.fctx = NULL;

error:
	return averror;
}

static int outputOpen(Output *out, const ConfigOutput *config, const AVFormatContext *inctx) {
	char url[1024];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(url, sizeof(url), config->url, &tm);

	int averror = avformat_alloc_output_context2(&out->fctx, NULL, config->format, url);
	if (0 != averror) {
		fprintf(stderr, "Cannot output URL: %s\n", url, av_err2str(averror));
		goto error;
	}

	for (int i = 0; i < MAX_STREAMS; ++i)
		out->stream_mapping[i] = -1;

	int mapped_index = 0;
	for (int i = 0; i < inctx->nb_streams; ++i) {
		if (i >= MAX_STREAMS) {
			fprintf(stderr, "WARNING: too many streams, stream %d for output will be ignored\n", i);
			continue;
		}

		const AVCodecParameters *codecpar = inctx->streams[i]->codecpar;
		if (codecpar->codec_type != AVMEDIA_TYPE_VIDEO && codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;

		out->stream_mapping[i] = mapped_index++;

		AVStream *stream = avformat_new_stream(out->fctx, NULL);
		if (!stream) {
			fprintf(stderr, "ERROR: cannot create new stream for stream %d for output: %s\n", i, av_err2str(averror));
			goto error_close;
		}

		averror = avcodec_parameters_copy(stream->codecpar, codecpar);
		if (averror < 0) {
			fprintf(stderr, "ERROR: cannot copy codec parameters for stream %d for output: %s\n", i, av_err2str(averror));
			goto error_close;
		}
	}

	av_dump_format(out->fctx, 0, url, 1);

	if (!(out->fctx->flags & AVFMT_NOFILE)) {
		const int averr = avio_open(&out->fctx->pb, url, AVIO_FLAG_WRITE);
		if (averr < 0) {
			fprintf(stderr, "avio_open(%s) error: %s\n", url, av_err2str(averr));
			goto error_close;
		}
	}

	AVDictionary *options = NULL;
	if (config->options)
		av_dict_copy(&options, config->options, 0);
	averror = avformat_write_header(out->fctx, options ? &options : NULL);
	if (averror < 0) {
			fprintf(stderr, "ERROR: cannot write header for output: %s\n", av_err2str(averror));
			goto error_close;
	}

	return 0;

error_close:
	avformat_free_context(out->fctx);
	out->fctx = NULL;

error:
	// TODO free formatted url
	return averror;
}

static int outputWrite(Output *out, const AVFormatContext *inctx, AVPacket *pkt) {
	if (!out->fctx)
		return 0;

	if (pkt->stream_index >= MAX_STREAMS || out->stream_mapping[pkt->stream_index] < 0) {
		// skip packet
		return 1;
	}

	AVStream *in_stream = inctx->streams[pkt->stream_index];
	pkt->stream_index = out->stream_mapping[pkt->stream_index];
	AVStream *out_stream = out->fctx->streams[pkt->stream_index];

	pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
	pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
	pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
	//pkt.pos = -1;

	int averror = av_interleaved_write_frame(out->fctx, pkt);
	if (averror < 0) {
			fprintf(stderr, "Error muxing packet: %s\n", av_err2str(averror));
			// FIXME handle error
	}
	return averror;
}

static void outputClose(Output *out) {
	if (!out->fctx)
		return;

	if (!(out->fctx->flags & AVFMT_NOFILE)) {
		avio_closep(&out->fctx->pb);
	}
	avformat_free_context(out->fctx);
	out->fctx = NULL;
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
		if (selected == -1 && (fmt[i] == AV_PIX_FMT_YUVJ420P || fmt[i] == AV_PIX_FMT_YUV420P))
			selected = i;
	}
	if (selected == -1) selected = 0;
	fprintf(stderr, "Selected: %d %s\n", fmt[selected], av_get_pix_fmt_name(fmt[selected]));
	 return fmt[selected];
}

static uint64_t byteDifference(const uint8_t *a, const uint8_t *b, int width, int height, int stride) {
	uint64_t value = 0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const int off = x + y * stride;
			value += abs((int)(a[off]) - (int)(b[off]));
		}
	}

	return value;
}

typedef struct {
	float u, y, v;
} Yuv;

static int frameCompare(const AVFrame *a, const AVFrame *b, Yuv *retval) {
	if (!a || !b) return -1;
	if (a->format != b->format) return -2;
	if (a->width != b->width || a->height != b->height) return -3;
	if (a->format != AV_PIX_FMT_YUV420P && a->format != AV_PIX_FMT_YUVJ420P) return -4;
	if (a->linesize[0] != b->linesize[0]) return -5;
	if (a->linesize[1] != b->linesize[1]) return -6;
	if (a->linesize[2] != b->linesize[2]) return -7;
	const float scale = a->width * a->height / 100.f;
	retval->y = byteDifference(a->data[0], b->data[0], a->width, a->height, a->linesize[0]) / scale;
	retval->u = byteDifference(a->data[1], b->data[1], a->width/2, a->height/2, a->linesize[1]) * 4 / scale;
	retval->v = byteDifference(a->data[2], b->data[2], a->width/2, a->height/2, a->linesize[2]) * 4 / scale;
	return 1;
}

static void processPacket(ZCamera *cam, const AVPacket *pkt) {
	const AVStream *stream = cam->in.fctx->streams[pkt->stream_index];
	const int is_video = stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
	const int is_keyframe = !!(pkt->flags & AV_PKT_FLAG_KEY);

	// Store packet in a queue
	{
		const int next_write = (cam->detect.cursor_write + 1) % PACKET_QUEUE_LEN;
		if (next_write == cam->detect.cursor_read) {
			fprintf(stderr, "Camera %s queue depleted!\n", cam->config->name);
			// FIXME what to do?
			av_packet_unref(cam->detect.queue + cam->detect.cursor_read);
			cam->detect.cursor_read = (cam->detect.cursor_read + 1) % PACKET_QUEUE_LEN;
		}

		av_packet_ref(cam->detect.queue + cam->detect.cursor_write, pkt);
		cam->detect.cursor_write = next_write;
	}

	// We don't care about non-leyframes
	if (!is_video || !is_keyframe) {
		return;
	}

	const int wr = cam->detect.cursor_write;
	const int rd = cam->detect.cursor_read;
	const int queue_len = (wr >= rd) ? wr - rd : wr + PACKET_QUEUE_LEN - rd;
	fprintf(stderr, "P[ptd:%lld dts:%lld S:%d F:%x sz:%d pos:%lld dur:%lld], queue: %d\n",
		(long long)pkt->pts, (long long)pkt->dts,
		pkt->stream_index, pkt->flags, pkt->size, (long long)pkt->pos, (long long)pkt->duration, queue_len);

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

	const int averr = avcodec_send_packet(cam->decode.ctx, pkt);
	if (averr < 0) {
		fprintf(stderr, "Error decoding frame: %s", av_err2str(averr));
		// return; ?
	}

	for (;;) {
		AVFrame *frame = av_frame_alloc();
		const int averr = avcodec_receive_frame(cam->decode.ctx, frame);
		if (averr == AVERROR(EAGAIN) || averr == AVERROR_EOF) {
			av_frame_free(&frame);
			break;
		}
		if (averr != 0) {
			fprintf(stderr, "Error decoding frame: %s\n", av_err2str(averr));
			av_frame_free(&frame);
			break;
		}

		Yuv diff = { 0 };
		const int comperr = frameCompare(cam->detect.prev, frame, &diff);
		int detected = 0;
		if (0 >= comperr) {
			fprintf(stderr, "Error comparing frames: %d\n", comperr);
			detected = 1;
		} else {
			const float difference = diff.y + diff.u + diff.v;
			detected = difference > cam->config->detect_threshold;
			fprintf(stderr, "F[%d, type=%d, format=%s, %dx%d delta=%f detected=%d]\n",
					cam->decode.ctx->frame_number, frame->pict_type, av_get_pix_fmt_name(frame->format), frame->width, frame->height, difference, detected);
			if (cam->detect.log) {
				fprintf(cam->detect.log, "%f %f %f %f %d\n", diff.y, diff.u, diff.v, difference, detected);
				fflush(cam->detect.log);
			}
		}

		if (detected) {
			// TODO write thumbnail
			cam->detect.active = 1;
		}

		av_frame_unref(cam->detect.prev);
		av_frame_free(&cam->detect.prev);
		cam->detect.prev = frame;
	}

	Output motion = { 0 };
	if (cam->detect.active) {
		// TODO write segment if detected
		if (0 != outputOpen(&motion, &cam->config->output_motion, cam->in.fctx)) {
			fprintf(stderr, "Camera %s failed to open motion file for writing\n", cam->config->name);
			motion.fctx = NULL;
		}
	}

	// Dump or ignore queue
	for (; cam->detect.cursor_read != cam->detect.cursor_write;
			cam->detect.cursor_read = (cam->detect.cursor_read + 1) % PACKET_QUEUE_LEN) {
		/* const int wr = cam->detect.cursor_write; */
		/* const int rd = cam->detect.cursor_read; */
		/* const int queue_len = (wr >= rd) ? wr - rd : wr + PACKET_QUEUE_LEN - rd; */
		/* fprintf(stderr, "%s %d %d %d\n", cam->config->name, rd, wr, queue_len); */

		AVPacket *packet = cam->detect.queue + cam->detect.cursor_read;
		if (motion.fctx && 0 != outputWrite(&motion, cam->in.fctx, packet)) {
			fprintf(stderr, "Camera %s failed writing a packet\n", cam->config->name);
		}
		av_packet_unref(packet);
	}

	outputClose(&motion);
	cam->detect.active = 0;
	return;

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
		// FIXME what to do here?
		return result;
	}

	processPacket(cam, &pkt);

	if (!cam->live.fctx) {
		// FIXME error check
		outputOpen(&cam->live, &cam->config->output_live, cam->in.fctx);
	}

	if (cam->live.fctx) {
		// FIXME error check
		outputWrite(&cam->live, cam->in.fctx, &pkt);
	}

	av_packet_unref(&pkt);
	return 0;
}

static void *zCameraThreadFunc(void *param) {
	ZCamera *cam = param;

	for (;;) {
		if (0 == openCamera(cam)) {
			while (!cam->close && 0 == readPacket(cam)) {}
		}

		if (cam->close)
			break;

		fprintf(stderr, "Retrying camera %p in 1 sec\n", cam);
		sleep(1);
	}

	fprintf(stderr, "Closing camera %s\n", cam->config->name);

	for (int i = 0; i < PACKET_QUEUE_LEN; ++i) {
		av_packet_unref(cam->detect.queue + i);
	}

	av_frame_free(&cam->detect.prev);
	avcodec_close(cam->decode.ctx);
	outputClose(&cam->live);
	avformat_close_input(&cam->in.fctx);
	if (cam->detect.log)
		fclose(cam->detect.log);

	return NULL;
}

ZCamera *zCameraCreate(ZCameraParams params) {
	FILE *detect_log = NULL;
	if (params.config->detect_logfile) {
		detect_log = fopen(params.config->detect_logfile, "a");
		if (!detect_log) {
			fprintf(stderr, "Cannot open log file '%s': %s\n", params.config->detect_logfile, strerror(errno));
			return NULL;
		}
	}

	ZCamera *cam = malloc(sizeof(*cam));
	memset(cam, 0, sizeof(*cam));
	cam->config = params.config;
	cam->detect.log = detect_log;

	if (0 != pthread_create(&cam->thread, NULL, zCameraThreadFunc, cam)) {
		printf("ERROR: Cannot create camera thread\n");
		free(cam);
		return NULL;
	}

	return cam;
}

void zCameraDestroy(ZCamera *cam) {
	if (!cam)
		return;

	cam->close = 1;

	void *retval = NULL;
	const int result = pthread_join(cam->thread, &retval);
	if (result != 0) {
		fprintf(stderr, "Error joining camera '%s' thread: %s\n", cam->config->name, strerror(result));
	}

	memset(cam, 0, sizeof(*cam));
}
