/* PANDAseq -- Assemble paired FASTQ Illumina reads and strip the region between amplification primers.
     Copyright (C) 2011-2012  Andre Masella

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */
#include<ltdl.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include"pandaseq.h"
#include"assembler.h"
#ifdef HAVE_PTHREAD
#include <pthread.h>
/* All modules share a single mutex to control reference counts */
static pthread_mutex_t ref_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

struct panda_module {
	volatile size_t refcnt;

	PandaCheck check;
	PandaPreCheck precheck;
	PandaDestroy destroy;
	void *user_data;

	lt_dlhandle handle;
	char *name;
	bool name_free;
	char *args;

	int api;
	char **version;
};

bool module_checkseq(PandaAssembler assembler, panda_result_seq *sequence)
{
	int it;
	for (it = 0; it < assembler->modules_length; it++) {
		PandaModule module = assembler->modules[it];
		if (!module->check(sequence, module->user_data)) {
			assembler->rejected[it]++;
			return false;
		}
	}
	return true;
}

bool module_precheckseq(PandaAssembler assembler, panda_seq_identifier *id, const panda_qual *forward, size_t forward_length, const panda_qual *reverse, size_t reverse_length)
{
	int it;
	for (it = 0; it < assembler->modules_length; it++) {
		PandaModule module = assembler->modules[it];
		if (!module->precheck(id, forward, forward_length, reverse, reverse_length, module->user_data)) {
			assembler->rejected[it]++;
			return false;
		}
	}
	return true;
}

void module_init(PandaAssembler assembler)
{
	int it;
	LOG(assembler, PANDA_CODE_API_VERSION, PANDA_API);
	for (it = 0; it < assembler->modules_length; it++) {
		PandaModule module = assembler->modules[it];
		if (module->handle != NULL) {
			const lt_dlinfo *info = lt_dlgetinfo(module->handle);
			LOG(assembler, PANDA_CODE_MOD_INFO, info == NULL ? "unknown" : info->name, module->version == NULL ? "?" : *(module->version), module->api, module->args);
			assembler->rejected[it] = 0;
		}
	}
}

void panda_assembler_module_stats(PandaAssembler assembler)
{
	int it;
	for (it = 0; it < assembler->modules_length; it++) {
		LOG(assembler, PANDA_CODE_REJECT_STAT, assembler->modules[it], assembler->rejected[it]);
	}
}

void module_destroy(PandaAssembler assembler)
{
	int it;
	LOG(assembler, PANDA_CODE_API_VERSION, PANDA_API);
	free(assembler->rejected);
	for (it = 0; it < assembler->modules_length; it++) {
		panda_module_unref(assembler->modules[it]);
	}
	assembler->modules_length = 0;
	free(assembler->modules);
}

static volatile int ltdl_count = 0;
static bool ref_ltdl() {
#ifdef HAVE_PTHREAD
	pthread_mutex_lock(&ref_lock);
#endif
	if (ltdl_count == 0) {
		if (lt_dlinit() != 0) {
#ifdef HAVE_PTHREAD
			pthread_mutex_unlock(&ref_lock);
#endif
			return false;
		}
	}
	ltdl_count++;
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ref_lock);
#endif
	return true;
}
static void unref_ltdl() {
#ifdef HAVE_PTHREAD
	pthread_mutex_lock(&ref_lock);
#endif
	if (--ltdl_count == 0) {
		lt_dlexit();
	}
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ref_lock);
#endif
}

