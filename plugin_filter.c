#include<stdio.h>
#include<stdlib.h>
#include<pandaseq-plugin.h>

HELP("Filters sequences based on the contents of a file of ids, one sequence ID per line.", "filter[:file]");

VER_INFO("1.0");

#define BUFF_SIZE 1024
char buffer[BUFF_SIZE];
PandaSet set;

PRECHECK {
	return panda_idset_contains(set, id);
}

INIT {
	FILE *file;
	bool close = false;

	if (args == NULL || *args == '\0') {
		file = stdin;
	} else {
		file = fopen(args, "r");
		if (file == NULL) {
			perror(args);
			return false;
		}
	}
	set = panda_idset_new();
	while (fgets(buffer, BUFF_SIZE, file) != NULL) {
		int it;
		for (it = 0; buffer[it] != '\n'; it++) ;
		buffer[it] = '\0';

		if (!panda_idset_add_str(set, buffer[0] == '@' ? (buffer + 1) : buffer, PANDA_TAG_OPTIONAL, NULL, NULL)) {
			fprintf(stderr, "ERR\tFILTER\tBAD\t%s\n", buffer);
			if (close)
				fclose(file);
			return false;
		}
	}
	if (ferror(file)) {
		perror(args);
		if (close)
			fclose(file);
		return false;
	}
	if (close)
		fclose(file);
	return true;
}

CLEANUP {
	panda_idset_unref(set);
	return;
}