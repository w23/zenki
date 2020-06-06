#include "zcamera.h"
#include "texture.h"

#ifdef USE_ATTO
#include "atto/app.h"
#define ATTO_GL_H_IMPLEMENT
#include "atto/gl.h"
#endif

#include <yaml.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

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

typedef struct {
	const char *str;
	int len;
} StringView;

int svCmp(StringView a, const char *b) {
	return strncmp(a.str, b, a.len);
}

typedef struct {
	int type;
	const char *scalar; // NULL == any
	intptr_t arg1;
	int stop;

	// return >0 on success, else error (TODO: continue looking/exit semantics?)
	int (*action)(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1);
} ExpectedNode;

// returns > 0 on success
static int yamlParse(yaml_parser_t *parser, intptr_t arg0, const ExpectedNode *nodes, int nodes_num) {
	int stop = 0;
	while (!stop) {
		yaml_event_t event;
		if (!yaml_parser_parse(parser, &event)) {
			fprintf(stderr, "Error parsing yaml\n");
			return 0;
		}

		switch (event.type) {
			case YAML_NO_EVENT: fprintf(stderr, "YAML_NO_EVENT\n"); break;
			case YAML_STREAM_START_EVENT: fprintf(stderr, "YAML_STREAM_START_EVENT\n"); break;
			case YAML_STREAM_END_EVENT: fprintf(stderr, "YAML_STREAM_END_EVENT\n"); break;
			case YAML_DOCUMENT_START_EVENT: fprintf(stderr, "YAML_DOCUMENT_START_EVENT\n"); break;
			case YAML_DOCUMENT_END_EVENT: fprintf(stderr, "YAML_DOCUMENT_END_EVENT\n"); break;
			case YAML_ALIAS_EVENT: fprintf(stderr, "YAML_ALIAS_EVENT\n"); break;
			case YAML_SCALAR_EVENT: fprintf(stderr, "YAML_SCALAR_EVENT: %.*s\n", (int)event.data.scalar.length, event.data.scalar.value); break;
			case YAML_SEQUENCE_START_EVENT: fprintf(stderr, "YAML_SEQUENCE_START_EVENT\n"); break;
			case YAML_SEQUENCE_END_EVENT: fprintf(stderr, "YAML_SEQUENCE_END_EVENT\n"); break;
			case YAML_MAPPING_START_EVENT: fprintf(stderr, "YAML_MAPPING_START_EVENT\n"); break;
			case YAML_MAPPING_END_EVENT: fprintf(stderr, "YAML_MAPPING_END_EVENT\n"); break;
		}

		int processed = 0;
		if (event.type == YAML_NO_EVENT) {
			processed = 1;
		} else {
			for (int i = 0; i < nodes_num; ++i) {
				const ExpectedNode *node = nodes + i;
				//printf("%d %d E%d\n", i, nodes_num, node->type);
				if (node->type != event.type)
					continue;

				if (node->type == YAML_SCALAR_EVENT && node->scalar && 0 != strncmp(node->scalar, (const char*)event.data.scalar.value, event.data.scalar.length))
					continue;

				if (node->action && 0 >= node->action(parser, &event, arg0, node->arg1)) {
					yaml_event_delete(&event);
					return 0;
				}

				if (node->stop)
					stop = 1;

				processed = 1;
				break;
			}
		}

		const int event_type = event.type;
		yaml_event_delete(&event);
		if (!processed) {
			fprintf(stderr, "Error: yaml event %d unhandled\n", event_type);
			return 0;
		}
	}

	return 1;
}

static const ExpectedNode expectMapping[] = {{.type = YAML_MAPPING_START_EVENT, .stop = 1}};
static int configParseExpectMapping(yaml_parser_t *parser) {
	return yamlParse(parser, 0, expectMapping, 1);
}

static int configParseReadString(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	if (event->type != YAML_SCALAR_EVENT) {
		fprintf(stderr, "%s expects scalar\n", __FUNCTION__);
		return 0;
	}

	*((char**)arg0) = stringCopy((const char*)event->data.scalar.value, event->data.scalar.length);
	return 1;
}

static int configParseReadNextString(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	const ExpectedNode nodeReadNextString[] = {{.type = YAML_SCALAR_EVENT, .arg1 = arg1, .action = configParseReadString, .stop = 1}};
	return yamlParse(parser, arg0, nodeReadNextString, 1);
}

