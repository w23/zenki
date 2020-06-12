#include "zcamera.h"
#include "texture.h"

#ifdef USE_ATTO
#include "atto/app.h"
#define ATTO_GL_H_IMPLEMENT
#include "atto/gl.h"
#endif

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

#ifdef USE_ATTO
static struct {
	ZCamera *cam;
	Texture t;
	int advance;
	AGLProgram program;
} g;

static void resize(ATimeUs ts, unsigned int old_width, unsigned int old_height) {
	glViewport(0, 0, a_app_state->width, a_app_state->height);
}

void frame(void *user, int format, const AVFrame *frame) {
	(void)user;
	texUploadFrame(&g.t, format, frame);
	g.advance = 1;
}

static const char shader_vertex[] =
	"void main() { gl_Position = gl_Vertex; }";

static const char shader_fragment[] =
	"uniform sampler2D Ty, Tu, Tv;"
	"uniform vec2 R;"
	"vec3 yuv2rgb(vec3 yuv) {"
		"return vec3("
			"dot(vec3(1.,      0., 1.28033), yuv),"
			"dot(vec3(1., -.21482, -.38059), yuv),"
			"dot(vec3(1., 2.12798, 0.), yuv));"
	"}"
	"void main() {"
		"vec2 uv = gl_FragCoord.xy / R;"
		"uv.y *= -1.;"
		"vec3 yuv = vec3("
			"texture2D(Ty, uv).r,"
			"texture2D(Tu, uv).r - .5,"
			"texture2D(Tv, uv).r - .5);"
		"gl_FragColor = vec4(yuv2rgb(yuv), 0.);"
		//"gl_FragColor = vec4(yuv2rgb(vec3(0., uv - .5)), 0.);"
	"}";

static void paint(ATimeUs ts, float dt) {
	while(!g.advance) {
		zCameraPollPacket(g.cam);
		g.advance = 1;
	}
	g.advance = 0;
	texBind(&g.t, 0);

	glUniform2f(glGetUniformLocation(g.program, "R"), a_app_state->width, a_app_state->height);
	glUniform1i(glGetUniformLocation(g.program, "Ty"), 0);
	glUniform1i(glGetUniformLocation(g.program, "Tu"), 1);
	glUniform1i(glGetUniformLocation(g.program, "Tv"), 2);

	glRects(-1, -1, 1, 1);
}

static int init(int argc, const char *const *argv) {
	if (argc != 2) {
		printf("Usage: %s url\n", argv[0]);
		return EXIT_FAILURE;
	}

	avInit(AV_LOG_ERROR);

	const ZCameraParams params = {
		.source_url = argv[1],
		.test_func = frame,
		.user = NULL,
	};
	g.cam = zCameraCreate(params);
	if (!g.cam) {
		printf("Failed to create source\n");
		return EXIT_FAILURE;
	}

	return 0;
}

void attoAppInit(struct AAppProctable* a) {
	const int argc = a_app_state->argc;
	const char *const *argv = a_app_state->argv;
	const int ret = init(argc, argv);

	if (ret != 0)
		aAppTerminate(ret);

	aGLInit();
	texInit(&g.t);

	g.program = aGLProgramCreateSimple(shader_vertex, shader_fragment);
	if (g.program <= 0) {
		aAppDebugPrintf("shader error: %s", a_gl_error);
		aAppTerminate(EXIT_FAILURE);
	}
	glUseProgram(g.program);

	a->paint = paint;
	a->resize = resize;
}

#else

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
			fprintf(stderr, "error reading key: %d", result);
			return result;
		}
		if (!key) return 1;

		value = NULL;
		nodes[0].arg1 = (intptr_t)&value;
		result = yagelParse(parser, 0, nodes);
		if (0 >= result || !value) {
			fprintf(stderr, "error reading value: %d", result);
			return result;
		}

		result = av_dict_set((AVDictionary**)(arg0 + arg1), key, value, AV_DICT_DONT_STRDUP_KEY | AV_DICT_DONT_STRDUP_VAL);
		if (0 > result) {
			fprintf(stderr, "error updating avdict: %d", result);
			return result;
		}
	}
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
	{.type = YAML_SCALAR_EVENT, .scalar = "output-live", .nest = (YagelNode[]){
			{.type = YAML_MAPPING_START_EVENT, .nest = (YagelNode[]){
					{.type = YAML_SCALAR_EVENT, .scalar = "format", .arg1 = offsetof(ConfigCamera, live_format), .action = yagelSaveNextString},
					{.type = YAML_SCALAR_EVENT, .scalar = "url", .arg1 = offsetof(ConfigCamera, live_url), .action = yagelSaveNextString},
					{.type = YAML_SCALAR_EVENT, .scalar = "format-options", .arg1 = offsetof(ConfigCamera, live_options), .action = configReadMappingToAvDictionary},
					{.type = YAML_MAPPING_END_EVENT, .stop = 1},
					{.type = -1},
				}, .stop = 1},
			{.type = -1},
		},
	},
	{.type = YAML_SCALAR_EVENT, .scalar = "basic-detect", .nest = (YagelNode[]){
			{.type = YAML_MAPPING_START_EVENT, .nest = (YagelNode[]){
					/* {.type = YAML_SCALAR_EVENT, .scalar = "coeffs", .nest = (YagelNode[]){ */
					/* 		{.type = YAML_SEQUENCE_START_EVENT, .action = configCameraReadDetectCoeffs}, */
					/* 		{.type = -1}, */
					/* 	}, */
					/* }, */
					{.type = YAML_SCALAR_EVENT, .scalar = "threshold", .arg1 = offsetof(ConfigCamera, detect_threshold), .action = yagelSaveNextFloat},
					{.type = YAML_SCALAR_EVENT, .scalar = "thumbnail", .arg1 = offsetof(ConfigCamera, detect_thumbnail), .action = yagelSaveNextString},
					{.type = YAML_SCALAR_EVENT, .scalar = "output-url", .arg1 = offsetof(ConfigCamera, detect_output), .action = yagelSaveNextString},
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

	while ((opt = getopt(argc, argv, "vc:")) != -1) {
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
#endif // USE_ATTO
