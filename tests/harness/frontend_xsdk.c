// The console's entry for tests/harness/frontend.c. frontend.txt beside the .xex gives the frontend its command line on
// the first line, as it would take it after the program name ("core 1500 [content]"), then one KEY=VALUE per line for
// its environment (FE_DIR, RAMDUMP, FE_CPU, ...). Lines starting with # are skipped. What the frontend prints, on
// stdout and stderr, goes to <title>.log beside the .xex (docs/GUIDE.md, "Files and paths").

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	MAX_ARGUMENTS = 8,
	LINE_BYTES = 512,
};

static const char CONFIG_NAME[] = "frontend.txt";

int frontend_main(int argc, char **argv);

static char command_line[LINE_BYTES];
static char *arguments[MAX_ARGUMENTS + 1];

// Drops the line's end, "\n" or "\r\n".
static void trim_line(char *line)
{
	size_t length_bytes = strlen(line);
	while (length_bytes > 0 && (line[length_bytes - 1] == '\n' || line[length_bytes - 1] == '\r'))
		line[--length_bytes] = '\0';
}

// The program name, then each word of line, which is cut at its spaces; returns the count.
static int split_arguments(char *line)
{
	static char program_name[] = "swanstation_test";
	int argc = 0;
	arguments[argc++] = program_name;
	while (argc < MAX_ARGUMENTS) {
		while (*line == ' ')
			line++;
		if (*line == '\0')
			break;
		arguments[argc++] = line;
		while (*line != '\0' && *line != ' ')
			line++;
		if (*line != '\0')
			*line++ = '\0';
	}
	arguments[argc] = NULL;
	return argc;
}

static int read_config(FILE *file, int *argc)
{
	if (fgets(command_line, sizeof command_line, file) == NULL) {
		fprintf(stderr, "%s is empty\n", CONFIG_NAME);
		return 1;
	}
	trim_line(command_line);
	*argc = split_arguments(command_line);

	char line[LINE_BYTES];
	while (fgets(line, sizeof line, file) != NULL) {
		trim_line(line);
		if (line[0] == '\0' || line[0] == '#')
			continue;
		char *const equals = strchr(line, '=');
		if (equals == NULL || equals == line) {
			fprintf(stderr, "%s: \"%s\" is not KEY=VALUE\n", CONFIG_NAME, line);
			return 1;
		}
		*equals = '\0';
		if (setenv(line, equals + 1, 1) != 0) {
			fprintf(stderr, "%s: setenv(%s) failed\n", CONFIG_NAME, line);
			return 1;
		}
	}
	return 0;
}

int main(void)
{
	// A line at a time, so a hang or crash still leaves what came before in the log.
	setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

	FILE *const file = fopen(CONFIG_NAME, "r");
	if (file == NULL) {
		fprintf(stderr, "no %s beside the .xex\n", CONFIG_NAME);
		return 1;
	}
	int argc = 0;
	const int config_failed = read_config(file, &argc);
	fclose(file);
	if (config_failed)
		return 1;

	const int status = frontend_main(argc, arguments);
	fprintf(stderr, "swanstation_test: the frontend returned %d\n", status);
	return status;
}
