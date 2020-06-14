#include "zcamera.h"

#define YAGEL_IMPLEMENT
#include "yagel.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define COUNTOF(a) (sizeof(a) / sizeof(*(a)))

static void avInit(int log_level) {
	av_log_set_level(log_level);
	av_register_all();
	avformat_network_init();
}

static int configReadMappingToAvDictionary(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	char *key, *value;
	YagelNode nodes[] = {
		{.type = YAML_SCALAR_EVENT, .action = yagelSaveString, .stop = 1},
		{.type = YAML_MAPPING_END_EVENT, .stop = 1},
		{.type = -1},
	};

	if (0 >= yagelExpectMapping(parser)) {
		fprintf(stderr, "%s expects mapping\n", __FUNCTION__);
		return 0;
	}

	for (;;) {
		key = NULL;
		nodes[0].arg1 = (intptr_t)&key;
		int result = yagelParse(parser, 0, nodes);
		if (0 >= result) {
			fprintf(stderr, "error reading key: %d\n", result);
			return result;
		}
		if (!key) return 1;

		value = NULL;
		nodes[0].arg1 = (intptr_t)&value;
		result = yagelParse(parser, 0, nodes);
		if (0 >= result || !value) {
			fprintf(stderr, "error reading value: %d\n", result);
			return result;
		}

		result = av_dict_set((AVDictionary**)(arg0 + arg1), key, value, AV_DICT_DONT_STRDUP_KEY | AV_DICT_DONT_STRDUP_VAL);
		if (0 > result) {
			fprintf(stderr, "error updating avdict: %d\n", result);
			return result;
		}
	}
}

static const YagelNode nodeOutput[] = {
	{.type = YAML_MAPPING_START_EVENT, .nest = (YagelNode[]){
		{.type = YAML_SCALAR_EVENT, .scalar = "format", .arg1 = offsetof(ConfigOutput, format), .action = yagelSaveNextString},
		{.type = YAML_SCALAR_EVENT, .scalar = "url", .arg1 = offsetof(ConfigOutput, url), .action = yagelSaveNextString},
		{.type = YAML_SCALAR_EVENT, .scalar = "options", .arg1 = offsetof(ConfigOutput, options), .action = configReadMappingToAvDictionary},
		{.type = YAML_MAPPING_END_EVENT, .stop = 1},
		{.type = -1},
	}, .stop = 1},
	{.type = -1},
};

static int configReadOutput(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	const int e = yagelParse(parser, arg0 + arg1, nodeOutput);
	return e;
}

static const YagelNode nodeCamera[] = {
	{.type = YAML_SCALAR_EVENT, .scalar = "input", .nest = (YagelNode[]){
			{.type = YAML_MAPPING_START_EVENT, .nest = (YagelNode[]){
					{.type = YAML_SCALAR_EVENT, .scalar = "url", .arg1 = offsetof(ConfigCamera, input_url), .action = yagelSaveNextString},
					{.type = YAML_MAPPING_END_EVENT, .stop = 1},
					{.type = -1},
				}, .stop = 1},
			{.type = -1},
		},
	},
	{.type = YAML_SCALAR_EVENT, .scalar = "output-live", .arg1 = offsetof(ConfigCamera, output_live), .action=configReadOutput},
	{.type = YAML_SCALAR_EVENT, .scalar = "output-motion", .arg1 = offsetof(ConfigCamera, output_motion), .action=configReadOutput},
	{.type = YAML_SCALAR_EVENT, .scalar = "basic-detect", .nest = (YagelNode[]){
			{.type = YAML_MAPPING_START_EVENT, .nest = (YagelNode[]){
					/* {.type = YAML_SCALAR_EVENT, .scalar = "coeffs", .nest = (YagelNode[]){ */
					/* 		{.type = YAML_SEQUENCE_START_EVENT, .action = configCameraReadDetectCoeffs}, */
					/* 		{.type = -1}, */
					/* 	}, */
					/* }, */
					{.type = YAML_SCALAR_EVENT, .scalar = "threshold", .arg1 = offsetof(ConfigCamera, detect_threshold), .action = yagelSaveNextFloat},
					{.type = YAML_SCALAR_EVENT, .scalar = "thumbnail", .arg1 = offsetof(ConfigCamera, detect_thumbnail), .action = yagelSaveNextString},
					{.type = YAML_SCALAR_EVENT, .scalar = "logfile", .arg1 = offsetof(ConfigCamera, detect_logfile), .action = yagelSaveNextString},
					{.type = YAML_MAPPING_END_EVENT, .stop = 1},
					{.type = -1},
				}, .stop = 1},
			{.type = -1},
		},

	},
	{.type = YAML_MAPPING_START_EVENT},
	{.type = YAML_MAPPING_END_EVENT, .stop = 1},
	{.type = -1},
};

