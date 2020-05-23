#include "zcamera.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void avInit(int log_level) {
	av_log_set_level(log_level);
	av_register_all();
	avformat_network_init();
}

int main(int argc, char* argv[]) {
	if (argc != 2) {
		printf("Usage: %s url\n", argv[0]);
		return EXIT_FAILURE;
	}

	avInit(AV_LOG_WARNING);

	const ZCameraParams params = {
		.source_url = argv[1],
	};
	ZCamera *cam = zCameraCreate(params);
	if (!cam) {
		printf("Failed to create source\n");
		return EXIT_FAILURE;
	}

	for (;;) {
		sleep(1);
	}

	zCameraDestroy(cam);

	return 0;
}