static const ExpectedNode nodeCameraInput[] = {
	{.type = YAML_MAPPING_END_EVENT, .stop = 1},
	{.type = YAML_SCALAR_EVENT, .scalar = "url", .action = configParseReadNextString},
};

static int configParseCameraInput(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	if (0 >= configParseExpectMapping(parser)) {
		fprintf(stderr, "Error: camera input should be mapping\n");
		return 0;
	}

	char *url = NULL;
	const int retval = yamlParse(parser, (intptr_t)&url, nodeCameraInput, COUNTOF(nodeCameraInput));
	fprintf(stderr, "\turl = %s\n", url);
	free(url);
	return retval;
}

static const ExpectedNode ignore[] = {
	{.type = YAML_MAPPING_END_EVENT, .stop = 1},
	{.type = YAML_SCALAR_EVENT},
	{.type = YAML_MAPPING_START_EVENT},
	{.type = YAML_SEQUENCE_START_EVENT},
	{.type = YAML_SEQUENCE_END_EVENT},
};

static int configParseCameraLiveOutput(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	return yamlParse(parser, 0, ignore, COUNTOF(ignore));
}

static int configParseCameraDetect(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	return yamlParse(parser, 0, ignore, COUNTOF(ignore));
}

static const ExpectedNode nodeCamera[] = {
	{.type = YAML_SCALAR_EVENT, .scalar = "input", .action = configParseCameraInput},
	{.type = YAML_SCALAR_EVENT, .scalar = "output-live", .action = configParseCameraLiveOutput},
	{.type = YAML_SCALAR_EVENT, .scalar = "basic-detect", .action = configParseCameraDetect},
	{.type = YAML_MAPPING_START_EVENT},
	{.type = YAML_MAPPING_END_EVENT, .stop = 1},
};

static int configParseCamera(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	fprintf(stderr, "Parsing camera %.*s\n", (int)event->data.scalar.length, event->data.scalar.value);
	return yamlParse(parser, 0/*FIXME cam ptr/index*/, nodeCamera, COUNTOF(nodeCamera));
}

static const ExpectedNode nodeCameras[] = {
	{.type = YAML_SCALAR_EVENT, .action = configParseCamera},
	{.type = YAML_MAPPING_START_EVENT},
	{.type = YAML_MAPPING_END_EVENT, .stop = 1},
};

static int configParseCameras(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	return yamlParse(parser, 0, nodeCameras, COUNTOF(nodeCameras));
}

static const ExpectedNode nodeTop[] = {
	{.type = YAML_STREAM_START_EVENT},
	{.type = YAML_DOCUMENT_START_EVENT},
	{.type = YAML_DOCUMENT_END_EVENT, .stop = 1},
	{.type = YAML_MAPPING_START_EVENT},
	{.type = YAML_SCALAR_EVENT, .scalar = "cameras", .action = configParseCameras},
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

	const int retval = yamlParse(&parser, 0, nodeTop, COUNTOF(nodeTop));

	yaml_parser_delete(&parser);
	return retval;
}

static void printUsage(const char *self) {
		fprintf(stderr, "Usage: %s [-c config] [-v]... rtsp://cam1 [rtsp://cam2]...\n", self);
}

int main(int argc, char* argv[]) {
	int log_level = AV_LOG_FATAL;
	int opt;
	const char *config = NULL;

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

	if (optind == argc) {
		fprintf(stderr, "No camera URLs supplied\n");
		printUsage(argv[0]);
		return EXIT_FAILURE;
	}

#define MAX_CAMERAS 4
	ZCamera *cams[MAX_CAMERAS];

	int ncameras = (argc - optind) / 2;
	if (ncameras > MAX_CAMERAS) {
		fprintf(stderr, "%d urls camera URLs supplied, but only max %d will be used\n", ncameras, MAX_CAMERAS);
		ncameras = MAX_CAMERAS;
	}

	avInit(log_level);

	for (int i = 0; i < ncameras; ++i) {
		const char *hls = argv[optind + i*2];
		const char *url = argv[optind + i*2 + 1];
		const ZCameraParams params = {
			.name_m3u8 = hls,
			.source_url = url,
		};
		cams[i] = zCameraCreate(params);
		if (!cams[i]) {
			fprintf(stderr, "Failed to create source '%s'\n", config);
		}
	}

	for(;;) { sleep(1); }

	return 0;
}
#endif // USE_ATTO
