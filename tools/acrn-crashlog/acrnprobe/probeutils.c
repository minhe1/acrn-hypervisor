/*
 * Copyright (C) 2018 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Copyright (C) 2018 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <malloc.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <openssl/sha.h>
#include <time.h>
#include "property.h"
#include "fsutils.h"
#include "history.h"
#include "load_conf.h"
#include "log_sys.h"
#include "probeutils.h"

#define CRASH_CURRENT_LOG       "currentcrashlog"
#define STATS_CURRENT_LOG       "currentstatslog"
#define VM_CURRENT_LOG		"currentvmlog"

#define BOOTID_NODE		"/proc/sys/kernel/random/boot_id"
#define BOOTID_LOG		"currentbootid"

unsigned long long get_uptime(void)
{
	long long time_ns;
	struct timespec ts;
	int res;

	res = clock_gettime(CLOCK_BOOTTIME, &ts);
	if (res == -1)
		return res;

	time_ns = (long long)ts.tv_sec * 1000000000LL +
		  (long long)ts.tv_nsec;

	return time_ns;
}

int get_uptime_string(char *newuptime, int *hours)
{
	long long tm;
	int seconds, minutes;

	tm = get_uptime();
	if (tm == -1)
		return -1;

	/* seconds */
	*hours = (int)(tm / 1000000000LL);
	seconds = *hours % 60;

	/* minutes */
	*hours /= 60;
	minutes = *hours % 60;

	/* hours */
	*hours /= 60;

	return snprintf(newuptime, UPTIME_SIZE, "%04d:%02d:%02d", *hours,
			minutes, seconds);
}

int get_current_time_long(char *buf)
{
	time_t t;
	struct tm *time_val;

	time(&t);
	time_val = localtime((const time_t *)&t);
	if (!time_val)
		return -1;

	return strftime(buf, LONG_TIME_SIZE, "%Y-%m-%d/%H:%M:%S  ", time_val);
}

static int compute_key(char *key, size_t key_len, const char *seed)
{
	SHA256_CTX sha;
	char buf[VERSION_SIZE];
	long long time_ns;
	char *tmp_key = key;
	unsigned char results[SHA256_DIGEST_LENGTH];
	size_t i;

	if (!key || !seed)
		return -1;
	if (key_len > SHA256_DIGEST_LENGTH * 2 || !key_len)
		return -1;

	SHA256_Init(&sha);
	time_ns = get_uptime();
	snprintf(buf, VERSION_SIZE, "%s%s%lld", gbuildversion, guuid, time_ns);

	SHA256_Update(&sha, (unsigned char *)buf, strlen(buf));
	SHA256_Update(&sha, (unsigned char *)seed, strlen(seed));

	SHA256_Final(results, &sha);

	for (i = 0; i < key_len / 2; i++) {
		sprintf(tmp_key, "%02x", results[i]);
		tmp_key += 2;
	}
	*tmp_key = 0;

	return 0;
}

/**
 * Generate an event id with specified type.
 *
 * @param seed1 Seed1.
 * @param seed2 Seed2, this parameter will be ignored if the value is NULL.
 * @param type The type of key. The length of generated id will be 20
 *		characters if type is KEY_SHORT; 32 characters if type is
 *		KEY_LONG.
 *
 * @return a pointer to result haskkey if successful, or NULL if not.
 */
char *generate_event_id(const char *seed1, const char *seed2,
			enum key_type type)
{
	int ret;
	char *buf;
	char *key;
	size_t klen;

	if (!seed1)
		return NULL;

	if (type == KEY_SHORT)
		klen = SHORT_KEY_LENGTH;
	else if (type == KEY_LONG)
		klen = LONG_KEY_LENGTH;
	else
		return NULL;

	key = (char *)malloc(klen + 1);
	if (!key) {
		LOGE("failed to generate event id, out of memory\n");
		return NULL;
	}

	if (seed2) {
		if (asprintf(&buf, "%s%s", seed1, seed2) == -1) {
			LOGE("failed to generate event id, out of memory\n");
			free(key);
			return NULL;
		}
		ret = compute_key(key, klen, (const char *)buf);
		free(buf);
	} else {
		ret = compute_key(key, klen, seed1);
	}

	if (ret < 0) {
		LOGE("compute_key error\n");
		free(key);
		key = NULL;
	}

	return key;
}

/**
 * Reserve a dir for log storage.
 *
 * @param mode Mode for log storage.
 * @param[out] dir Prefix of dir path reserved.
 * @param[out] index of dir reserved.
 *
 * @return 0 if successful, or -1 if not.
 */
static int reserve_log_folder(enum e_dir_mode mode, char *dir,
				unsigned int *current)
{
	char path[512];
	int res;
	struct sender_t *crashlog;
	char *outdir;
	unsigned int maxdirs;

	crashlog = get_sender_by_name("crashlog");
	if (!crashlog)
		return -1;

	outdir = crashlog->outdir;

	switch (mode) {
	case MODE_CRASH:
		sprintf(path, "%s/%s", outdir, CRASH_CURRENT_LOG);
		sprintf(dir, "%s/%s", outdir, "crashlog");
		break;
	case MODE_STATS:
		sprintf(path, "%s/%s", outdir, STATS_CURRENT_LOG);
		sprintf(dir, "%s/%s", outdir, "stats");
		break;
	case MODE_VMEVENT:
		sprintf(path, "%s/%s", outdir, VM_CURRENT_LOG);
		sprintf(dir, "%s/%s", outdir, "vmevent");
		break;
	default:
		LOGW("Invalid mode %d\n", mode);
		return -1;
	}

	/* Read current value in file */
	res = file_read_int(path, current);
	if (res < 0)
		return res;

	maxdirs = atoi(crashlog->maxcrashdirs);
	/* Open file in read/write mode to update the new current */
	res = file_update_int(path, *current, maxdirs);
	if (res < 0)
		return res;


	return 0;
}

