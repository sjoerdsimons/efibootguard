/*
 * EFI Boot Guard
 *
 * Copyright (c) Siemens AG, 2017
 *
 * Authors:
 *  Andreas Reichel <andreas.reichel.ext@siemens.com>
 *  Michael Adler <michael.adler@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#include <sys/queue.h>
#include <sys/stat.h>

#include "env_api.h"
#include "ebgenv.h"
#include "uservars.h"
#include "version.h"
#include "env_config_file.h"

static char doc[] =
	"bg_setenv/bg_printenv - Environment tool for the EFI Boot Guard";

#define OPT(name, key, arg, flags, doc)                                        \
	{                                                                      \
		name, key, arg, flags, doc                                     \
	}

#define BG_CLI_OPTIONS_COMMON                                                  \
	OPT("filepath", 'f', "ENVFILE", 0,                                     \
	    "Environment to use. Expects a file name, "                        \
	    "usually called BGENV.DAT.")                                       \
	, OPT("part", 'p', "ENV_PART", 0,                                      \
	      "Set environment partition to update. If no partition is "       \
	      "specified, "                                                    \
	      "the one with the smallest revision value above zero is "        \
	      "updated.")                                                      \
	, OPT("verbose", 'v', 0, 0, "Be verbose")                              \
	, OPT("version", 'V', 0, 0, "Print version")

static struct argp_option options_setenv[] = {
	BG_CLI_OPTIONS_COMMON,
	OPT("preserve", 'P', 0, 0, "Preserve existing entries"),
	OPT("kernel", 'k', "KERNEL", 0, "Set kernel to load"),
	OPT("args", 'a', "KERNEL_ARGS", 0, "Set kernel arguments"),
	OPT("revision", 'r', "REVISION", 0, "Set revision value"),
	OPT("ustate", 's', "USTATE", 0, "Set update status for environment"),
	OPT("watchdog", 'w', "WATCHDOG_TIMEOUT", 0,
	    "Watchdog timeout in seconds"),
	OPT("confirm", 'c', 0, 0, "Confirm working environment"),
	OPT("update", 'u', 0, 0, "Automatically update oldest revision"),
	OPT("uservar", 'x', "KEY=VAL", 0,
	    "Set user-defined string variable. For setting multiple variables, "
	    "use this option multiple times."),
	OPT("in_progress", 'i', "IN_PROGRESS", 0,
	    "Set in_progress variable to simulate a running update process."),
	{},
};

static struct argp_option options_printenv[] = {
	BG_CLI_OPTIONS_COMMON,
	OPT("current", 'c', 0, 0,
	    "Only print values from the current environment"),
	OPT("output", 'o', "LIST", 0,
	    "Comma-separated list of fields which are printed. "
	    "Available fields: in_progress, revision, kernel, kernelargs, "
	    "watchdog_timeout, ustate, user. "
	    "If omitted, all available fields are printed."),
	{},
};

/* Common arguments used by both bg_setenv and bg_printenv. */
struct arguments_common {
	char *envfilepath;
	bool verbosity;
	/* which partition to operate on; a negative value means no partition
	 * was specified. */
	int which_part;
	bool part_specified;
};

/* Arguments used by bg_setenv. */
struct arguments_setenv {
	struct arguments_common common;
	/* auto update feature automatically updates partition with
	 * oldest environment revision (lowest value) */
	bool auto_update;
	/* whether to keep existing entries in BGENV before applying new
	 * settings */
	bool preserve_env;
};

struct fields {
	unsigned int in_progress : 1;
	unsigned int revision : 1;
	unsigned int kernel : 1;
	unsigned int kernelargs : 1;
	unsigned int wdog_timeout : 1;
	unsigned int ustate : 1;
	unsigned int user : 1;
};

static const struct fields ALL_FIELDS = {1, 1, 1, 1, 1, 1, 1};

/* Arguments used by bg_printenv. */
struct arguments_printenv {
	struct arguments_common common;
	bool current;
	/* a bitset to decide which fields are printed */
	struct fields output_fields;
};

typedef enum { ENV_TASK_SET, ENV_TASK_DEL } BGENV_TASK;

