#include "zcamera.h"
#include "texture.h"

#ifdef USE_ATTO
#include "atto/app.h"
#define ATTO_GL_H_IMPLEMENT
#include "atto/gl.h"
#endif

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

int main(int argc, char* argv[]) {
	if (argc != 2) {
		printf("Usage: %s url\n", argv[0]);
		return EXIT_FAILURE;
	}

	avInit(AV_LOG_ERROR);

	const ZCameraParams params = {
		.source_url = argv[1],
		.test_func = NULL,
		.user = NULL,
	};
	ZCamera *cam = zCameraCreate(params);
	if (!cam) {
		printf("Failed to create source\n");
		return EXIT_FAILURE;
	}

	for(;;)	zCameraPollPacket(cam);

	return 0;
}
#endif // USE_ATTO