#define MAX_CAMERAS 8
static struct {
	ConfigCamera cameras[MAX_CAMERAS];
	int ncameras;
} config;

static int configParseCamera(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	fprintf(stderr, "Parsing camera %.*s\n", (int)event->data.scalar.length, event->data.scalar.value);
	if (config.ncameras == MAX_CAMERAS) {
		fprintf(stderr, "Max number of cameras reached: %d\n", MAX_CAMERAS);
		return 0;
	}

	ConfigCamera *cam = config.cameras + config.ncameras;
	const int result = yagelParse(parser, (intptr_t)cam, nodeCamera);
	if (result <= 0) {
		fprintf(stderr, "Error reading camera\n");
		memset(cam, 0, sizeof(*cam));
		return 0;
	}

	cam->name = strndup((const char*)event->data.scalar.value, event->data.scalar.length);
	config.ncameras++;
	return 1;
}

static const YagelNode nodeTop[] = {
	{.type = YAML_STREAM_START_EVENT},
	{.type = YAML_DOCUMENT_START_EVENT},
	{.type = YAML_DOCUMENT_END_EVENT, .stop = 1},
	{.type = YAML_MAPPING_END_EVENT},
	{.type = YAML_MAPPING_START_EVENT},
	{.type = YAML_SCALAR_EVENT, .scalar = "cameras", .nest = (YagelNode[]){
			{.type = YAML_MAPPING_START_EVENT, .nest = (YagelNode[]){
					{.type = YAML_SCALAR_EVENT, .action = configParseCamera},
					{.type = YAML_MAPPING_END_EVENT, .stop = 1},
					{.type = -1},
				}, .stop = 1},
			{.type = -1},
		}},
	{.type = -1},
};

static int readConfig(const char *filename) {
	FILE *f = fopen(filename, "rb");
	if (!f) {
		perror("Cannot open config file");
		return -errno;
	}

	yaml_parser_t parser;
	yaml_parser_initialize(&parser);
	yaml_parser_set_input_file(&parser, f);

	const int retval = yagelParse(&parser, 0, nodeTop);

	yaml_parser_delete(&parser);
	return retval;
}

static void printUsage(const char *self) {
		fprintf(stderr, "Usage: %s [-c config] [-v]... rtsp://cam1 [rtsp://cam2]...\n", self);
}

static struct {
	int exit;
	ZCamera *cams[MAX_CAMERAS];
} g;

static void signalHandlerExit(int sig) {
	const static char message[] ="Signal received, exiting\n";
	write(0, message, sizeof(message)-1);
	g.exit = 1;
}

static void setupSignals() {
	struct sigaction sa = {
		.sa_handler = signalHandlerExit,
		.sa_mask = 0,
		.sa_flags = 0,
		.sa_restorer = NULL,
	};
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

int main(int argc, char* argv[]) {
	int log_level = AV_LOG_FATAL;
	int opt;
	int test_only = 0;

	while ((opt = getopt(argc, argv, "tvc:")) != -1) {
		switch(opt) {
			case 'c':
				if (!readConfig(optarg)) {
					fprintf(stderr, "reading config file %s failed\n", optarg);
					return EXIT_FAILURE;
				}
				break;
			case 'v':
				log_level += 8; // AV_LOG_ levels are 8 apart
				break;
			case 't':
				test_only = 1;
				break;
			default: // '?'
				printUsage(argv[0]);
				return EXIT_FAILURE;
		}
	}

	if (config.ncameras < 1) {
		fprintf(stderr, "No camera URLs supplied\n");
		printUsage(argv[0]);
		return EXIT_FAILURE;
	}

	if (test_only)
		return 0;

	setupSignals();
	avInit(log_level);

	for (int i = 0; i < config.ncameras; ++i) {
		const ConfigCamera *ccam = config.cameras + i;
		const ZCameraParams params = {
			ccam,
		};
		g.cams[i] = zCameraCreate(params);
		if (!g.cams[i]) {
			fprintf(stderr, "Failed to create source '%s'\n", ccam->name);
		}
	}

	while (!g.exit) { sleep(1); }

	for (int i = 0; i < config.ncameras; ++i) {
		if (!g.cams[i])
			continue;
		fprintf(stderr, "Stopping camera %s\n", config.cameras[i].name);
		zCameraDestroy(g.cams[i]);
	}

	return 0;
}