struct stailhead *headp;
struct env_action {
	char *key;
	uint64_t type;
	uint8_t *data;
	BGENV_TASK task;
	STAILQ_ENTRY(env_action) journal;
};

STAILQ_HEAD(stailhead, env_action) head = STAILQ_HEAD_INITIALIZER(head);

static void journal_free_action(struct env_action *action)
{
	if (!action) {
		return;
	}
	free(action->data);
	free(action->key);
	free(action);
}

static error_t journal_add_action(BGENV_TASK task, char *key, uint64_t type,
				  uint8_t *data, size_t datalen)
{
	struct env_action *new_action;

	new_action = calloc(1, sizeof(struct env_action));
	if (!new_action) {
		return ENOMEM;
	}
	new_action->task = task;
	if (key) {
		if (asprintf(&(new_action->key), "%s", key) == -1) {
			new_action->key = NULL;
			goto newaction_nomem;
		}
	}
	new_action->type = type;
	if (data && datalen) {
		new_action->data = (uint8_t *)malloc(datalen);
		if (!new_action->data) {
			new_action->data = NULL;
			goto newaction_nomem;
		}
		memcpy(new_action->data, data, datalen);
	}
	STAILQ_INSERT_TAIL(&head, new_action, journal);
	return 0;

newaction_nomem:
	journal_free_action(new_action);
	return ENOMEM;
}

static void journal_process_action(BGENV *env, struct env_action *action)
{
	ebgenv_t e;
	char *tmp;

	switch (action->task) {
	case ENV_TASK_SET:
		VERBOSE(stdout, "Task = SET, key = %s, type = %llu, val = %s\n",
			action->key, (long long unsigned int)action->type,
			(char *)action->data);
		if (strncmp(action->key, "ustate", strlen("ustate")+1) == 0) {
			uint16_t ustate;
			unsigned long t;
			char *arg;
			int ret;
			e.bgenv = env;
			arg = (char *)action->data;
			errno = 0;
			t = strtol(arg, &tmp, 10);
			if ((errno == ERANGE && (t == LONG_MAX ||
			                         t == LONG_MIN)) ||
			    (errno != 0 && t == 0) || tmp == arg) {
				fprintf(stderr, "Invalid value for ustate: %s",
						(char *)action->data);
				return;
			}
			ustate = (uint16_t)t;;
			if ((ret = ebg_env_setglobalstate(&e, ustate)) != 0) {
				fprintf(stderr,
					"Error setting global state: %s.",
					strerror(-ret));
			}
			return;
		}
		bgenv_set(env, action->key, action->type, action->data,
			  strlen((char *)action->data) + 1);
		break;
	case ENV_TASK_DEL:
		VERBOSE(stdout, "Task = DEL, key = %s\n", action->key);
		bgenv_set(env, action->key, action->type, "", 1);
		break;
	}
}

static char *ustatemap[] = {"OK", "INSTALLED", "TESTING", "FAILED", "UNKNOWN"};

static uint8_t str2ustate(char *str)
{
	uint8_t i;

	if (!str) {
		return USTATE_UNKNOWN;
	}
	for (i = USTATE_MIN; i < USTATE_MAX; i++) {
		if (strncasecmp(str, ustatemap[i], strlen(ustatemap[i])) == 0) {
			return i;
		}
	}
	return USTATE_UNKNOWN;
}

static char *ustate2str(uint8_t ustate)
{
	if (ustate > USTATE_MAX) {
		ustate = USTATE_MAX;
	}
	return ustatemap[ustate];
}

static error_t set_uservars(char *arg)
{
	char *key, *value;

	key = strtok(arg, "=");
	if (key == NULL) {
		return 0;
	}

	value = strtok(NULL, "=");
	if (value == NULL) {
		return journal_add_action(ENV_TASK_DEL, key,
					  USERVAR_TYPE_DEFAULT |
					  USERVAR_TYPE_DELETED, NULL, 0);
	}
	return journal_add_action(ENV_TASK_SET, key, USERVAR_TYPE_DEFAULT |
				  USERVAR_TYPE_STRING_ASCII,
				  (uint8_t *)value, strlen(value) + 1);
}

