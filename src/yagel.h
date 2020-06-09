#pragma once
#include <yaml.h>
#include <stdint.h>

typedef struct YagelNode YagelNode;
typedef struct YagelNode {
	int type; // -1 == end
	const char *scalar; // NULL == any
	intptr_t arg1;
	int stop;

	// return >0 on success, else error (TODO: continue looking/exit semantics?)
	int (*action)(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1);
	const YagelNode *const nest;
} YagelNode;

int yagelParse(yaml_parser_t *parser, intptr_t arg0, const YagelNode *nodes);
int yagelSaveString(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1);
int yagelExpectMapping(yaml_parser_t *parser);
int yagelSaveNextFloat(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1);
int yagelSaveNextString(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1);

#if defined(YAGEL_IMPLEMENT)

// returns > 0 on success
int yagelParse(yaml_parser_t *parser, intptr_t arg0, const YagelNode *nodes) {
#define MAX_EXPECTED_NODES_STACK_DEPTH 8
	const YagelNode *stack[MAX_EXPECTED_NODES_STACK_DEPTH];
	stack[0] = nodes;
	int stack_pos = 0;

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
			const YagelNode *nodes = stack[stack_pos];
			for (int i = 0; nodes[i].type != -1; ++i) {
				const YagelNode *node = nodes + i;
				//printf("%d %d E%d\n", i, nodes_num, node->type);
				if (node->type != event.type)
					continue;

				if (node->type == YAML_SCALAR_EVENT
						&& node->scalar
						&& 0 != strncmp(node->scalar, (const char*)event.data.scalar.value, event.data.scalar.length))
					continue;

				if (node->action && 0 >= node->action(parser, &event, arg0, node->arg1)) {
					yaml_event_delete(&event);
					return 0;
				}

				if (node->stop) {
					if (stack_pos == 0) {
						stop = 1;
					} else {
						--stack_pos;
					}
				}

				if (node->nest) {
					++stack_pos;
					if (stack_pos == MAX_EXPECTED_NODES_STACK_DEPTH) {
						fprintf(stderr, "Error: nodes stack overflow: %d is the max\n", MAX_EXPECTED_NODES_STACK_DEPTH);
						yaml_event_delete(&event);
						return 0;
					}
					stack[stack_pos] = node->nest;
				}

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

int yagelSaveString(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	if (event->type != YAML_SCALAR_EVENT) {
		fprintf(stderr, "%s expects scalar\n", __FUNCTION__);
		return 0;
	}

	*((char**)(arg0 + arg1)) = strndup((const char*)event->data.scalar.value, event->data.scalar.length);
	return 1;
}

int yagelSaveNextString(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	const YagelNode nodeReadNextString[] = {{.type = YAML_SCALAR_EVENT, .arg1 = arg1, .action = yagelSaveString, .stop = 1}, {.type = -1}};
	return yagelParse(parser, arg0, nodeReadNextString);
}

static int configParseReadFloat(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	if (event->type != YAML_SCALAR_EVENT) {
		fprintf(stderr, "%s expects scalar\n", __FUNCTION__);
		return 0;
	}

	// Y U NO strntof
	const size_t len = event->data.scalar.length;
	char buffer[16];
	char *value;
	if (len < 16) {
		memcpy(buffer, event->data.scalar.value, len);
		buffer[len] = '\0';
		value = buffer;
	} else {
		value = strndup((const char*)event->data.scalar.value, len);
	}

	char *endptr;
	*((float*)(arg0 + arg1)) = strtof(value, &endptr);

	int retval = 1;
	if (endptr != value + len) {
		fprintf(stderr, "error reading %s as float\n", value);
		retval = 0;
	}

	if (len >= 16)
		free(value);
	return retval;
}

int yagelSaveNextFloat(yaml_parser_t *parser, const yaml_event_t *event, intptr_t arg0, intptr_t arg1) {
	const YagelNode nodeReadNextString[] = {{.type = YAML_SCALAR_EVENT, .arg1 = arg1, .action = configParseReadFloat, .stop = 1}, {.type = -1}};
	return yagelParse(parser, arg0, nodeReadNextString);
}

static const YagelNode expectMapping[] = {{.type = YAML_MAPPING_START_EVENT, .stop = 1}, {.type = -1}};
int yagelExpectMapping(yaml_parser_t *parser) {
	return yagelParse(parser, 0, expectMapping);
}

#endif //defined(YAGEL_IMPLEMENT)