PandaModule panda_module_load(char *path)
{
	PandaModule m;
	lt_dlhandle handle;
	bool (*init) (char *args);
	PandaCheck check;
	int *api;
	char **version;
	char *name;
	char *args;

	name = malloc(strlen(path));
	memcpy(name, path, strlen(path));
	args = name;
	while (*args != '\0' && *args != LT_PATHSEP_CHAR) {
		args++;
	}
	if (*args == '\0') {
		args = NULL;
	} else {
		*args = '\0';
		args++;
	}

	if (!ref_ltdl()) {
		return NULL;
	}

	handle = lt_dlopenext(name);
	if (handle == NULL) {
		fprintf(stderr, "Could not open module %s: %s\n", name,
			lt_dlerror());
		free(name);
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ref_lock);
#endif
		unref_ltdl();
		return NULL;
	}

	api = lt_dlsym(handle, "api");
	if (api == NULL || *api > PANDA_API) {
		lt_dlclose(handle);
		fprintf(stderr, "Invalid API in %s. Are you sure this module was compiled for this version of PANDAseq?\n", name);
		free(name);
		unref_ltdl();
		return NULL;
	}

	*(void **)(&check) = lt_dlsym(handle, "check");
	if (check == NULL) {
		lt_dlclose(handle);
		fprintf(stderr, "Could not find check function in %s\n", name);
		free(name);
		unref_ltdl();
		return NULL;
	}

	*(void **)(&init) = lt_dlsym(handle, "init");
	if (init != NULL && !init(args)) {
		free(name);
		unref_ltdl();
		return NULL;
	}

	m = malloc(sizeof(struct panda_module));
	m->api = *api;
	m->args = args;
	m->check = check;
	m->handle = handle;
	m->name = name;
	m->name_free = false;
	m->refcnt = 1;
	m->user_data = NULL;
	m->version = lt_dlsym(handle, "version");
	*(void **)(&m->destroy) = lt_dlsym(handle, "destroy");
	*(void **)(&m->precheck) = lt_dlsym(handle, "precheck");
	return m;
}

PandaModule panda_module_new(char *name, PandaCheck check, PandaPreCheck precheck, void *user_data, PandaDestroy cleanup)
{
	PandaModule m;
	if (check == NULL)
		return NULL;
	m = malloc(sizeof(struct panda_module));
	m->api = PANDA_API;
	m->args = NULL;
	m->check = check;
	m->destroy = cleanup;
	m->handle = NULL;
	m->name = malloc(strlen(name));
	memcpy(m->name, name, strlen(name));
	m->name_free = true;
	m->precheck = precheck;
	m->refcnt = 1;
	m->user_data = user_data;
	m->version = NULL;
	return m;
}
	
PandaModule panda_module_ref(PandaModule module) {
#ifdef HAVE_PTHREAD
	pthread_mutex_lock(&ref_lock);
#endif
	module->refcnt++;
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ref_lock);
#endif
	return module;
}

void panda_module_unref(PandaModule module) {
	int count;
#ifdef HAVE_PTHREAD
	pthread_mutex_lock(&ref_lock);
#endif
	count = --(module->refcnt);
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ref_lock);
#endif
	if(count == 0) {
		if (module->destroy != NULL)
			module->destroy(module->user_data);
		if (module->name != NULL && module->name_free)
			free(module->name);
		if (module->handle != NULL)
			lt_dlclose(module->handle);
		free(module);
		unref_ltdl();
	}
}

const char *panda_module_get_name(PandaModule module) {
	return module->name;
}

const char *panda_module_get_args(PandaModule module) {
	return module->args;
}

int panda_module_get_api(PandaModule module) {
	return module->api;
}

const char *panda_module_get_version(PandaModule module) {
	return module->version == NULL ? NULL : *module->version;
}

const char *panda_module_get_description(PandaModule module) {
	char **val;
	const lt_dlinfo *info;
	if (module->handle == NULL)
		return NULL;
	info = lt_dlgetinfo(module->handle);
	val = lt_dlsym(module->handle, "desc");
	return val == NULL ? NULL : *val;
}

const char *panda_module_get_usage(PandaModule module) {
	char **val;
	const lt_dlinfo *info;
	if (module->handle == NULL)
		return NULL;
	info = lt_dlgetinfo(module->handle);
	val = lt_dlsym(module->handle, "usage");
	return val == NULL ? NULL : *val;
}

void panda_assembler_add_module(PandaAssembler assembler, PandaModule module) {
	if (module == NULL) {
		return;
	}
#ifdef HAVE_PTHREAD
	pthread_mutex_lock(&assembler->mutex);
#endif
	if (assembler->modules_length == assembler->modules_size) {
		if (assembler->modules_size == 0) {
			assembler->modules_size = 8;
		} else {
			assembler->modules_size *= 2;
		}
		assembler->modules = realloc(assembler->modules, assembler->modules_size * sizeof(PandaModule));
		assembler->rejected = realloc(assembler->rejected, assembler->modules_size * sizeof(long));
	}
	assembler->rejected[assembler->modules_length] = 0;
	assembler->modules[assembler->modules_length++] = panda_module_ref(module);
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&assembler->mutex);
#endif
}