static int parse_int(char *arg)
{
	char *tmp;
	long i;

	errno = 0;
	i = strtol(arg, &tmp, 10);
	if (errno == ERANGE ||             /* out of range */
	    (errno != 0 && i == 0) ||      /* no conversion was performed */
	    tmp == arg || *tmp != '\0' ||  /* invalid input */
	    i < INT_MIN || i > INT_MAX) {  /* not a valid int */
		errno = EINVAL;
		return -1;
	}
	return (int) i;
}

static error_t parse_common_opt(int key, char *arg, bool compat_mode,
				struct arguments_common *arguments)
{
	bool found = false;
	int i;
	switch (key) {
	case 'f':
		found = true;
		free(arguments->envfilepath);
		arguments->envfilepath = NULL;

		if (compat_mode) {
			/* compat mode, permitting "bg_setenv -f <dir>" */
			struct stat sb;

			int res = stat(arg, &sb);
			if (res == 0 && S_ISDIR(sb.st_mode)) {
				fprintf(stderr,
					"WARNING: Using -f to specify only the "
					"ouptut directory is deprecated.\n");
				res = asprintf(&arguments->envfilepath, "%s/%s",
					       arg, FAT_ENV_FILENAME);
				if (res == -1) {
					return ENOMEM;
				}
			}
		}

		if (!arguments->envfilepath) {
			arguments->envfilepath = strdup(arg);
			if (!arguments->envfilepath) {
				return ENOMEM;
			}
		}
		break;
	case 'p':
		found = true;
		i = parse_int(arg);
		if (errno) {
			fprintf(stderr, "Invalid number specified for -p.\n");
			return 1;
		}
		if (i >= 0 && i < ENV_NUM_CONFIG_PARTS) {
			arguments->which_part = i;
			arguments->part_specified = true;
		} else {
			fprintf(stderr,
				"Selected partition out of range. Valid range: "
				"0..%d.\n",
				ENV_NUM_CONFIG_PARTS - 1);
			return 1;
		}
		break;
	case 'v':
		found = true;
		/* Set verbosity in this program */
		arguments->verbosity = true;
		/* Set verbosity in the library */
		bgenv_be_verbose(true);
		break;
	case 'V':
		found = true;
		fprintf(stdout, "EFI Boot Guard %s\n", EFIBOOTGUARD_VERSION);
		exit(0);
	}
	if (!found) {
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static error_t parse_setenv_opt(int key, char *arg, struct argp_state *state)
{
	struct arguments_setenv *arguments = state->input;
	int i, res;
	char *tmp;
	error_t e = 0;

	switch (key) {
	case 'k':
		if (strlen(arg) > ENV_STRING_LENGTH) {
			fprintf(stderr, "Error, kernel filename is too long. "
					"Maximum of %d "
					"characters permitted.\n",
				ENV_STRING_LENGTH);
			return 1;
		}
		e = journal_add_action(ENV_TASK_SET, "kernelfile", 0,
				       (uint8_t *)arg, strlen(arg) + 1);
		break;
	case 'a':
		if (strlen(arg) > ENV_STRING_LENGTH) {
			fprintf(stderr,
				"Error, kernel arguments string is too long. "
				"Maximum of %d characters permitted.\n",
				ENV_STRING_LENGTH);
			return 1;
		}
		e = journal_add_action(ENV_TASK_SET, "kernelparams", 0,
				       (uint8_t *)arg, strlen(arg) + 1);
		break;
	case 's':
		i = parse_int(arg);
		if (errno) {
			// maybe user specified an enum string
			i = str2ustate(arg);
			if (i == USTATE_UNKNOWN) {
				fprintf(stderr, "Invalid state specified.\n");
				return 1;
			}
		}
		if (i < 0 || i > 3) {
			fprintf(
			    stderr,
			    "Invalid ustate value specified. Possible values: "
			    "0 (%s), 1 (%s), 2 (%s), 3 (%s)\n",
			    ustatemap[0], ustatemap[1], ustatemap[2],
			    ustatemap[3]);
			return 1;
		} else {
			res = asprintf(&tmp, "%u", i);
			if (res == -1) {
				return ENOMEM;
			}
			e = journal_add_action(ENV_TASK_SET, "ustate", 0,
					       (uint8_t *)tmp, strlen(tmp) + 1);
			free(tmp);
			VERBOSE(stdout, "Ustate set to %d (%s).\n", i,
				ustate2str(i));
		}
		break;
	case 'i':
		i = parse_int(arg);
		if (errno) {
			fprintf(stderr, "Invalid value specified.\n");
			return 1;
		}
		if (i < 0 || i > 1) {
			fprintf(stderr,
				"Invalid value specified. Possible values: "
				"0 (no), 1 (yes)\n");
			return 1;
		} else {
			res = asprintf(&tmp, "%u", i);
			if (res == -1) {
				return ENOMEM;
			}
			e = journal_add_action(ENV_TASK_SET, "in_progress", 0,
					       (uint8_t *)tmp, strlen(tmp) + 1);
			free(tmp);
			VERBOSE(stdout, "in_progress set to %d.\n", i);
		}
		break;
	case 'r':
		i = parse_int(arg);
		if (errno) {
			fprintf(stderr, "Invalid revision specified.\n");
			return 1;
		}
		VERBOSE(stdout, "Revision is set to %u.\n", (unsigned int) i);
		e = journal_add_action(ENV_TASK_SET, "revision", 0,
				       (uint8_t *)arg, strlen(arg) + 1);
		break;
	case 'w':
		i = parse_int(arg);
		if (errno || i < 0) {
			fprintf(stderr,
				"Invalid watchdog timeout specified.\n");
			return 1;
		}
		VERBOSE(stdout,
			"Setting watchdog timeout to %d seconds.\n", i);
		e = journal_add_action(ENV_TASK_SET,
				       "watchdog_timeout_sec", 0,
				       (uint8_t *)arg, strlen(arg) + 1);
		break;
	case 'c':
		VERBOSE(stdout,
			"Confirming environment to work. Removing boot-once "
			"and testing flag.\n");
		e = journal_add_action(ENV_TASK_SET, "ustate", 0,
				       (uint8_t *)"0", 2);
		break;
	case 'u':
		arguments->auto_update = true;
		break;
	case 'x':
		/* Set user-defined variable(s) */
		e = set_uservars(arg);
		break;
	case 'P':
		arguments->preserve_env = true;
		break;
	case ARGP_KEY_ARG:
		/* too many arguments - program terminates with call to
		 * argp_usage with non-zero return code */
		argp_usage(state);
		break;
	default:
		return parse_common_opt(key, arg, true, &arguments->common);
	}

	if (e) {
		fprintf(stderr, "Error creating journal: %s\n", strerror(e));
	}
	return e;
}

static error_t parse_output_fields(char *fields, struct fields *output_fields)
{
	char *token;
	memset(output_fields, 0, sizeof(struct fields));
	while ((token = strsep(&fields, ","))) {
		if (*token == '\0') continue;
		if (strcmp(token, "in_progress") == 0) {
			output_fields->in_progress = true;
		} else if (strcmp(token, "revision") == 0) {
			output_fields->revision = true;
		} else if (strcmp(token, "kernel") == 0) {
			output_fields->kernel = true;
		} else if (strcmp(token, "kernelargs") == 0) {
			output_fields->kernelargs = true;
		} else if (strcmp(token, "watchdog_timeout") == 0) {
			output_fields->wdog_timeout = true;
		} else if (strcmp(token, "ustate") == 0) {
			output_fields->ustate = true;
		} else if (strcmp(token, "user") == 0) {
			output_fields->user = true;
		} else {
			fprintf(stderr, "Unknown output field: %s\n", token);
			return 1;
		}
	}
	return 0;
}

static error_t parse_printenv_opt(int key, char *arg, struct argp_state *state)
{
	struct arguments_printenv *arguments = state->input;
	error_t e = 0;

	switch (key) {
	case 'c':
		arguments->current = true;
		break;
	case 'o':
		e = parse_output_fields(arg, &arguments->output_fields);
		break;
	case ARGP_KEY_ARG:
		/* too many arguments - program terminates with call to
		 * argp_usage with non-zero return code */
		argp_usage(state);
		break;
	default:
		return parse_common_opt(key, arg, false, &arguments->common);
	}

	return e;
}

static void dump_uservars(uint8_t *udata)
{
	char *key, *value;
	uint64_t type;
	uint32_t rsize, dsize;
	uint64_t val_unum;
	int64_t val_snum;

	while (*udata) {
		bgenv_map_uservar(udata, &key, &type, (uint8_t **)&value,
				  &rsize, &dsize);
		fprintf(stdout, "%s ", key);
		type &= USERVAR_STANDARD_TYPE_MASK;
		if (type == USERVAR_TYPE_STRING_ASCII) {
			fprintf(stdout, "= %s\n", value);
		} else if (type >= USERVAR_TYPE_UINT8 &&
			   type <= USERVAR_TYPE_UINT64) {
			switch(type) {
			case USERVAR_TYPE_UINT8:
				val_unum = *((uint8_t *) value);
				break;
			case USERVAR_TYPE_UINT16:
				val_unum = *((uint16_t *) value);
				break;
			case USERVAR_TYPE_UINT32:
				val_unum = *((uint32_t *) value);
				break;
			case USERVAR_TYPE_UINT64:
				val_unum = *((uint64_t *) value);
				break;
			}
			fprintf(stdout, "= %llu\n",
				(long long unsigned int) val_unum);
		} else if (type >= USERVAR_TYPE_SINT8 &&
			   type <= USERVAR_TYPE_SINT64) {
			switch(type) {
			case USERVAR_TYPE_SINT8:
				val_snum = *((int8_t *) value);
				break;
			case USERVAR_TYPE_SINT16:
				val_snum = *((int16_t *) value);
				break;
			case USERVAR_TYPE_SINT32:
				val_snum = *((int32_t *) value);
				break;
			case USERVAR_TYPE_SINT64:
				val_snum = *((int64_t *) value);
				break;
			}
			fprintf(stdout, "= %lld\n",
				(long long signed int) val_snum);
		} else {
			switch(type) {
			case USERVAR_TYPE_CHAR:
				fprintf(stdout, "= %c\n", (char) *value);
				break;
			case USERVAR_TYPE_BOOL:
				fprintf(stdout, "= %s\n",
				       (bool) *value ? "true" : "false");
				break;
			default:
				fprintf(stdout, "( Type is not printable )\n");
			}
		}

		udata = bgenv_next_uservar(udata);
	}
}

static void dump_env(BG_ENVDATA *env, struct fields output_fields)
{
	char buffer[ENV_STRING_LENGTH];
	fprintf(stdout, "Values:\n");
	if (output_fields.in_progress) {
		fprintf(stdout, "in_progress:      %s\n",
			env->in_progress ? "yes" : "no");
	}
	if (output_fields.revision) {
		fprintf(stdout, "revision:         %u\n", env->revision);
	}
	if (output_fields.kernel) {
		fprintf(stdout, "kernel:           %s\n",
			str16to8(buffer, env->kernelfile));
	}
	if (output_fields.kernelargs) {
		fprintf(stdout, "kernelargs:       %s\n",
			str16to8(buffer, env->kernelparams));
	}
	if (output_fields.wdog_timeout) {
		fprintf(stdout, "watchdog timeout: %u seconds\n",
			env->watchdog_timeout_sec);
	}
	if (output_fields.ustate) {
		fprintf(stdout, "ustate:           %u (%s)\n",
			(uint8_t)env->ustate, ustate2str(env->ustate));
	}
	if (output_fields.user) {
		fprintf(stdout, "\n");
		fprintf(stdout, "user variables:\n");
		dump_uservars(env->userdata);
	}
	fprintf(stdout, "\n\n");
}

static void update_environment(BGENV *env, bool verbosity)
{
	if (verbosity) {
		fprintf(stdout, "Processing journal...\n");
	}

	while (!STAILQ_EMPTY(&head)) {
		struct env_action *action = STAILQ_FIRST(&head);

		journal_process_action(env, action);
		STAILQ_REMOVE_HEAD(&head, journal);
		journal_free_action(action);
	}

	env->data->crc32 = crc32(0, (const Bytef *)env->data,
				 sizeof(BG_ENVDATA) - sizeof(env->data->crc32));

}

static void dump_envs(struct fields output_fields)
{
	for (int i = 0; i < ENV_NUM_CONFIG_PARTS; i++) {
		fprintf(stdout, "\n----------------------------\n");
		fprintf(stdout, " Config Partition #%d ", i);
		BGENV *env = bgenv_open_by_index(i);
		if (!env) {
			fprintf(stderr, "Error, could not read environment "
					"for index %d\n",
				i);
			return;
		}
		dump_env(env->data, output_fields);
		bgenv_close(env);
	}
}

static void dump_latest_env(struct fields output_fields)
{
	BGENV *env = bgenv_open_latest();
	if (!env) {
		fprintf(stderr, "Failed to retrieve latest environment.\n");
		return;
	}
	dump_env(env->data, output_fields);
	bgenv_close(env);
}

static void dump_env_by_index(uint32_t index, struct fields output_fields)
{
	BGENV *env = bgenv_open_by_index(index);
	if (!env) {
		fprintf(stderr, "Failed to retrieve latest environment.\n");
		return;
	}
	dump_env(env->data, output_fields);
	bgenv_close(env);
}

static bool get_env(char *configfilepath, BG_ENVDATA *data)
{
	FILE *config;
	bool result = true;

	if (!(config = open_config_file(configfilepath, "rb"))) {
		return false;
	}

	if (!(fread(data, sizeof(BG_ENVDATA), 1, config) == 1)) {
		VERBOSE(stderr, "Error reading environment data from %s\n",
			configfilepath);
		if (feof(config)) {
			VERBOSE(stderr, "End of file encountered.\n");
		}
		result = false;
	}

	if (close_config_file(config)) {
		VERBOSE(stderr,
			"Error closing environment file after reading.\n");
	};
	return result;
}

static int printenv_from_file(char *envfilepath, struct fields output_fields)
{
	int success = 0;
	BG_ENVDATA data;

	success = get_env(envfilepath, &data);
	if (success) {
		dump_env(&data, output_fields);
		return 0;
	} else {
		fprintf(stderr, "Error reading environment file.\n");
		return 1;
	}
}

static int dumpenv_to_file(char *envfilepath, bool verbosity, bool preserve_env)
{
	/* execute journal and write to file */
	int result = 0;
	BGENV env;
	BG_ENVDATA data;

	memset(&env, 0, sizeof(BGENV));
	memset(&data, 0, sizeof(BG_ENVDATA));
	env.data = &data;

	if (preserve_env && !get_env(envfilepath, &data)) {
		return 1;
	}

	update_environment(&env, verbosity);
	if (verbosity) {
		dump_env(env.data, ALL_FIELDS);
	}
	FILE *of = fopen(envfilepath, "wb");
	if (of) {
		if (fwrite(&data, sizeof(BG_ENVDATA), 1, of) != 1) {
			fprintf(stderr,
				"Error writing to output file: %s\n",
				strerror(errno));
			result = 1;
		} else {
			fprintf(stdout, "Output written to %s.\n", envfilepath);
		}
		if (fclose(of)) {
			fprintf(stderr, "Error closing output file.\n");
			result = 1;
		};
	} else {
		fprintf(stderr, "Error opening output file %s (%s).\n",
			envfilepath, strerror(errno));
		result = 1;
	}

	return result;
}

/* This is the entrypoint for the command bg_printenv. */
static error_t bg_printenv(int argc, char **argv)
{
	struct argp argp_printenv = {
		.options = options_printenv,
		.parser = parse_printenv_opt,
		.doc = doc,
	};

	struct arguments_printenv arguments = {
		.output_fields = ALL_FIELDS,
	};

	error_t e = argp_parse(&argp_printenv, argc, argv, 0, 0, &arguments);
	if (e) {
		return e;
	}

	const struct arguments_common *common = &arguments.common;

	/* count the number of arguments which result in bg_printenv
	 * operating on a single partition; to avoid ambiguity, we only
	 * allow one such argument. */
	int counter = 0;
	if (common->envfilepath) ++counter;
	if (common->part_specified) ++counter;
	if (arguments.current) ++counter;
	if (counter > 1) {
		fprintf(stderr, "Error, only one of -c/-f/-p can be set.\n");
		return 1;
	}

	if (common->envfilepath) {
		e = printenv_from_file(common->envfilepath,
				       arguments.output_fields);
		free(common->envfilepath);
		return e;
	}

	/* not in file mode */
	if (!bgenv_init()) {
		fprintf(stderr, "Error initializing FAT environment.\n");
		return 1;
	}

	if (arguments.current) {
		fprintf(stdout, "Using latest config partition\n");
		dump_latest_env(arguments.output_fields);
	} else if (common->part_specified) {
		fprintf(stdout, "Using config partition #%d\n",
			arguments.common.which_part);
		dump_env_by_index(common->which_part, arguments.output_fields);
	} else {
		dump_envs(arguments.output_fields);
	}

	bgenv_finalize();
	return 0;
}

/* This is the entrypoint for the command bg_setenv. */
static error_t bg_setenv(int argc, char **argv)
{
	if (argc < 2) {
		printf("No task to perform. Please specify at least one"
		       " optional argument. See --help for further"
		       " information.\n");
		return 1;
	}

	struct argp argp_setenv = {
		.options = options_setenv,
		.parser = parse_setenv_opt,
		.doc = doc,
	};

	struct arguments_setenv arguments;
	memset(&arguments, 0, sizeof(struct arguments_setenv));

	STAILQ_INIT(&head);

	error_t e;
	e = argp_parse(&argp_setenv, argc, argv, 0, 0, &arguments);
	if (e) {
		return e;
	}

	if (arguments.auto_update && arguments.common.part_specified) {
		fprintf(stderr, "Error, both automatic and manual partition "
				"selection. Cannot use -p and -u "
				"simultaneously.\n");
		return 1;
	}

	int result = 0;

	/* arguments are parsed, journal is filled */

	/* is output to file or input from file ? */
	if (arguments.common.envfilepath) {
		result = dumpenv_to_file(arguments.common.envfilepath,
					 arguments.common.verbosity,
					 arguments.preserve_env);
		free(arguments.common.envfilepath);
		return result;
	}

	/* not in file mode */
	if (!bgenv_init()) {
		fprintf(stderr, "Error initializing FAT environment.\n");
		return 1;
	}

	if (arguments.common.verbosity) {
		dump_envs(ALL_FIELDS);
	}

	BGENV *env_new = NULL;
	BGENV *env_current;

	if (arguments.auto_update) {
		/* clone latest environment */

		env_current = bgenv_open_latest();
		if (!env_current) {
			fprintf(stderr, "Failed to retrieve latest environment."
					"\n");
			result = 1;
			goto cleanup;
		}
		env_new = bgenv_open_oldest();
		if (!env_new) {
			fprintf(stderr, "Failed to retrieve oldest environment."
					"\n");
			bgenv_close(env_current);
			result = 1;
			goto cleanup;
		}
		if (arguments.common.verbosity) {
			fprintf(stdout,
				"Updating environment with revision %u\n",
				env_new->data->revision);
		}

		if (!env_current->data || !env_new->data) {
			fprintf(stderr, "Invalid environment data pointer.\n");
			bgenv_close(env_current);
			result = 1;
			goto cleanup;
		}

		memcpy((char *)env_new->data, (char *)env_current->data,
		       sizeof(BG_ENVDATA));
		env_new->data->revision = env_current->data->revision + 1;

		bgenv_close(env_current);
	} else {
		if (arguments.common.part_specified) {
			env_new = bgenv_open_by_index(
				arguments.common.which_part);
		} else {
			env_new = bgenv_open_latest();
		}
		if (!env_new) {
			fprintf(stderr, "Failed to retrieve environment by "
					"index.\n");
			result = 1;
			goto cleanup;
		}
	}

	update_environment(env_new, arguments.common.verbosity);

	if (arguments.common.verbosity) {
		fprintf(stdout, "New environment data:\n");
		fprintf(stdout, "---------------------\n");
		dump_env(env_new->data, ALL_FIELDS);
	}
	if (!bgenv_write(env_new)) {
		fprintf(stderr, "Error storing environment.\n");
		result = 1;
	} else {
		fprintf(stdout, "Environment update was successful.\n");
	}

cleanup:
	bgenv_close(env_new);
	bgenv_finalize();
	return result;
}

int main(int argc, char **argv)
{
	if (strstr(argv[0], "bg_setenv")) {
		return bg_setenv(argc, argv);
	} else {
		return bg_printenv(argc, argv);
	}
}