#define strcat_fmt(buf, fmt, ...) \
(__extension__ \
({ \
	char __buf[1024] = {'\0',}; \
	snprintf(__buf, sizeof(__buf), fmt, ##__VA_ARGS__); \
	strcat(buf, __buf); \
}) \
)

/**
 * Create a crashfile with given params.
 *
 * @param dir Where to generate crashfile.
 * @param event Event name.
 * @param hashkey Event id.
 * @param type Subtype of this event.
 * @param data* String obtained by get_data.
 */
void generate_crashfile(char *dir, char *event, char *hashkey,
			char *type, char *data0,
			char *data1, char *data2)
{
	char *buf;
	char *path;
	char datetime[LONG_TIME_SIZE];
	char uptime[UPTIME_SIZE];
	int hours;
	int ret;
	const int fmtsize = 128;
	int filesize;

	datetime[0] = 0;
	ret = get_current_time_long(datetime);
	if (ret <= 0)
		return;
	uptime[0] = 0;
	get_uptime_string(uptime, &hours);

	filesize = fmtsize + strlen(event) +
		   strlen(hashkey) + strlen(guuid) +
		   strlen(datetime) + strlen(uptime) +
		   strlen(gbuildversion) + strlen(type);
	if (data0)
		filesize += strlen(data0);
	if (data1)
		filesize += strlen(data1);
	if (data2)
		filesize += strlen(data2);

	buf = malloc(filesize);
	if (buf == NULL) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	memset(buf, 0, filesize);
	strcat_fmt(buf, "EVENT=%s\n", event);
	strcat_fmt(buf, "ID=%s\n", hashkey);
	strcat_fmt(buf, "DEVICEID=%s\n", guuid);
	strcat_fmt(buf, "DATE=%s\n", datetime);
	strcat_fmt(buf, "UPTIME=%s\n", uptime);
	strcat_fmt(buf, "BUILD=%s\n", gbuildversion);
	strcat_fmt(buf, "TYPE=%s\n", type);
	if (data0)
		strcat_fmt(buf, "DATA0=%s\n", data0);
	if (data1)
		strcat_fmt(buf, "DATA1=%s\n", data1);
	if (data2)
		strcat_fmt(buf, "DATA2=%s\n", data2);
	strcat(buf, "_END\n");

	ret = asprintf(&path, "%s/%s", dir, "crashfile");
	if (ret < 0) {
		LOGE("compute string failed, out of memory\n");
		free(buf);
		return;
	}

	ret = overwrite_file(path, buf);
	if (ret)
		LOGE("new crashfile (%s) fail, error (%s)\n", path,
		     strerror(errno));

	free(buf);
	free(path);
}

/**
 * Create a dir for log storage.
 *
 * @param mode Mode for log storage.
 * @param hashkey Event id.
 *
 * @return a pointer to generated path if successful, or NULL if not.
 */
char *generate_log_dir(enum e_dir_mode mode, char *hashkey)
{
	char *path;
	char dir[PATH_MAX];
	unsigned int current;
	int ret;

	ret = reserve_log_folder(mode, dir, &current);
	if (ret)
		return NULL;

	ret = asprintf(&path, "%s%d_%s", dir, current, hashkey);
	if (ret == -1) {
		LOGE("construct log path failed, out of memory\n");
		hist_raise_infoerror("DIR CREATE");
		return NULL;
	}

	ret = mkdir(path, 0777);
	if (ret == -1) {
		LOGE("Cannot create dir %s\n", path);
		hist_raise_infoerror("DIR CREATE");
		free(path);
		return NULL;
	}

	return path;
}

int is_boot_id_changed(void)
{
	void *boot_id;
	void *logged_boot_id;
	char logged_boot_id_path[PATH_MAX];
	unsigned long size;
	struct sender_t *crashlog;
	int res;
	int result = 1; /* returns changed by default */

	crashlog = get_sender_by_name("crashlog");
	if (!crashlog)
		return result;

	res = read_file(BOOTID_NODE, &size, &boot_id);
	if (res == -1 || !size)
		return result;

	snprintf(logged_boot_id_path, sizeof(logged_boot_id_path), "%s/%s",
		 crashlog->outdir, BOOTID_LOG);
	if (file_exists(logged_boot_id_path)) {
		res = read_file(logged_boot_id_path, &size, &logged_boot_id);
		if (res == -1 || !size)
			goto out;

		if (!strcmp((char *)logged_boot_id, (char *)boot_id))
			result = 0;

		free(logged_boot_id);
	}

	if (result)
		overwrite_file(logged_boot_id_path, boot_id);
out:
	free(boot_id);
	return result;
}
