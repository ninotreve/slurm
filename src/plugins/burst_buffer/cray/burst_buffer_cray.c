/*****************************************************************************\
 *  burst_buffer_cray.c - Plugin for managing a Cray burst_buffer
 *****************************************************************************
 *  Copyright (C) 2014-2015 SchedMD LLC.
 *  Written by Morris Jette <jette@schedmd.com>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#define _GNU_SOURCE	/* For POLLRDHUP */
#include <ctype.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#if HAVE_JSON
#  include <json-c/json.h>
#endif

#include "slurm/slurm.h"

#include "src/common/assoc_mgr.h"
#include "src/common/fd.h"
#include "src/common/list.h"
#include "src/common/pack.h"
#include "src/common/parse_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/timers.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/state_save.h"
#include "src/plugins/burst_buffer/common/burst_buffer_common.h"

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "burst_buffer" for SLURM burst_buffer) and <method> is a
 * description of how this plugin satisfies that application.  SLURM will only
 * load a burst_buffer plugin if the plugin_type string has a prefix of
 * "burst_buffer/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[]        = "burst_buffer cray plugin";
const char plugin_type[]        = "burst_buffer/cray";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

/* Most state information is in a common structure so that we can more
 * easily use common functions from multiple burst buffer plugins */
static bb_state_t 	bb_state;
static char *		state_save_loc = NULL;

/* Description of each Cray DW configuration entry
 */
typedef struct bb_configs {
	uint32_t id;
	uint32_t instance;
} bb_configs_t;

/* Description of each Cray DW instance entry, including persistent buffers
 */
typedef struct bb_instances {
	uint32_t id;
	uint32_t bytes;
	char *label;
} bb_instances_t;

/* Description of each Cray DW pool entry
 */
typedef struct bb_pools {
	char *id;
	char *units;
	uint64_t granularity;
	uint64_t quantity;
	uint64_t free;
} bb_pools_t;

/* Description of each Cray DW pool entry
 */
typedef struct bb_sessions {
	uint32_t id;
	char    *token;
	bool     used;
	uint32_t user_id;
} bb_sessions_t;

typedef struct {
	char   **args;
	uint32_t job_id;
	uint32_t user_id;
} pre_run_args_t;

typedef struct {
	char   **args1;
	char   **args2;
	uint32_t job_id;
	uint32_t timeout;
	uint32_t user_id;
} stage_args_t;

typedef struct {		/* Used for scheduling */
	char *   name;		/* BB GRES name, e.g. "nodes" */
	uint64_t add_cnt;	/* Additional GRES required */
	uint64_t avail_cnt;	/* Additional GRES available */
} needed_gres_t;

typedef struct create_buf_data {
	char *access;		/* Access mode */
	bool hurry;		/* Set to destroy in a hurry (no stage-out) */
	uint32_t job_id;	/* Job ID to use */
	char *job_script;	/* Path to job script */
	char *name;		/* Name of the persistent burst buffer */
	uint64_t size;		/* Size in bytes */
	char *type;		/* Access type */
	uint32_t user_id;
} create_buf_data_t;

static int	_alloc_job_bb(struct job_record *job_ptr, bb_job_t *bb_job,
			      bool job_ready);
static void	_apply_limits(void);
static void *	_bb_agent(void *args);
static void	_bb_free_configs(bb_configs_t *ents, int num_ent);
static void	_bb_free_instances(bb_instances_t *ents, int num_ent);
static void	_bb_free_pools(bb_pools_t *ents, int num_ent);
static void	_bb_free_sessions(bb_sessions_t *ents, int num_ent);
static bb_configs_t *
		_bb_get_configs(int *num_ent, bb_state_t *state_ptr);
static bb_instances_t *
		_bb_get_instances(int *num_ent, bb_state_t *state_ptr);
static bb_pools_t *
		_bb_get_pools(int *num_ent, bb_state_t *state_ptr);
static bb_sessions_t *
		_bb_get_sessions(int *num_ent, bb_state_t *state_ptr);
static int	_build_bb_script(struct job_record *job_ptr, char *script_file);
static int	_create_bufs(struct job_record *job_ptr, bb_job_t *bb_job,
			     bool job_ready);
static void *	_create_persistent(void *x);
static void *	_destroy_persistent(void *x);
static void	_free_create_args(create_buf_data_t *create_args);
static void	_free_script_argv(char **script_argv);
static bb_job_t *
		_get_bb_job(struct job_record *job_ptr);
static void	_job_queue_del(void *x);
static bb_configs_t *
		_json_parse_configs_array(json_object *jobj, char *key,
					    int *num);
static bb_instances_t *
		_json_parse_instances_array(json_object *jobj, char *key,
					    int *num);
static struct bb_pools *
		_json_parse_pools_array(json_object *jobj, char *key, int *num);
static struct bb_sessions *
		_json_parse_sessions_array(json_object *jobj, char *key,
					   int *num);
static void	_json_parse_configs_object(json_object *jobj,
					   bb_configs_t *ent);
static void	_json_parse_instances_object(json_object *jobj,
					     bb_instances_t *ent);
static void	_json_parse_pools_object(json_object *jobj, bb_pools_t *ent);
static void	_json_parse_sessions_object(json_object *jobj,
					    bb_sessions_t *ent);
static void	_log_script_argv(char **script_argv, char *resp_msg);
static void	_load_state(bool init_config);
static int	_open_part_state_file(char **state_file);
static int	_parse_bb_opts(struct job_descriptor *job_desc,
			       uint64_t *bb_size, uid_t submit_uid);
static void	_parse_config_links(json_object *instance, bb_configs_t *ent);
static void	_parse_instance_capacity(json_object *instance,
					 bb_instances_t *ent);
static int	_parse_interactive(struct job_descriptor *job_desc,
				   uint64_t *bb_size);
static void	_pick_alloc_account(bb_alloc_t *bb_alloc);
static void	_purge_bb_files(uint32_t job_id);
static void	_purge_vestigial_bufs(void);
static void	_python2json(char *buf);
static void	_recover_limit_state(void);
static int	_queue_stage_in(struct job_record *job_ptr, bb_job_t *bb_job);
static int	_queue_stage_out(struct job_record *job_ptr);
static void	_queue_teardown(uint32_t job_id, uint32_t user_id, bool hurry);
static void	_reset_buf_state(uint32_t user_id, uint32_t job_id, char *name,
				 int new_state);
static void	_save_limits_state(void);
static void	_set_assoc_ptr(bb_alloc_t *bb_alloc);
static void *	_start_pre_run(void *x);
static void *	_start_stage_in(void *x);
static void *	_start_stage_out(void *x);
static void *	_start_teardown(void *x);
static void	_test_config(void);
static int	_test_size_limit(struct job_record *job_ptr, bb_job_t *bb_job);
static void	_timeout_bb_rec(void);
static int	_write_file(char *file_name, char *buf);
static int	_write_nid_file(char *file_name, char *node_list,
				uint32_t job_id);

/* Convert a Python string to real JSON format. Specifically replace single
 * quotes with double quotes and strip leading "u" before the single quotes.
 * See: https://github.com/stedolan/jq/issues/312 */
static void _python2json(char *buf)
{
	bool quoted = false;
	int i, o;

	if (!buf)
		return;
	for (i = 0, o = 0; ; i++) {
		if (buf[i] == '\'') {
			buf[o++] = '\"';
			quoted = !quoted;
		} else if ((buf[i] == 'u') && (buf[i+1] == '\'') && !quoted) {
			/* Skip over unicode flag */
		} else {
			buf[o++] = buf[i];
			if (buf[i] == '\0')
				break;
		}
	}
}

/* Free an array of xmalloced records. The array must be NULL terminated. */
static void _free_script_argv(char **script_argv)
{
	int i;

	for (i = 0; script_argv[i]; i++)
		xfree(script_argv[i]);
	xfree(script_argv);
}

/* Log a command's arguments. */
static void _log_script_argv(char **script_argv, char *resp_msg)
{
	char *cmd_line = NULL;
	int i;

	if (!bb_state.bb_config.debug_flag)
		return;

	for (i = 0; script_argv[i]; i++) {
		if (i)
			xstrcat(cmd_line, " ");
		xstrcat(cmd_line, script_argv[i]);
	}
	info("%s", cmd_line);
	info("%s", resp_msg);
	xfree(cmd_line);
}

static void _job_queue_del(void *x)
{
	job_queue_rec_t *job_rec = (job_queue_rec_t *) x;
	if (job_rec) {
		xfree(job_rec);
	}
}

/* Purge files we have created for the job.
 * bb_state.bb_mutex is locked on function entry. */
static void _purge_bb_files(uint32_t job_id)

{
	char *hash_dir = NULL, *job_dir = NULL;
	char *script_file = NULL, *path_file = NULL, *client_nids_file = NULL;
	int hash_inx;

	hash_inx = job_id % 10;
	xstrfmtcat(hash_dir, "%s/hash.%d", state_save_loc, hash_inx);
	(void) mkdir(hash_dir, 0700);
	xstrfmtcat(job_dir, "%s/job.%u", hash_dir, job_id);
	(void) mkdir(job_dir, 0700);

	xstrfmtcat(client_nids_file, "%s/client_nids", job_dir);
	xstrfmtcat(path_file, "%s/pathfile", job_dir);
	xstrfmtcat(script_file, "%s/script", job_dir);

	(void) unlink(client_nids_file);
	(void) unlink(path_file);
	(void) unlink(script_file);
	(void) unlink(job_dir);

	xfree(client_nids_file);
	xfree(path_file);
	xfree(script_file);
	xfree(job_dir);
	xfree(hash_dir);
}

/* Validate that our configuration is valid for this plugin type */
static void _test_config(void)
{
	if (!bb_state.bb_config.get_sys_state) {
		debug("%s: GetSysState is NULL", __func__);
		bb_state.bb_config.get_sys_state =
			xstrdup("/opt/cray/dw_wlm/default/bin/dw_wlm_cli");
	}
}

/* Allocate resources to a job and begin stage-in */
static int _alloc_job_bb(struct job_record *job_ptr, bb_job_t *bb_job,
			 bool job_ready)
{
	char jobid_buf[32];
	int rc = SLURM_SUCCESS;

	if (bb_state.bb_config.debug_flag) {
		info("%s: start job allocate %s", __func__,
		     jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
	}

	if (bb_job->buf_cnt &&
	    (_create_bufs(job_ptr, bb_job, job_ready) > 0))
		return EAGAIN;

	if (bb_job->total_size || bb_job->swap_size) {
		if (bb_job->state < BB_STATE_STAGING_IN) {
			bb_job->state = BB_STATE_STAGING_IN;
			rc = _queue_stage_in(job_ptr, bb_job);
			if (rc != SLURM_SUCCESS) {
				bb_job->state = BB_STATE_TEARDOWN;
				_queue_teardown(job_ptr->job_id,
						job_ptr->user_id, true);
			}
		}
	} else {
		bb_job->state = BB_STATE_STAGED_IN;
	}

	return rc;
}

/* Perform periodic background activities */
static void *_bb_agent(void *args)
{
	/* Locks: write job */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK };

	while (!bb_state.term_flag) {
		bb_sleep(&bb_state, AGENT_INTERVAL);
		if (bb_state.term_flag)
			break;
		_load_state(false);	/* Has own locking */
		lock_slurmctld(job_write_lock);
		pthread_mutex_lock(&bb_state.bb_mutex);
		_timeout_bb_rec();
		pthread_mutex_unlock(&bb_state.bb_mutex);
		unlock_slurmctld(job_write_lock);

		_save_limits_state();	/* Has own locks excluding file write */
	}
	return NULL;
}

/* Return the burst buffer size specification of a job
 * RET size data structure or NULL of none found
 * NOTE: delete return value using _del_bb_size() */
static bb_job_t *_get_bb_job(struct job_record *job_ptr)
{
	char *bb_specs, *bb_hurry, *bb_name, *bb_size, *bb_type, *bb_access;
	char *end_ptr = NULL, *save_ptr = NULL, *sep, *tok, *tmp;
	bool have_bb = false;
	uint64_t tmp_cnt;
	int inx;
	bb_job_t *bb_job;
	char *job_type = NULL, *job_access = NULL;

	if ((job_ptr->burst_buffer == NULL) ||
	    (job_ptr->burst_buffer[0] == '\0'))
		return NULL;

	if ((bb_job = bb_job_find(&bb_state, job_ptr->job_id)))
		return bb_job;	/* Cached data */

	bb_job = bb_job_alloc(&bb_state, job_ptr->job_id);
	bb_job->account = xstrdup(job_ptr->account);
	if (job_ptr->part_ptr)
		bb_job->partition = xstrdup(job_ptr->part_ptr->name);
	if (job_ptr->qos_ptr) {
		slurmdb_qos_rec_t *qos_ptr =
			(slurmdb_qos_rec_t *)job_ptr->qos_ptr;
		bb_job->qos = xstrdup(qos_ptr->name);
	}
	bb_job->state = BB_STATE_PENDING;
	bb_specs = xstrdup(job_ptr->burst_buffer);
	tok = strtok_r(bb_specs, " ", &save_ptr);
	while (tok) {
		tmp_cnt = 0;
		if (!strncmp(tok, "SLURM_JOB=", 10)) {
			/* Format: "SLURM_JOB=SIZE=%"PRIu64",ACCESS=%s,TYPE=%s" */
			have_bb = true;
			tok += 10;
			/* Work from the back and replace keys with '\0' */
			job_type = strstr(tok, ",TYPE=");
			if (job_type) {
				job_type[0] = '\0';
				job_type += 6;
			}
			job_access = strstr(tok, ",ACCESS=");
			if (job_access) {
				job_access[0] = '\0';
				job_access += 8;
			}
			bb_size = strstr(tok, "SIZE=");
			if (bb_size) {
				bb_size[0] = '\0';
				tmp_cnt = bb_get_size_num(bb_size + 5,
						bb_state.bb_config.granularity);
				bb_job->total_size += tmp_cnt;
			} else
				tmp_cnt = 0;
		} else if (!strncmp(tok, "SLURM_SWAP=", 11)) {
			/* Format: "SLURM_SWAP=%uGB(%uNodes)" */
			tok += 11;
			bb_job->swap_size += strtol(tok, &end_ptr, 10);
			if (bb_job->swap_size)
				have_bb = true;
			if ((end_ptr[0] == 'G') && (end_ptr[1] == 'B') &&
			    (end_ptr[2] == '(')) {
				bb_job->swap_nodes = strtol(end_ptr + 3,
							    NULL, 10);
			} else {
				bb_job->swap_nodes = 1;
			}
		} else if (!strncmp(tok, "SLURM_GRES=", 11)) {
			/* Format: "SLURM_GRES=nodes:%u" */
			tmp = xstrdup(tok + 11);
			tok = strtok_r(tmp, ",", &end_ptr);
			while (tok) {
				have_bb = true;
				inx = bb_job->gres_cnt++;
				bb_job->gres_ptr = xrealloc(bb_job->gres_ptr,
							    sizeof(bb_gres_t) *
							    bb_job->gres_cnt);
				sep = strchr(tok, ':');
				if (sep) {
					sep[0] = '\0';
					bb_job->gres_ptr[inx].count =
						strtol(sep + 1, NULL, 10);
				} else {
					bb_job->gres_ptr[inx].count = 1;
				}
				bb_job->gres_ptr[inx].name = xstrdup(tok);
				tok = strtok_r(NULL, ",", &end_ptr);
			}
			xfree(tmp);
		} else if (!strncmp(tok, "SLURM_PERSISTENT_CREATE=", 24)) {
			/* Format: SLURM_PERSISTENT_CREATE=NAME=%s,SIZE=%"PRIu64",ACCESS=%s,TYPE=%s" */
			have_bb = true;
			tok += 24;
			/* Work from the back and replace keys with '\0' */
			bb_type = strstr(tok, ",TYPE=");
			if (bb_type) {
				bb_type[0] = '\0';
				bb_type += 6;
			}
			bb_access = strstr(tok, ",ACCESS=");
			if (bb_access) {
				bb_access[0] = '\0';
				bb_access += 8;
			}
			bb_size = strstr(tok, ",SIZE=");
			if (bb_size) {
				bb_size[0] = '\0';
				tmp_cnt = bb_get_size_num(bb_size + 6,
						bb_state.bb_config.granularity);
				bb_job->persist_add += tmp_cnt;
			} else
				tmp_cnt = 0;
			bb_name = strstr(tok, "NAME=");
			if (bb_name)
				bb_name += 5;
			inx = bb_job->buf_cnt++;
			bb_job->buf_ptr = xrealloc(bb_job->buf_ptr,
						   sizeof(bb_buf_t) *
						   bb_job->buf_cnt);
			bb_job->buf_ptr[inx].access = xstrdup(bb_access);
			//bb_job->buf_ptr[inx].destroy = false;
			//bb_job->buf_ptr[inx].hurry = false;
			bb_job->buf_ptr[inx].name = xstrdup(bb_name);
			bb_job->buf_ptr[inx].size = tmp_cnt;
			bb_job->buf_ptr[inx].state = BB_STATE_PENDING;
			bb_job->buf_ptr[inx].type = xstrdup(bb_type);
		} else if (!strncmp(tok, "SLURM_PERSISTENT_DESTROY=", 25)) {
			/* Format: SLURM_PERSISTENT_DESTROY=NAME=%s[,HURRY]" */
			have_bb = true;
			tok += 25;
			/* Work from the back and replace keys with '\0' */
			bb_hurry = strstr(tok, ",HURRY");
			if (bb_hurry)
				bb_hurry[0] = '\0';
			bb_name = strstr(tok, "NAME=");
			if (bb_name)
				bb_name += 5;
			inx = bb_job->buf_cnt++;
			bb_job->buf_ptr = xrealloc(bb_job->buf_ptr,
						   sizeof(bb_buf_t) *
						   bb_job->buf_cnt);
			//bb_job->buf_ptr[inx].access = NULL;
			bb_job->buf_ptr[inx].destroy = true;
			bb_job->buf_ptr[inx].hurry = (bb_hurry != NULL);
			bb_job->buf_ptr[inx].name = xstrdup(bb_name);
			//bb_job->buf_ptr[inx].size = 0;
			bb_job->buf_ptr[inx].state = BB_STATE_PENDING;
			//bb_job->buf_ptr[inx].type = NULL;
		} else if (!strncmp(tok, "SLURM_PERSISTENT_USE", 20)) {
			/* Format: SLURM_PERSISTENT_USE" */
			have_bb = true;
		}
		tok = strtok_r(NULL, " ", &save_ptr);
	}
	xfree(bb_specs);

	if (!have_bb) {
		bb_job_del(&bb_state, job_ptr->job_id);
		return NULL;
	}
	if (bb_state.bb_config.debug_flag)
		bb_job_log(&bb_state, bb_job);
	return bb_job;
}

/* At slurmctld start up time, for every currently active burst buffer,
 * update that user's limit */
static void _apply_limits(void)
{
	bb_alloc_t *bb_alloc;
	int i;

	for (i = 0; i < BB_HASH_SIZE; i++) {
		bb_alloc = bb_state.bb_ahash[i];
		while (bb_alloc) {
			_set_assoc_ptr(bb_alloc);
			bb_limit_add(bb_alloc->user_id, bb_alloc->account,
				     bb_alloc->partition, bb_alloc->qos,
				     bb_alloc->size, &bb_state);
                        bb_alloc = bb_alloc->next;
		}
	}
}

/* Write current burst buffer state to a file so that we can preserve account,
 * partition, and QOS information of persistent burst buffers as there is no
 * place to store that information within the DataWarp data structures */
static void _save_limits_state(void)
{
	static time_t last_save_time = 0;
	static int high_buffer_size = 16 * 1024;
	time_t save_time;
	bb_alloc_t *bb_alloc;
	uint32_t rec_count = 0;
	Buf buffer;
	char *old_file = NULL, *new_file = NULL, *reg_file = NULL;
	int i, count_offset, offset, state_fd;
	int error_code = 0;
	uint16_t protocol_version = SLURM_15_08_PROTOCOL_VERSION;

	if ((bb_state.persist_create_time < last_save_time) ||
	    (bb_state.bb_ahash == NULL))
		return;

	/* Build buffer with name/account/partition/qos information for all
	 * named burst buffers so we can preserve limits across restarts */
	buffer = init_buf(high_buffer_size);
	pack16(protocol_version, buffer);
	count_offset = get_buf_offset(buffer);
	pack32(rec_count, buffer);
	pthread_mutex_lock(&bb_state.bb_mutex);
	for (i = 0; i < BB_HASH_SIZE; i++) {
		bb_alloc = bb_state.bb_ahash[i];
		while (bb_alloc) {
			if (bb_alloc->name) {
				packstr(bb_alloc->account,	buffer);
				pack_time(bb_alloc->create_time,buffer);
				packstr(bb_alloc->name,		buffer);
				packstr(bb_alloc->partition,	buffer);
				packstr(bb_alloc->qos,		buffer);
				pack32(bb_alloc->user_id,	buffer);
				if (bb_state.bb_config.flags & BB_FLAG_EMULATE_CRAY)
					pack64(bb_alloc->size,	buffer);
				rec_count++;
			}
			bb_alloc = bb_alloc->next;
		}
	}
	save_time = time(NULL);
	pthread_mutex_unlock(&bb_state.bb_mutex);
	offset = get_buf_offset(buffer);
	set_buf_offset(buffer, count_offset);
	pack32(rec_count, buffer);
	set_buf_offset(buffer, offset);

	xstrfmtcat(old_file, "%s/%s", slurmctld_conf.state_save_location,
		   "burst_buffer_cray_state.old");
	xstrfmtcat(reg_file, "%s/%s", slurmctld_conf.state_save_location,
		   "burst_buffer_cray_state");
	xstrfmtcat(new_file, "%s/%s", slurmctld_conf.state_save_location,
		   "burst_buffer_cray_state.new");

	state_fd = creat(new_file, 0600);
	if (state_fd < 0) {
		error("%s: Can't save state, error creating file %s, %m",
		      __func__, new_file);
		error_code = errno;
	} else {
		int pos = 0, nwrite = get_buf_offset(buffer), amount, rc;
		char *data = (char *)get_buf_data(buffer);
		high_buffer_size = MAX(nwrite, high_buffer_size);
		while (nwrite > 0) {
			amount = write(state_fd, &data[pos], nwrite);
			if ((amount < 0) && (errno != EINTR)) {
				error("Error writing file %s, %m", new_file);
				break;
			}
			nwrite -= amount;
			pos    += amount;
		}

		rc = fsync_and_close(state_fd, "burst_buffer_cray");
		if (rc && !error_code)
			error_code = rc;
	}
	if (error_code)
		(void) unlink(new_file);
	else {			/* file shuffle */
		last_save_time = save_time;
		(void) unlink(old_file);
		if (link(reg_file, old_file)) {
			debug4("unable to create link for %s -> %s: %m",
			       reg_file, old_file);
		}
		(void) unlink(reg_file);
		if (link(new_file, reg_file)) {
			debug4("unable to create link for %s -> %s: %m",
			       new_file, reg_file);
		}
		(void) unlink(new_file);
	}
	xfree(old_file);
	xfree(reg_file);
	xfree(new_file);
	free_buf(buffer);
}

/* Open the partition state save file, or backup if necessary.
 * state_file IN - the name of the state save file used
 * RET the file description to read from or error code
 */
static int _open_part_state_file(char **state_file)
{
	int state_fd;
	struct stat stat_buf;

	*state_file = xstrdup(slurmctld_conf.state_save_location);
	xstrcat(*state_file, "/burst_buffer_cray_state");
	state_fd = open(*state_file, O_RDONLY);
	if (state_fd < 0) {
		error("Could not open burst buffer state file %s: %m",
		      *state_file);
	} else if (fstat(state_fd, &stat_buf) < 0) {
		error("Could not stat burst buffer state file %s: %m",
		      *state_file);
		(void) close(state_fd);
	} else if (stat_buf.st_size < 4) {
		error("Burst buffer state file %s too small", *state_file);
		(void) close(state_fd);
	} else 	/* Success */
		return state_fd;

	error("NOTE: Trying backup burst buffer state save file. Information may be lost!");
	xstrcat(*state_file, ".old");
	state_fd = open(*state_file, O_RDONLY);
	return state_fd;
}

/* Recover saved burst buffer state and use it to preserve account, partition,
 * and QOS information for persistent burst buffers. */
static void _recover_limit_state(void)
{
	char *state_file = NULL, *data = NULL;
	int data_allocated, data_read = 0;
	uint16_t protocol_version = (uint16_t)NO_VAL;
	uint32_t data_size = 0, rec_count = 0, name_len = 0, user_id = 0;
	uint64_t size;
	int i, state_fd;
	char *account = NULL, *name = NULL, *partition = NULL, *qos = NULL;
	char *end_ptr = NULL;
	time_t create_time = 0;
	bb_alloc_t *bb_alloc;
	Buf buffer;

	state_fd = _open_part_state_file(&state_file);
	if (state_fd < 0) {
		info("No burst buffer state file (%s) to recover",
		     state_file);
		xfree(state_file);
		return;
	}
	data_allocated = BUF_SIZE;
	data = xmalloc(data_allocated);
	while (1) {
		data_read = read(state_fd, &data[data_size], BUF_SIZE);
		if (data_read < 0) {
			if  (errno == EINTR)
				continue;
			else {
				error("Read error on %s: %m", state_file);
				break;
			}
		} else if (data_read == 0)     /* eof */
			break;
		data_size      += data_read;
		data_allocated += data_read;
		xrealloc(data, data_allocated);
	}
	close(state_fd);
	xfree(state_file);

	buffer = create_buf(data, data_size);
	safe_unpack16(&protocol_version, buffer);
	if (protocol_version == (uint16_t)NO_VAL) {
		error("******************************************************************");
		error("Can not recover burst_buffer/cray state, data version incompatible");
		error("******************************************************************");
		return;
	}

	safe_unpack32(&rec_count, buffer);
	for (i = 0; i < rec_count; i++) {
		safe_unpackstr_xmalloc(&account,   &name_len, buffer);
		safe_unpack_time(&create_time, buffer);
		safe_unpackstr_xmalloc(&name,      &name_len, buffer);
		safe_unpackstr_xmalloc(&partition, &name_len, buffer);
		safe_unpackstr_xmalloc(&qos,       &name_len, buffer);
		safe_unpack32(&user_id, buffer);
		if (bb_state.bb_config.flags & BB_FLAG_EMULATE_CRAY)
			safe_unpack64(&size, buffer);

		if (bb_state.bb_config.flags & BB_FLAG_EMULATE_CRAY) {
			bb_alloc = bb_alloc_name_rec(&bb_state, name, user_id);
			if (name && (name[0] >='0') && (name[0] <='9'))
				bb_alloc->job_id = strtol(name, &end_ptr, 10);
			bb_alloc->seen_time = time(NULL);
			bb_alloc->size = size;
		} else {
			bb_alloc = bb_find_name_rec(name, user_id, &bb_state);
		}
		if (bb_alloc) {
			xfree(bb_alloc->account);
			bb_alloc->account = account;
			account = NULL;
			bb_alloc->create_time = create_time;
			xfree(bb_alloc->partition);
			bb_alloc->partition = partition;
			partition = NULL;
			xfree(bb_alloc->qos);
			bb_alloc->qos = qos;
			qos = NULL;		
		}
		xfree(account);
		xfree(name);
		xfree(partition);
		xfree(qos);
	}

	info("Recovered state of %d burst buffers", rec_count);
	free_buf(buffer);
	return;

unpack_error:
	error("Incomplete burst buffer data checkpoint file");
	xfree(account);
	xfree(name);
	xfree(partition);
	xfree(qos);
	free_buf(buffer);
	return;
}

/* We just found an unexpected session, set default account, QOS, & partition.
 * Copy the information from any currently existing session for the same user.
 * If none found, use his default account and QOS. */
static void _pick_alloc_account(bb_alloc_t *bb_alloc)
{
	/* read locks on assoc & qos */
	assoc_mgr_lock_t assoc_locks = { READ_LOCK, NO_LOCK, READ_LOCK, NO_LOCK,
					 NO_LOCK, NO_LOCK, NO_LOCK };
	slurmdb_assoc_rec_t assoc_rec, *assoc_ptr = NULL;
	slurmdb_qos_rec_t   qos_rec,   *qos_ptr   = NULL;
	bb_alloc_t *bb_ptr = NULL;

	bb_ptr = bb_state.bb_ahash[bb_alloc->user_id % BB_HASH_SIZE];
	while (bb_ptr) {
		if ((bb_ptr          != bb_alloc) &&
		    (bb_ptr->user_id == bb_alloc->user_id)) {
			bb_alloc->account   = xstrdup(bb_ptr->account);
			bb_alloc->assoc_ptr = bb_ptr->assoc_ptr;
			bb_alloc->partition = xstrdup(bb_ptr->partition);
			bb_alloc->qos       = xstrdup(bb_ptr->qos);
			return;
		}
		bb_ptr = bb_ptr->next;
	}

	/* Set default for this user */
	bb_alloc->partition = xstrdup(default_part_name);
	memset(&assoc_rec, 0, sizeof(slurmdb_assoc_rec_t));
	memset(&qos_rec, 0, sizeof(slurmdb_qos_rec_t));
	assoc_rec.partition = default_part_name;
	assoc_rec.uid = bb_alloc->user_id;
	assoc_mgr_lock(&assoc_locks);
	if (assoc_mgr_fill_in_assoc(acct_db_conn, &assoc_rec,
				    accounting_enforce,
				    (slurmdb_assoc_rec_t **) &assoc_ptr,
				    true) == SLURM_SUCCESS) {
		bb_alloc->assoc_ptr = assoc_ptr;
		bb_alloc->account   = xstrdup(assoc_rec.acct);

		if (assoc_ptr->usage->valid_qos) {
			if (assoc_ptr->def_qos_id)
				qos_rec.id = assoc_ptr->def_qos_id;
			else if (bit_set_count(assoc_ptr->usage->valid_qos)==1)
				qos_rec.id =
					bit_ffs(assoc_ptr->usage->valid_qos);
			else if (assoc_mgr_root_assoc
				 && assoc_mgr_root_assoc->def_qos_id)
				qos_rec.id = assoc_mgr_root_assoc->def_qos_id;
			else
				qos_rec.name = "normal";
		}
		if (assoc_mgr_fill_in_qos(acct_db_conn, &qos_rec,
					  accounting_enforce, &qos_ptr,
					  true) == SLURM_SUCCESS) {
			bb_alloc->qos = xstrdup(qos_ptr->name);
		}
	}
	assoc_mgr_unlock(&assoc_locks);
}

/* For a given user/partition/account, set it's assoc_ptr */
static void _set_assoc_ptr(bb_alloc_t *bb_alloc)
{
	/* read locks on assoc */
	assoc_mgr_lock_t assoc_locks = { READ_LOCK, NO_LOCK, NO_LOCK, NO_LOCK,
					 NO_LOCK, NO_LOCK, NO_LOCK };
	slurmdb_assoc_rec_t assoc_rec, *assoc_ptr = NULL;

	memset(&assoc_rec, 0, sizeof(slurmdb_assoc_rec_t));
	assoc_rec.acct      = bb_alloc->account;
	assoc_rec.partition = bb_alloc->partition;
	assoc_rec.uid       = bb_alloc->user_id;
	assoc_mgr_lock(&assoc_locks);
	if (assoc_mgr_fill_in_assoc(acct_db_conn, &assoc_rec,
				    accounting_enforce,
				    (slurmdb_assoc_rec_t **) &assoc_ptr,
				    true) == SLURM_SUCCESS) {
		bb_alloc->assoc_ptr = assoc_ptr;
	}
	assoc_mgr_unlock(&assoc_locks);
}

/*
 * Determine the current actual burst buffer state.
 */
static void _load_state(bool init_config)
{
	burst_buffer_gres_t *gres_ptr;
	bb_configs_t *configs;
	bb_instances_t *instances;
	bb_pools_t *pools;
	bb_sessions_t *sessions;
	bb_alloc_t *bb_alloc;
	int num_configs = 0, num_instances = 0, num_pools = 0, num_sessions = 0;
	int i, j;
	char *end_ptr = NULL;

	/*
	 * Load the pools information
	 */
	pools = _bb_get_pools(&num_pools, &bb_state);
	if (pools == NULL) {
		error("%s: failed to find DataWarp entries, what now?",
		      __func__);
		return;
	}

	pthread_mutex_lock(&bb_state.bb_mutex);
	if (!bb_state.bb_config.default_pool && (num_pools > 0)) {
		info("%s: Setting DefaultPool to %s", __func__, pools[0].id);
		bb_state.bb_config.default_pool = xstrdup(pools[0].id);
	}

	for (i = 0; i < num_pools; i++) {
		/* ID: "bytes" */
		if (strcmp(pools[i].id, bb_state.bb_config.default_pool) == 0) {
			bb_state.bb_config.granularity
				= pools[i].granularity;
			bb_state.total_space
				= pools[i].quantity * pools[i].granularity;
			if (bb_state.bb_config.flags & BB_FLAG_EMULATE_CRAY)
				continue;
			bb_state.used_space
				= (pools[i].quantity - pools[i].free) *
				  pools[i].granularity;

			/* Everything else is a generic burst buffer resource */
			bb_state.bb_config.gres_cnt = 0;
		} else {
			bb_state.bb_config.gres_ptr
				= xrealloc(bb_state.bb_config.gres_ptr,
					   sizeof(burst_buffer_gres_t) *
					   (bb_state.bb_config.gres_cnt + 1));
			gres_ptr = bb_state.bb_config.gres_ptr +
				   bb_state.bb_config.gres_cnt;
			bb_state.bb_config.gres_cnt++;
			gres_ptr->avail_cnt = pools[i].quantity;
			gres_ptr->granularity = pools[i].granularity;
			gres_ptr->name = xstrdup(pools[i].id);
			if (bb_state.bb_config.flags & BB_FLAG_EMULATE_CRAY)
				continue;
			gres_ptr->used_cnt = pools[i].quantity - pools[i].free;
		}
	}
	pthread_mutex_unlock(&bb_state.bb_mutex);
	_bb_free_pools(pools, num_pools);

	/*
	 * Load the instances information
	 */
	instances = _bb_get_instances(&num_instances, &bb_state);
	if (instances == NULL) {
		info("%s: failed to find DataWarp instances", __func__);
	}
	sessions = _bb_get_sessions(&num_sessions, &bb_state);
	pthread_mutex_lock(&bb_state.bb_mutex);
	bb_state.last_load_time = time(NULL);
	for (i = 0; i < num_sessions; i++) {
		if (!init_config) {
			bb_alloc = bb_find_name_rec(sessions[i].token,
						    sessions[i].user_id,
						    &bb_state);
			if (bb_alloc) {
				bb_alloc->seen_time = bb_state.last_load_time;
				continue;
			}
			error("%s: Unexpected burst buffer %s found",
			      __func__, sessions[i].token);
		}

		bb_alloc = bb_alloc_name_rec(&bb_state, sessions[i].token,
					     sessions[i].user_id);
//FIXME: Set create_time
		if ((sessions[i].token != NULL)  &&
		    (sessions[i].token[0] >='0') &&
		    (sessions[i].token[0] <='9')) {
			bb_alloc->job_id =
				strtol(sessions[i].token, &end_ptr, 10);
		}
//FIXME: Below logic seems wrong
		for (j = 0; j < num_instances; j++)
			bb_alloc->size = instances[j].bytes;
		bb_alloc->seen_time = bb_state.last_load_time;

		if (!init_config) {	/* Newly found buffer */
			_pick_alloc_account(bb_alloc);
			bb_limit_add(bb_alloc->user_id, bb_alloc->account,
				     bb_alloc->partition, bb_alloc->qos,
				     bb_alloc->size, &bb_state);
		}
	}
	pthread_mutex_unlock(&bb_state.bb_mutex);
	_bb_free_sessions(sessions, num_sessions);
	_bb_free_instances(instances, num_instances);

	if (!init_config)
		return;

	/*
	 * Load the configurations information
	 */
	configs = _bb_get_configs(&num_configs, &bb_state);
	if (configs == NULL) {
		info("%s: failed to find DataWarp configurations", __func__);
	}
	_bb_free_configs(configs, num_sessions);
//FIXME: configurations data is currently unused, is it needed?

	_recover_limit_state();
	_apply_limits();

	return;
}

/* Write an string representing the NIDs of a job's nodes to an arbitrary
 * file location
 * RET 0 or Slurm error code
 */
static int _write_nid_file(char *file_name, char *node_list, uint32_t job_id)
{
#if defined(HAVE_NATIVE_CRAY)
	char *tmp, *sep, *tok, *save_ptr = NULL, *buf = NULL;
	int i, rc;

	xassert(file_name);
	tmp = xstrdup(node_list);
	sep = strrchr(tmp, ']');
	if (sep)
		sep[0] = '\0';
	sep = strchr(tmp, '[');
	if (sep) {
		sep++;
	} else {
		sep = tmp;
		for (i = 0; !isdigit(sep[0]) && sep[0]; i++)
			sep++;
	}
	tok = strtok_r(sep, ",", &save_ptr);
	while (tok) {
		xstrfmtcat(buf, "%s\n", tok);
		tok = strtok_r(NULL, ",", &save_ptr);
	}
	xfree(tmp);

	if (buf) {
		rc = _write_file(file_name, buf);
		xfree(buf);
	} else {
		error("%s: job %u has node list without numeric component (%s)",
		      __func__, job_id, node_list);
		rc = EINVAL;
	}
	return rc;
#else
	char *tok, *buf = NULL;
	int rc;

	xassert(file_name);
	if (node_list && node_list[0]) {
		hostlist_t hl = hostlist_create(node_list);
		while ((tok = hostlist_shift(hl))) {
			xstrfmtcat(buf, "%s\n", tok);
			free(tok);
		}
		hostlist_destroy(hl);
		rc = _write_file(file_name, buf);
		xfree(buf);
	} else {
		error("%s: job %u lacks a node list",  __func__, job_id);
		rc = EINVAL;
	}
	return rc;
#endif
}

/* Write an arbitrary string to an arbitrary file name */
static int _write_file(char *file_name, char *buf)
{
	int amount, fd, nwrite, pos;

	(void) unlink(file_name);
	fd = creat(file_name, 0600);
	if (fd < 0) {
		error("Error creating file %s, %m", file_name);
		return errno;
	}

	if (!buf) {
		error("%s: buf is NULL", __func__);
		return SLURM_ERROR;
	}

	nwrite = strlen(buf);
	pos = 0;
	while (nwrite > 0) {
		amount = write(fd, &buf[pos], nwrite);
		if ((amount < 0) && (errno != EINTR)) {
			error("Error writing file %s, %m", file_name);
			close(fd);
			return ESLURM_WRITING_TO_FILE;
		}
		nwrite -= amount;
		pos    += amount;
	}

	(void) close(fd);
	return SLURM_SUCCESS;
}

static int _queue_stage_in(struct job_record *job_ptr, bb_job_t *bb_job)
{
	char *hash_dir = NULL, *job_dir = NULL;
	char *client_nodes_file_nid = NULL;
	char **setup_argv, **data_in_argv;
	stage_args_t *stage_args;
	int hash_inx = job_ptr->job_id % 10;
	pthread_attr_t stage_attr;
	pthread_t stage_tid = 0;
	int rc = SLURM_SUCCESS;

	xstrfmtcat(hash_dir, "%s/hash.%d", state_save_loc, hash_inx);
	(void) mkdir(hash_dir, 0700);
	xstrfmtcat(job_dir, "%s/job.%u", hash_dir, job_ptr->job_id);
	if (job_ptr->sched_nodes) {
		xstrfmtcat(client_nodes_file_nid, "%s/client_nids", job_dir);
		if (_write_nid_file(client_nodes_file_nid,
				    job_ptr->sched_nodes, job_ptr->job_id))
			xfree(client_nodes_file_nid);
	}
	setup_argv = xmalloc(sizeof(char *) * 20);	/* NULL terminated */
	setup_argv[0] = xstrdup("dw_wlm_cli");
	setup_argv[1] = xstrdup("--function");
	setup_argv[2] = xstrdup("setup");
	setup_argv[3] = xstrdup("--token");
	xstrfmtcat(setup_argv[4], "%u", job_ptr->job_id);
	setup_argv[5] = xstrdup("--caller");
	setup_argv[6] = xstrdup("SLURM");
	setup_argv[7] = xstrdup("--user");
	xstrfmtcat(setup_argv[8], "%d", job_ptr->user_id);
	setup_argv[9] = xstrdup("--capacity");
	xstrfmtcat(setup_argv[10], "%s:%s",
		   bb_state.bb_config.default_pool,
		   bb_get_size_str(bb_job->total_size));
	setup_argv[11] = xstrdup("--job");
	xstrfmtcat(setup_argv[12], "%s/script", job_dir);
	if (client_nodes_file_nid) {
#if defined(HAVE_NATIVE_CRAY)
		setup_argv[13] = xstrdup("--nidlistfile");
#else
		setup_argv[13] = xstrdup("--nodehostnamefile");
#endif
		setup_argv[14] = xstrdup(client_nodes_file_nid);
	}

	data_in_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	data_in_argv[0] = xstrdup("dw_wlm_cli");
	data_in_argv[1] = xstrdup("--function");
	data_in_argv[2] = xstrdup("data_in");
	data_in_argv[3] = xstrdup("--token");
	xstrfmtcat(data_in_argv[4], "%u", job_ptr->job_id);
	data_in_argv[5] = xstrdup("--job");
	xstrfmtcat(data_in_argv[6], "%s/script", job_dir);

	stage_args = xmalloc(sizeof(stage_args_t));
	stage_args->job_id  = job_ptr->job_id;
	stage_args->timeout = bb_state.bb_config.stage_in_timeout;
	stage_args->args1   = setup_argv;
	stage_args->args2   = data_in_argv;

	slurm_attr_init(&stage_attr);
	if (pthread_attr_setdetachstate(&stage_attr, PTHREAD_CREATE_DETACHED))
		error("pthread_attr_setdetachstate error %m");
	while (pthread_create(&stage_tid, &stage_attr, _start_stage_in,
			      stage_args)) {
		if (errno != EAGAIN) {
			error("%s: pthread_create: %m", __func__);
			_start_stage_in(stage_args);	/* Do in-line */
			break;
		}
		usleep(100000);
	}
	slurm_attr_destroy(&stage_attr);

	xfree(hash_dir);
	xfree(job_dir);
	xfree(client_nodes_file_nid);
	return rc;
}

static void *_start_stage_in(void *x)
{
	stage_args_t *stage_args;
	char **setup_argv, **data_in_argv, *resp_msg = NULL, *op = NULL;
	int rc = SLURM_SUCCESS, status = 0, timeout;
	slurmctld_lock_t job_read_lock =
		    { NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK };
	slurmctld_lock_t job_write_lock =
		    { NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK };
	struct job_record *job_ptr;
	bb_alloc_t *bb_alloc = NULL;
	bb_job_t *bb_job;
	DEF_TIMERS;

	stage_args = (stage_args_t *) x;
	setup_argv   = stage_args->args1;
	data_in_argv = stage_args->args2;

	if (stage_args->timeout)
		timeout = stage_args->timeout * 1000;
	else
		timeout = 5000;
	op = "setup";
	START_TIMER;
	resp_msg = bb_run_script("setup",
				 bb_state.bb_config.get_sys_state,
				 setup_argv, timeout, &status);
	END_TIMER;
	if (DELTA_TIMER > 500000) {	/* 0.5 secs */
		info("%s: setup for job %u ran for %s",
		     __func__, stage_args->job_id, TIME_STR);
	} else if (bb_state.bb_config.debug_flag) {
		debug("%s: setup for job %u ran for %s",
		     __func__, stage_args->job_id, TIME_STR);
	}
	_log_script_argv(setup_argv, resp_msg);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: setup for job %u status:%u response:%s",
		      __func__, stage_args->job_id, status, resp_msg);
		rc = SLURM_ERROR;
	} else {
		lock_slurmctld(job_read_lock);
		pthread_mutex_lock(&bb_state.bb_mutex);
		job_ptr = find_job_record(stage_args->job_id);
		bb_job = bb_job_find(&bb_state, stage_args->job_id);
		if (!job_ptr) {
			error("%s: unable to find job record for job %u",
			      __func__, stage_args->job_id);
			rc = SLURM_ERROR;
		} else if (!bb_job) {
			error("%s: unable to find bb_job record for job %u",
			      __func__, stage_args->job_id);
		} else {
			bb_job->state = BB_STATE_STAGING_IN;
			bb_alloc = bb_alloc_job(&bb_state, job_ptr, bb_job);
			if (bb_state.bb_config.flags & BB_FLAG_EMULATE_CRAY)
				bb_alloc->create_time = time(NULL);
		}
		pthread_mutex_unlock(&bb_state.bb_mutex);
		unlock_slurmctld(job_read_lock);
	}

	if (rc == SLURM_SUCCESS) {
		if (stage_args->timeout)
			timeout = stage_args->timeout * 1000;
		else
			timeout = 24 * 60 * 60 * 1000;	/* One day */
		xfree(resp_msg);
		op = "dws_data_in";
		START_TIMER;
		resp_msg = bb_run_script("dws_data_in",
					 bb_state.bb_config.get_sys_state,
					 data_in_argv, timeout, &status);
		END_TIMER;
		if (DELTA_TIMER > 5000000) {	/* 5 secs */
			info("%s: dws_data_in for job %u ran for %s",
			     __func__, stage_args->job_id, TIME_STR);
		} else if (bb_state.bb_config.debug_flag) {
			debug("%s: dws_data_in for job %u ran for %s",
			     __func__, stage_args->job_id, TIME_STR);
		}
		_log_script_argv(data_in_argv, resp_msg);
		if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
			error("%s: dws_data_in for job %u status:%u response:%s",
			      __func__, stage_args->job_id, status, resp_msg);
			rc = SLURM_ERROR;
		}
	}

	lock_slurmctld(job_write_lock);
	job_ptr = find_job_record(stage_args->job_id);
	if (!job_ptr) {
		error("%s: unable to find job record for job %u",
		      __func__, stage_args->job_id);
	} else if (rc == SLURM_SUCCESS) {
		pthread_mutex_lock(&bb_state.bb_mutex);
		bb_job = bb_job_find(&bb_state, stage_args->job_id);
		if (bb_job)
			bb_job->state = BB_STATE_STAGED_IN;
		if (bb_job && bb_job->total_size) {
			bb_alloc = bb_find_alloc_rec(&bb_state, job_ptr);
			if (bb_alloc) {
				bb_alloc->state = BB_STATE_STAGED_IN;
				bb_alloc->state_time = time(NULL);
				if (bb_state.bb_config.debug_flag) {
					info("%s: Stage-in complete for job %u",
					     __func__, stage_args->job_id);
				}
				queue_job_scheduler();
			} else {
				error("%s: unable to find bb_alloc record for job %u",
				      __func__, stage_args->job_id);
			}
		}
		pthread_mutex_unlock(&bb_state.bb_mutex);
	} else {
		xfree(job_ptr->state_desc);
		job_ptr->state_reason = FAIL_BURST_BUFFER_OP;
		xstrfmtcat(job_ptr->state_desc, "%s: %s: %s",
			   plugin_type, op, resp_msg);
		job_ptr->priority = 0;	/* Hold job */
		bb_alloc = bb_find_alloc_rec(&bb_state, job_ptr);
		if (bb_alloc) {
			bb_alloc->state = BB_STATE_TEARDOWN;
			bb_alloc->state_time = time(NULL);
		}
		_queue_teardown(job_ptr->job_id, job_ptr->user_id, true);
	}
	unlock_slurmctld(job_write_lock);

	xfree(resp_msg);
	_free_script_argv(setup_argv);
	_free_script_argv(data_in_argv);
	xfree(stage_args);
	return NULL;
}

static int _queue_stage_out(struct job_record *job_ptr)
{
	char *hash_dir = NULL, *job_dir = NULL;
	char **post_run_argv, **data_out_argv;
	stage_args_t *stage_args;
	int hash_inx = job_ptr->job_id % 10, rc = SLURM_SUCCESS;
	pthread_attr_t stage_attr;
	pthread_t stage_tid = 0;

	xstrfmtcat(hash_dir, "%s/hash.%d", state_save_loc, hash_inx);
	xstrfmtcat(job_dir, "%s/job.%u", hash_dir, job_ptr->job_id);

	data_out_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	data_out_argv[0] = xstrdup("dw_wlm_cli");
	data_out_argv[1] = xstrdup("--function");
	data_out_argv[2] = xstrdup("data_out");
	data_out_argv[3] = xstrdup("--token");
	xstrfmtcat(data_out_argv[4], "%u", job_ptr->job_id);
	data_out_argv[5] = xstrdup("--job");
	xstrfmtcat(data_out_argv[6], "%s/script", job_dir);

	post_run_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	post_run_argv[0] = xstrdup("dw_wlm_cli");
	post_run_argv[1] = xstrdup("--function");
	post_run_argv[2] = xstrdup("post_run");
	post_run_argv[3] = xstrdup("--token");
	xstrfmtcat(post_run_argv[4], "%u", job_ptr->job_id);
	post_run_argv[5] = xstrdup("--job");
	xstrfmtcat(post_run_argv[6], "%s/script", job_dir);

	stage_args = xmalloc(sizeof(stage_args_t));
	stage_args->args1   = data_out_argv;
	stage_args->args2   = post_run_argv;
	stage_args->job_id  = job_ptr->job_id;
	stage_args->timeout = bb_state.bb_config.stage_out_timeout;
	stage_args->user_id = job_ptr->user_id;

	slurm_attr_init(&stage_attr);
	if (pthread_attr_setdetachstate(&stage_attr, PTHREAD_CREATE_DETACHED))
		error("pthread_attr_setdetachstate error %m");
	while (pthread_create(&stage_tid, &stage_attr, _start_stage_out,
			      stage_args)) {
		if (errno != EAGAIN) {
			error("%s: pthread_create: %m", __func__);
			_start_stage_out(stage_args);	/* Do in-line */
			break;
		}
		usleep(100000);
	}
	slurm_attr_destroy(&stage_attr);

	xfree(hash_dir);
	xfree(job_dir);
	return rc;
}

static void *_start_stage_out(void *x)
{
	stage_args_t *stage_args;
	char **post_run_argv, **data_out_argv, *resp_msg = NULL, *op = NULL;
	int rc = SLURM_SUCCESS, status = 0, timeout;
	slurmctld_lock_t job_write_lock =
		    { NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK };
	struct job_record *job_ptr;
	bb_alloc_t *bb_alloc = NULL;
	bb_job_t *bb_job = NULL;
	DEF_TIMERS;

	stage_args = (stage_args_t *) x;
	data_out_argv = stage_args->args1;
	post_run_argv = stage_args->args2;

	if (stage_args->timeout)
		timeout = stage_args->timeout * 1000;
	else
		timeout = 24 * 60 * 60 * 1000;	/* One day */
	op = "dws_data_out";
	START_TIMER;
	resp_msg = bb_run_script("dws_data_out",
				 bb_state.bb_config.get_sys_state,
				 data_out_argv, timeout, &status);
	END_TIMER;
	if (DELTA_TIMER > 5000000) {	/* 5 secs */
		info("%s: dws_data_out for job %u ran for %s",
		     __func__, stage_args->job_id, TIME_STR);
	} else if (bb_state.bb_config.debug_flag) {
		debug("%s: dws_data_out for job %u ran for %s",
		     __func__, stage_args->job_id, TIME_STR);
	}
	_log_script_argv(data_out_argv, resp_msg);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: dws_data_out for job %u status:%u response:%s",
		      __func__, stage_args->job_id, status, resp_msg);
		rc = SLURM_ERROR;
	}

	if (rc == SLURM_SUCCESS) {
		if (stage_args->timeout)
			timeout = stage_args->timeout * 1000;
		else
			timeout = 5000;
		op = "dws_post_run";
		START_TIMER;
		resp_msg = bb_run_script("dws_post_run",
					 bb_state.bb_config.get_sys_state,
					 post_run_argv, timeout, &status);
		END_TIMER;
		if (DELTA_TIMER > 500000) {	/* 0.5 secs */
			info("%s: dws_post_run for job %u ran for %s",
			     __func__, stage_args->job_id, TIME_STR);
		} else if (bb_state.bb_config.debug_flag) {
			debug("%s: dws_post_run for job %u ran for %s",
			     __func__, stage_args->job_id, TIME_STR);
		}
		_log_script_argv(post_run_argv, resp_msg);
		if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
			error("%s: dws_post_run for job %u status:%u response:%s",
			      __func__, stage_args->job_id, status, resp_msg);
			rc = SLURM_ERROR;
		}
		xfree(resp_msg);
	}

	lock_slurmctld(job_write_lock);
	job_ptr = find_job_record(stage_args->job_id);
	if (!job_ptr) {
		error("%s: unable to find job record for job %u",
		      __func__, stage_args->job_id);
	} else {
		if (rc != SLURM_SUCCESS) {
			job_ptr->state_reason = FAIL_BURST_BUFFER_OP;
			xfree(job_ptr->state_desc);
			xstrfmtcat(job_ptr->state_desc, "%s: %s: %s",
				   plugin_type, op, resp_msg);
		}
		pthread_mutex_lock(&bb_state.bb_mutex);
		bb_alloc = bb_find_alloc_rec(&bb_state, job_ptr);
		if (bb_alloc) {
			if (rc == SLURM_SUCCESS) {
				if (bb_state.bb_config.debug_flag) {
					info("%s: Stage-out complete for job %u",
					     __func__, stage_args->job_id);
				}
				/* bb_alloc->state = BB_STATE_STAGED_OUT; */
				bb_alloc->state = BB_STATE_TEARDOWN;
				bb_alloc->state_time = time(NULL);
			} else if (bb_state.bb_config.debug_flag) {
				info("%s: Stage-out failed for job %u",
				     __func__, stage_args->job_id);
			}
		} else {
			error("%s: unable to find bb record for job %u",
			      __func__, stage_args->job_id);
		}
		bb_job = _get_bb_job(job_ptr);
		if (bb_job)
			bb_job->state = BB_STATE_TEARDOWN;
		if (rc == SLURM_SUCCESS) {
			_queue_teardown(stage_args->job_id, stage_args->user_id,
					false);
		}
		pthread_mutex_unlock(&bb_state.bb_mutex);
	}
	unlock_slurmctld(job_write_lock);

	xfree(resp_msg);
	_free_script_argv(post_run_argv);
	_free_script_argv(data_out_argv);
	xfree(stage_args);
	return NULL;
}

static void _queue_teardown(uint32_t job_id, uint32_t user_id, bool hurry)
{
	struct stat buf;
	char *hash_dir = NULL, *job_script = NULL;
	char **teardown_argv;
	stage_args_t *teardown_args;
	int fd, hash_inx = job_id % 10;
	pthread_attr_t teardown_attr;
	pthread_t teardown_tid = 0;

	xstrfmtcat(hash_dir, "%s/hash.%d", state_save_loc, hash_inx);
	xstrfmtcat(job_script, "%s/job.%u/script", hash_dir, job_id);
	if (stat(job_script, &buf) == -1) {
		xfree(job_script);
		xstrfmtcat(job_script, "%s/burst_buffer_script", state_save_loc);
		if (stat(job_script, &buf) == -1) {
			fd = creat(job_script, 0755);
			if (fd >= 0) {
				char *dummy_script = "#!/bin/bash\nexit 0\n";
				write(fd, dummy_script, strlen(dummy_script)+1);
				close(fd);
			}
		}
	}

	teardown_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	teardown_argv[0] = xstrdup("dw_wlm_cli");
	teardown_argv[1] = xstrdup("--function");
	teardown_argv[2] = xstrdup("teardown");
	teardown_argv[3] = xstrdup("--token");
	xstrfmtcat(teardown_argv[4], "%u", job_id);
	teardown_argv[5] = xstrdup("--job");
	teardown_argv[6] = xstrdup(job_script);
	if (hurry)
		teardown_argv[7] = xstrdup("--hurry");

	teardown_args = xmalloc(sizeof(stage_args_t));
	teardown_args->job_id  = job_id;
	teardown_args->user_id = user_id;
	teardown_args->timeout = 0;
	teardown_args->args1   = teardown_argv;

	slurm_attr_init(&teardown_attr);
	if (pthread_attr_setdetachstate(&teardown_attr,PTHREAD_CREATE_DETACHED))
		error("pthread_attr_setdetachstate error %m");
	while (pthread_create(&teardown_tid, &teardown_attr, _start_teardown,
			      teardown_args)) {
		if (errno != EAGAIN) {
			error("%s: pthread_create: %m", __func__);
			_start_teardown(teardown_args);	/* Do in-line */
			break;
		}
		usleep(100000);
	}
	slurm_attr_destroy(&teardown_attr);

	xfree(hash_dir);
	xfree(job_script);
}

static void *_start_teardown(void *x)
{
	stage_args_t *teardown_args;
	char **teardown_argv, *resp_msg = NULL;
	int status = 0, timeout;
	struct job_record *job_ptr;
	bb_alloc_t *bb_alloc = NULL;
	bb_job_t *bb_job = NULL;
	/* Locks: write job */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK };
	DEF_TIMERS;

	teardown_args = (stage_args_t *) x;
	teardown_argv = teardown_args->args1;

	START_TIMER;
	if (teardown_args->timeout)
		timeout = teardown_args->timeout * 1000;
	else
		timeout = 5000;
	resp_msg = bb_run_script("teardown",
				 bb_state.bb_config.get_sys_state,
				 teardown_argv, timeout, &status);
	END_TIMER;
	if ((DELTA_TIMER > 500000) ||	/* 0.5 secs */
	    (bb_state.bb_config.debug_flag)) {
		info("%s: teardown for job %u ran for %s",
		     __func__, teardown_args->job_id, TIME_STR);
	}
	_log_script_argv(teardown_argv, resp_msg);
	/* "Teardown" is run at every termination of every job that _might_
	 * have a burst buffer, so an error of "token not found" should be
	 * fairly common and not indicative of a problem. */
	if ((!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) &&
	    (!resp_msg || !strstr(resp_msg, "token not found"))) {
		error("%s: %s: teardown for job %u status:%u response:%s",
		      plugin_name, __func__, teardown_args->job_id, status,
		      resp_msg);
	} else {
		lock_slurmctld(job_write_lock);
		pthread_mutex_lock(&bb_state.bb_mutex);
		_purge_bb_files(teardown_args->job_id);
		if ((job_ptr = find_job_record(teardown_args->job_id))) {
			if ((bb_alloc = bb_find_alloc_rec(&bb_state, job_ptr))){
				bb_limit_rem(bb_alloc->user_id,
					     bb_alloc->account,
					     bb_alloc->partition, bb_alloc->qos,
					     bb_alloc->size, &bb_state);
				bb_free_alloc_rec(&bb_state, bb_alloc);
				(void) bb_free_alloc_rec(&bb_state, bb_alloc);
			}
			if ((bb_job = _get_bb_job(job_ptr)))
				bb_job->state = BB_STATE_COMPLETE;
		} else {
			/* This will happen when slurmctld restarts and needs
			 * to clear vestigial buffers */
			char buf_name[32];
			snprintf(buf_name, sizeof(buf_name), "%u",
				 teardown_args->job_id);
			bb_alloc = bb_find_name_rec(buf_name,
						    teardown_args->user_id,
						    &bb_state);
			if (bb_alloc) {
				bb_limit_rem(bb_alloc->user_id,
					     bb_alloc->account,
					     bb_alloc->partition, bb_alloc->qos,
					     bb_alloc->size, &bb_state);
				bb_free_alloc_rec(&bb_state, bb_alloc);
				(void) bb_free_alloc_rec(&bb_state, bb_alloc);
			}

		}
		pthread_mutex_unlock(&bb_state.bb_mutex);
		unlock_slurmctld(job_write_lock);
	}

	xfree(resp_msg);
	_free_script_argv(teardown_argv);
	xfree(teardown_args);
	return NULL;
}

static void _free_needed_gres_struct(needed_gres_t *needed_gres_ptr,
				     int gres_cnt)
{
	int i;
	if (needed_gres_ptr == NULL)
		return;

	for (i = 0; i < gres_cnt; i++)
		xfree(needed_gres_ptr->name);
	xfree(needed_gres_ptr);
}

static uint64_t _get_bb_resv(char *gres_name, burst_buffer_info_msg_t *resv_bb)
{
	burst_buffer_info_t *bb_array;
	burst_buffer_gres_t *gres_ptr;
	uint64_t resv_gres = 0;
	int i, j;

	if (!resv_bb)
		return resv_gres;

	for (i = 0, bb_array = resv_bb->burst_buffer_array;
	     i < resv_bb->record_count; i++, bb_array++) {
		if (bb_array->name && xstrcmp(bb_array->name, bb_state.name))
			continue;
		for (j = 0, gres_ptr = bb_array->gres_ptr;
		     j < bb_array->gres_cnt; j++, gres_ptr++) {
			if (!xstrcmp(gres_name, gres_ptr->name))
				resv_gres += gres_ptr->used_cnt;
		}
	}

	return resv_gres;
}

/* Test if a job can be allocated a burst buffer.
 * This may preempt currently active stage-in for higher priority jobs.
 *
 * RET 0: Job can be started now
 *     1: Job exceeds configured limits, continue testing with next job
 *     2: Job needs more resources than currently available can not start,
 *        skip all remaining jobs
 */
static int _test_size_limit(struct job_record *job_ptr, bb_job_t *bb_job)
{
	burst_buffer_info_msg_t *resv_bb;
	needed_gres_t *needed_gres_ptr = NULL;
	struct preempt_bb_recs *preempt_ptr = NULL;
	List preempt_list;
	ListIterator preempt_iter;
	bb_user_t *user_ptr;
	int64_t tmp_g, tmp_u, tmp_j, tmp_r;
	int64_t lim_u, add_space, resv_space = 0;
	int64_t tmp_f;	/* Could go negative due to reservations */
	int64_t add_total_space_needed = 0, add_user_space_needed = 0;
	int64_t add_total_space_avail  = 0, add_user_space_avail  = 0;
	int64_t add_total_gres_needed  = 0, add_total_gres_avail  = 0;
	time_t now = time(NULL);
	bb_alloc_t *bb_ptr = NULL;
	int d, i, j, k;
	char jobid_buf[32];

	xassert(bb_job);
	add_space = bb_job->total_size + bb_job->persist_add;

	/* Determine if burst buffer can be allocated now for the job.
	 * If not, determine how much space must be free. */
	if (bb_limit_test(job_ptr->user_id, bb_job->account, bb_job->partition,
			  bb_job->qos, add_space, &bb_state) < 1) {
		debug("%s: %s requested space above limit", __func__,
		      jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
		return 1;
	}

	resv_bb = job_test_bb_resv(job_ptr, now);
	if (resv_bb) {
		burst_buffer_info_t *resv_bb_ptr;
		for (i = 0, resv_bb_ptr = resv_bb->burst_buffer_array;
		     i < resv_bb->record_count; i++, resv_bb_ptr++) {
			if (xstrcmp(resv_bb_ptr->name, bb_state.name))
				continue;
			resv_bb_ptr->used_space =
				bb_granularity(resv_bb_ptr->used_space,
					       bb_state.bb_config.granularity);
			resv_space += resv_bb_ptr->used_space;
		}
	}

	if (bb_state.bb_config.user_size_limit != NO_VAL64) {
		user_ptr = bb_find_user_rec(job_ptr->user_id, &bb_state);
		tmp_u = user_ptr->size;
		tmp_j = add_space;
		lim_u = bb_state.bb_config.user_size_limit;
		if (tmp_u + tmp_j > lim_u)
			add_user_space_needed = tmp_u + tmp_j - lim_u;
	}
	add_total_space_needed = bb_state.used_space + add_space + resv_space -
				 bb_state.total_space;
	needed_gres_ptr = xmalloc(sizeof(needed_gres_t) * bb_job->gres_cnt);
	for (i = 0; i < bb_job->gres_cnt; i++) {
		needed_gres_ptr[i].name = xstrdup(bb_job->gres_ptr[i].name);
		for (j = 0; j < bb_state.bb_config.gres_cnt; j++) {
			if (strcmp(bb_job->gres_ptr[i].name,
				   bb_state.bb_config.gres_ptr[j].name))
				continue;
			tmp_g = bb_granularity(bb_job->gres_ptr[i].count,
					       bb_state.bb_config.gres_ptr[j].
					       granularity);
			bb_job->gres_ptr[i].count = tmp_g;
			if (tmp_g > bb_state.bb_config.gres_ptr[j].avail_cnt) {
				debug("%s: %s requests more %s GRES than"
				      "configured", __func__,
				      jobid2fmt(job_ptr, jobid_buf,
						sizeof(jobid_buf)),
				      bb_job->gres_ptr[i].name);
				_free_needed_gres_struct(needed_gres_ptr,
							 bb_job->gres_cnt);
				if (resv_bb)
					slurm_free_burst_buffer_info_msg(resv_bb);
				return 1;
			}
			tmp_r = _get_bb_resv(bb_job->gres_ptr[i].name,resv_bb);
			tmp_f = bb_state.bb_config.gres_ptr[j].avail_cnt -
				bb_state.bb_config.gres_ptr[j].used_cnt - tmp_r;
			if (tmp_g > tmp_f)
				needed_gres_ptr[i].add_cnt = tmp_g - tmp_f;
			add_total_gres_needed += needed_gres_ptr[i].add_cnt;
			break;
		}
		if (j >= bb_state.bb_config.gres_cnt) {
			debug("%s: %s requests %s GRES which are undefined",
			      __func__,
			      jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)),
			      bb_job->gres_ptr[i].name);
			_free_needed_gres_struct(needed_gres_ptr,
						 bb_job->gres_cnt);
			if (resv_bb)
				slurm_free_burst_buffer_info_msg(resv_bb);
			return 1;
		}
	}

	if (resv_bb)
		slurm_free_burst_buffer_info_msg(resv_bb);

	if ((add_total_space_needed <= 0) &&
	    (add_user_space_needed  <= 0) && (add_total_gres_needed <= 0)) {
		_free_needed_gres_struct(needed_gres_ptr, bb_job->gres_cnt);
		return 0;
	}

	/* Identify candidate burst buffers to revoke for higher priority job */
	preempt_list = list_create(bb_job_queue_del);
	for (i = 0; i < BB_HASH_SIZE; i++) {
		bb_ptr = bb_state.bb_ahash[i];
		while (bb_ptr) {
			if (bb_ptr->job_id &&
			    (bb_ptr->use_time > now) &&
			    (bb_ptr->use_time > job_ptr->start_time)) {
				preempt_ptr = xmalloc(sizeof(
						struct preempt_bb_recs));
				preempt_ptr->bb_ptr = bb_ptr;
				preempt_ptr->job_id = bb_ptr->job_id;
				preempt_ptr->size = bb_ptr->size;
				preempt_ptr->use_time = bb_ptr->use_time;
				preempt_ptr->user_id = bb_ptr->user_id;
				list_push(preempt_list, preempt_ptr);
				add_total_space_avail += bb_ptr->size;
				if (bb_ptr->user_id == job_ptr->user_id);
					add_user_space_avail += bb_ptr->size;
				if (add_total_gres_needed<add_total_gres_avail)
					j = bb_ptr->gres_cnt;
				else
					j = 0;
				for ( ; j < bb_ptr->gres_cnt; j++) {
					d = needed_gres_ptr[j].add_cnt -
					    needed_gres_ptr[j].avail_cnt;
					if (d <= 0)
						continue;
					for (k = 0; k < bb_job->gres_cnt; k++){
						if (strcmp(needed_gres_ptr[j].name,
							   bb_job->gres_ptr[k].name))
							continue;
						if (bb_job->gres_ptr[k].count <
						    d) {
							d = bb_job->
							    gres_ptr[k].count;
						}
						add_total_gres_avail += d;
						needed_gres_ptr[j].avail_cnt+=d;
					}
				}
			}
			bb_ptr = bb_ptr->next;
		}
	}

	if ((add_total_space_avail >= add_total_space_needed) &&
	    (add_user_space_avail  >= add_user_space_needed)  &&
	    (add_total_gres_avail  >= add_total_gres_needed)) {
		list_sort(preempt_list, bb_preempt_queue_sort);
		preempt_iter = list_iterator_create(preempt_list);
		while ((preempt_ptr = list_next(preempt_iter)) &&
		       (add_total_space_needed || add_user_space_needed ||
			add_total_gres_needed)) {
			bool do_preempt = false;
			if (add_user_space_needed &&
			    (preempt_ptr->user_id == job_ptr->user_id)) {
				do_preempt = true;
				add_user_space_needed  -= preempt_ptr->size;
				add_total_space_needed -= preempt_ptr->size;
			}
			if ((add_total_space_needed > add_user_space_needed) &&
			    (preempt_ptr->user_id != job_ptr->user_id)) {
				do_preempt = true;
				add_total_space_needed -= preempt_ptr->size;
			}
			if (add_total_gres_needed) {
				for (j = 0; j < bb_job->gres_cnt; j++) {
					d = needed_gres_ptr[j].add_cnt;
					if (d <= 0)
						continue;
					for (k = 0;
					     k < preempt_ptr->bb_ptr->gres_cnt;
					     k++) {
						if (strcmp(needed_gres_ptr[j].name,
							   preempt_ptr->bb_ptr->
							   gres_ptr[k].name))
							continue;
						if (preempt_ptr->bb_ptr->
						    gres_ptr[k].used_cnt < d) {
							d = preempt_ptr->bb_ptr->
							    gres_ptr[k].used_cnt;
						}
						add_total_gres_needed -= d;
						needed_gres_ptr[j].add_cnt -= d;
						do_preempt = true;
					}
				}
			}
			if (do_preempt) {
				preempt_ptr->bb_ptr->cancelled = true;
				preempt_ptr->bb_ptr->end_time = 0;
				preempt_ptr->bb_ptr->state = BB_STATE_TEARDOWN;
				preempt_ptr->bb_ptr->state_time = time(NULL);
				_queue_teardown(preempt_ptr->job_id,
						preempt_ptr->user_id, true);
				if (bb_state.bb_config.debug_flag) {
					info("%s: %s: Preempting stage-in of "
					     "job %u for %s", plugin_type,
					     __func__, preempt_ptr->job_id,
					     jobid2fmt(job_ptr, jobid_buf,
						       sizeof(jobid_buf)));
				}
			}
		}
		list_iterator_destroy(preempt_iter);
	}
	FREE_NULL_LIST(preempt_list);
	_free_needed_gres_struct(needed_gres_ptr, bb_job->gres_cnt);

	return 2;
}

/* Handle timeout of burst buffer events:
 * 1. Purge per-job burst buffer records when the stage-out has completed and
 *    the job has been purged from Slurm
 * 2. Test for StageInTimeout events
 * 3. Test for StageOutTimeout events
 */
static void _timeout_bb_rec(void)
{
	bb_alloc_t **bb_pptr, *bb_alloc = NULL;
	struct job_record *job_ptr;
	int i;

	if (bb_state.bb_config.flags & BB_FLAG_EMULATE_CRAY)
		return;

	for (i = 0; i < BB_HASH_SIZE; i++) {
		bb_pptr = &bb_state.bb_ahash[i];
		bb_alloc = bb_state.bb_ahash[i];
		while (bb_alloc) {
			if (bb_alloc->seen_time < bb_state.last_load_time) {
				if (bb_alloc->job_id == 0) {
					info("%s: Persistent burst buffer %s "
					     "purged",
					     __func__, bb_alloc->name);
				} else if (bb_state.bb_config.debug_flag) {
					info("%s: burst buffer for job %u "
					     "purged",
					     __func__, bb_alloc->job_id);
				}
				bb_limit_rem(bb_alloc->user_id,
					     bb_alloc->account,
					     bb_alloc->partition, bb_alloc->qos,
					     bb_alloc->size, &bb_state);
				*bb_pptr = bb_alloc->next;
				bb_free_alloc_buf(bb_alloc);
				break;
			}
			if (bb_alloc->state == BB_STATE_COMPLETE) {
				job_ptr = find_job_record(bb_alloc->job_id);
				if (!job_ptr || IS_JOB_PENDING(job_ptr)) {
					/* Job purged or BB preempted */
					*bb_pptr = bb_alloc->next;
					bb_free_alloc_buf(bb_alloc);
					break;
				}
			}
			bb_pptr = &bb_alloc->next;
			bb_alloc = bb_alloc->next;
		}
	}
}

/* Translate a batch script or interactive burst_buffer options into in
 * appropriate burst_buffer argument */
static int _parse_bb_opts(struct job_descriptor *job_desc, uint64_t *bb_size,
			  uid_t submit_uid)
{
	char *end_ptr = NULL, *script, *save_ptr = NULL;
	char *bb_access = NULL, *bb_name = NULL, *bb_type = NULL, *capacity;
	char *job_access = NULL, *job_type = NULL;
	char *sub_tok, *tok, *persistent = NULL;
	uint64_t byte_cnt = 0, tmp_cnt;
	uint32_t node_cnt = 0, swap_cnt = 0;
	int rc = SLURM_SUCCESS;
	bool enable_persist = false, hurry;

	xassert(bb_size);
	*bb_size = 0;
	if (!job_desc->script)
		return _parse_interactive(job_desc, bb_size);

	if (validate_operator(submit_uid) ||
	    (bb_state.bb_config.flags & BB_FLAG_ENABLE_PERSISTENT))
		enable_persist = true;

	script = xstrdup(job_desc->script);
	tok = strtok_r(script, "\n", &save_ptr);
	while (tok) {
		tmp_cnt = 0;
		if (tok[0] != '#') {
			break;	/* Quit at first non-comment */
		} else if ((tok[1] == 'B') && (tok[2] == 'B')) {
			hurry = false;
			tok += 3;
			while (isspace(tok[0]))
				tok++;
			if (!strncmp(tok, "create_persistent", 17) &&
			    !enable_persist) {
				info("%s: User %d disabled from creating persistent burst buffer",
				     __func__, submit_uid);
				rc = ESLURM_INVALID_BURST_BUFFER_REQUEST;
				break;
			} else if (!strncmp(tok, "create_persistent", 17)) {
				if ((sub_tok = strstr(tok, "capacity=")))
					tmp_cnt = bb_get_size_num(sub_tok + 9,
						  bb_state.bb_config.granularity);
				if (tmp_cnt == 0) {
					rc = ESLURM_INVALID_BURST_BUFFER_REQUEST;
					break;
				}
				if ((sub_tok = strstr(tok, "name="))) {
					bb_name = xstrdup(sub_tok + 5);
					if ((sub_tok = strchr(bb_name, ' ')))
						sub_tok[0] = '\0';
				} else {
					rc = ESLURM_INVALID_BURST_BUFFER_REQUEST;
					break;
				}
				if ((bb_name[0] >= '0') && (bb_name[0] <= '9')){
					rc = ESLURM_INVALID_BURST_BUFFER_REQUEST;
					break;
				}
				if ((sub_tok = strstr(tok, "access="))) {
					bb_access = xstrdup(sub_tok + 7);
					if ((sub_tok = strchr(bb_access, ' ')))
						sub_tok[0] = '\0';
				}
				if ((sub_tok = strstr(tok, "type="))) {
					bb_type = xstrdup(sub_tok + 5);
					if ((sub_tok = strchr(bb_type, ' ')))
						sub_tok[0] = '\0';
				}
				xstrfmtcat(persistent,
					   "SLURM_PERSISTENT_CREATE=NAME=%s,SIZE=%"PRIu64,
					   bb_name, tmp_cnt);
				if (bb_access) {
					xstrfmtcat(persistent,
						   ",ACCESS=%s", bb_access);
				}
				if (bb_type) {
					xstrfmtcat(persistent,
						   ",TYPE=%s", bb_type);
				}
				xstrcat(persistent, " ");
				xfree(bb_access),
				xfree(bb_name);
				xfree(bb_type);
				*bb_size += tmp_cnt;
			} else if (!strncmp(tok, "destroy_persistent", 17) &&
				   !enable_persist) {
				info("%s: User %d disabled from destroying persistent burst buffer",
				     __func__, submit_uid);
				rc = ESLURM_INVALID_BURST_BUFFER_REQUEST;
				break;
			} else if (!strncmp(tok, "destroy_persistent", 17)) {
				if ((sub_tok = strstr(tok, "name="))) {
					bb_name = xstrdup(sub_tok + 5);
					if ((sub_tok = strchr(bb_name, ' ')))
						sub_tok[0] = '\0';
				} else
					rc = ESLURM_INVALID_BURST_BUFFER_REQUEST;
				if ((sub_tok = strstr(tok, "hurry"))) {
					hurry = true;
					sub_tok[0] = '\0';
				} else if ((sub_tok = strchr(bb_name, ' ')))
					sub_tok[0] = '\0';
				xstrfmtcat(persistent,
					   "SLURM_PERSISTENT_DESTROY=NAME=%s",
					   bb_name);
				if (hurry)
					xstrcat(persistent, "HURRY ");
				else
					xstrcat(persistent, " ");
				xfree(bb_name);
			}
		} else if ((tok[1] == 'D') && (tok[2] == 'W')) {
			/* We just capture the size requirement and leave other
			 * parsing to Cray's tools */
			tok += 3;
			while (isspace(tok[0]) && (tok[0] != '\0'))
				tok++;
			if (!strncmp(tok, "jobdw", 5) &&
			    (capacity = strstr(tok, "capacity="))) {
				tmp_cnt = bb_get_size_num(capacity + 9,
						bb_state.bb_config.granularity);
				if (tmp_cnt == 0) {
					rc = ESLURM_INVALID_BURST_BUFFER_CHANGE;
					break;
				}
				if (tmp_cnt & BB_SIZE_IN_NODES) {
					node_cnt += tmp_cnt &
						   (~BB_SIZE_IN_NODES);
				} else
					byte_cnt += tmp_cnt;
				if ((sub_tok = strstr(tok, "access_mode="))) {
					job_access = xstrdup(sub_tok + 12);
					sub_tok = strchr(job_access, ' ');
					if (sub_tok)
						sub_tok[0] = '\0';
				}
				if ((sub_tok = strstr(tok, "type="))) {
					job_type = xstrdup(sub_tok + 5);
					sub_tok = strchr(job_type, ' ');
					if (sub_tok)
						sub_tok[0] = '\0';
				}
			} else if (!strncmp(tok, "swap", 4)) {
				tok += 4;
				while (isspace(tok[0]) && (tok[0] != '\0'))
					tok++;
				swap_cnt += strtol(tok, &end_ptr, 10);
			} else if (!strncmp(tok, "persistentdw", 12)) {
				xstrcat(persistent, "SLURM_PERSISTENT_USE ");
			}
		}
		tok = strtok_r(NULL, "\n", &save_ptr);
	}
	xfree(script);

	if ((rc == SLURM_SUCCESS) &&
	    (byte_cnt || node_cnt || swap_cnt || persistent)) {
		xfree(job_desc->burst_buffer);
		if (swap_cnt) {
			uint32_t job_nodes;
			if ((job_desc->max_nodes == 0) ||
			    (job_desc->max_nodes == NO_VAL)) {
				job_nodes = 1;
				info("%s: user %u submitted job with swap "
				     "space specification, but no node count "
				     "specification",
				     __func__, job_desc->user_id);

			} else {
				job_nodes = job_desc->max_nodes;
			}
			xstrfmtcat(job_desc->burst_buffer,
				   "SLURM_SWAP=%uGB(%uNodes) ",
				   swap_cnt, job_nodes);
			byte_cnt += (swap_cnt * 1024 * 1024 * 1024) * job_nodes;
		}
		if (byte_cnt) {
			/* Include cache plus swap space */
			if (job_desc->burst_buffer)
				xstrcat(job_desc->burst_buffer, " ");
			xstrfmtcat(job_desc->burst_buffer,
				   "SLURM_JOB=SIZE=%"PRIu64"", byte_cnt);
			if (job_access) {
				xstrfmtcat(job_desc->burst_buffer,
				   ",ACCESS=%s", job_access);
			}
			if (job_type) {
				xstrfmtcat(job_desc->burst_buffer,
				   ",TYPE=%s", job_type);
			}
			xstrcat(job_desc->burst_buffer, " ");
			*bb_size += byte_cnt;
		}
		if (node_cnt) {
			xstrfmtcat(job_desc->burst_buffer,
				   "SLURM_GRES=nodes:%u ", node_cnt);
		}
		if (persistent) {
			xstrcat(job_desc->burst_buffer, persistent);
		}
	}
	xfree(job_access);
	xfree(job_type);
	xfree(persistent);

	return rc;
}

/* Parse interactive burst_buffer options into an appropriate burst_buffer
 * argument */
static int _parse_interactive(struct job_descriptor *job_desc,
			      uint64_t *bb_size)
{
	char *access = NULL, *sep = "", *type = NULL;
	char *end_ptr = NULL, *tok;
	int64_t tmp_cnt;
	uint64_t byte_cnt = 0;
	uint32_t node_cnt = 0, swap_cnt = 0;
	int rc = SLURM_SUCCESS;

	if (!job_desc->burst_buffer)
		return rc;

	if ((tok = strstr(job_desc->burst_buffer, "capacity="))) {
		tmp_cnt = bb_get_size_num(tok + 9,
					  bb_state.bb_config.granularity);
		if (tmp_cnt == 0)
			return ESLURM_INVALID_BURST_BUFFER_CHANGE;
		if (tmp_cnt & BB_SIZE_IN_NODES)
			node_cnt += tmp_cnt & (~BB_SIZE_IN_NODES);
		else
			byte_cnt += tmp_cnt;
	}

	if ((tok = strstr(job_desc->burst_buffer, "swap=")))
		swap_cnt = strtol(tok + 5, &end_ptr, 10);

	xfree(job_desc->burst_buffer);
	if ((rc == SLURM_SUCCESS) && (byte_cnt || node_cnt || swap_cnt)) {
		if (swap_cnt) {
			uint32_t job_nodes;
			if ((job_desc->max_nodes == 0) ||
			    (job_desc->max_nodes == NO_VAL)) {
				job_nodes = 1;
				info("%s: user %u submitted job with swap "
				     "space specification, but no node count "
				     "specification",
				     __func__, job_desc->user_id);

			} else {
				job_nodes = job_desc->max_nodes;
			}
			xstrfmtcat(job_desc->burst_buffer,
				   "SLURM_SWAP=%uGB(%uNodes)",
				   swap_cnt, job_nodes);
			sep = " ";
			byte_cnt += swap_cnt * 1024 * 1024 * job_nodes;
		}
		if (byte_cnt) {
			xstrfmtcat(job_desc->burst_buffer,
				   "%sSLURM_JOB=SIZE=%"PRIu64"", sep, byte_cnt);
			sep = " ";
			*bb_size += byte_cnt;
			if (access) {
				xstrfmtcat(job_desc->burst_buffer,
					   ",ACCESS=%s", access);
			}
			if (type) {
				xstrfmtcat(job_desc->burst_buffer,
					   ",TYPE=%s", type);
			}
		}
		if (node_cnt) {
			xstrfmtcat(job_desc->burst_buffer,
				   "%sSLURM_GRES=nodes:%u", sep, node_cnt);
		}
	}
	xfree(access);
	xfree(type);

	return rc;
}

/* For interactive jobs, build a script containing the relevant DataWarp
 * commands, as needed by the Cray API */
static int _build_bb_script(struct job_record *job_ptr, char *script_file)
{
	char *in_buf, *out_buf = NULL;
	char *sep, *tok, *tmp;
	int i, rc;

	xstrcat(out_buf, "#!/bin/bash\n");

	if ((tok = strstr(job_ptr->burst_buffer, "swap="))) {
		tok += 5;
		i = strtol(tok, NULL, 10);
		xstrfmtcat(out_buf, "#DW swap=%dGiB\n", i);
	}

	in_buf = xstrdup(job_ptr->burst_buffer);
	tmp = in_buf;
	if ((tok = strstr(tmp, "jobdw="))) {
		tok += 6;
		sep = NULL;
		if ((tok[0] == '\'') || (tok[0] == '\"')) {
			sep = strchr(tok + 1, tok[0]);
			if (sep) {
				tok++;
				sep[0] = '\0';
			}
		}
		if (!sep) {
			sep = tok;
			while ((sep[0] != ' ') && (sep[0] != '\0'))
				sep++;
			sep[0] = '\0';
		}
		xstrfmtcat(out_buf, "#DW jobdw %s\n", tok);
	}
	xfree(in_buf);

	rc = _write_file(script_file, out_buf);
	xfree(out_buf);
	return rc;
}

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Read and validate configuration file here. Spawn thread to
 * periodically read Datawarp state.
 */
extern int init(void)
{
	pthread_attr_t attr;

	pthread_mutex_lock(&bb_state.bb_mutex);
	bb_load_config(&bb_state, (char *)plugin_type); /* Removes "const" */
	_test_config();
	if (bb_state.bb_config.debug_flag)
		info("%s: %s", plugin_type,  __func__);
	bb_alloc_cache(&bb_state);
	slurm_attr_init(&attr);
	while (pthread_create(&bb_state.bb_thread, &attr, _bb_agent, NULL)) {
		if (errno != EAGAIN) {
			fatal("%s: Unable to start thread: %m", __func__);
			break;
		}
		usleep(100000);
	}
	slurm_attr_destroy(&attr);
	if (!state_save_loc)
		state_save_loc = slurm_get_state_save_location();
	pthread_mutex_unlock(&bb_state.bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * fini() is called when the plugin is unloaded. Free all memory and shutdown
 * threads.
 */
extern int fini(void)
{
	pthread_mutex_lock(&bb_state.bb_mutex);
	if (bb_state.bb_config.debug_flag)
		info("%s: %s", plugin_type,  __func__);

	pthread_mutex_lock(&bb_state.term_mutex);
	bb_state.term_flag = true;
	pthread_cond_signal(&bb_state.term_cond);
	pthread_mutex_unlock(&bb_state.term_mutex);

	if (bb_state.bb_thread) {
		pthread_join(bb_state.bb_thread, NULL);
		bb_state.bb_thread = 0;
	}
	bb_clear_config(&bb_state.bb_config, true);
	bb_clear_cache(&bb_state);
	xfree(state_save_loc);
	pthread_mutex_unlock(&bb_state.bb_mutex);

	return SLURM_SUCCESS;
}

/* Identify and purge any vestigial buffers (i.e. we have a job buffer, but
 * the matching job is either gone or completed) */
static void _purge_vestigial_bufs(void)
{
	bb_alloc_t *bb_alloc = NULL;
	int i;

	for (i = 0; i < BB_HASH_SIZE; i++) {
		bb_alloc = bb_state.bb_ahash[i];
		while (bb_alloc) {
			if (bb_alloc->job_id &&
			    !find_job_record(bb_alloc->job_id)) {
				info("%s: Purging vestigial buffer for job %u",
				     plugin_type, bb_alloc->job_id);
				_queue_teardown(bb_alloc->job_id,
						bb_alloc->user_id, false);
			}
			bb_alloc = bb_alloc->next;
		}
	}
}

/*
 * Return the total burst buffer size in MB
 */
extern uint64_t bb_p_get_system_size(void)
{
	uint64_t size = 0;

	pthread_mutex_lock(&bb_state.bb_mutex);
	size = bb_state.total_space / (1024 * 1024);	/* bytes to MB */
	pthread_mutex_unlock(&bb_state.bb_mutex);
	return size;
}

/*
 * Load the current burst buffer state (e.g. how much space is available now).
 * Run at the beginning of each scheduling cycle in order to recognize external
 * changes to the burst buffer state (e.g. capacity is added, removed, fails,
 * etc.)
 *
 * init_config IN - true if called as part of slurmctld initialization
 * Returns a SLURM errno.
 */
extern int bb_p_load_state(bool init_config)
{
	if (!init_config)
		return SLURM_SUCCESS;

	/* In practice the Cray APIs are too slow to run inline on each
	 * scheduling cycle. Do so on a periodic basis from _bb_agent(). */
	if (bb_state.bb_config.debug_flag)
		debug("%s: %s", plugin_type,  __func__);
	_load_state(init_config);	/* Has own locking */
	pthread_mutex_lock(&bb_state.bb_mutex);
	bb_set_tres_pos(&bb_state);
	_purge_vestigial_bufs();
	pthread_mutex_unlock(&bb_state.bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Note configuration may have changed. Handle changes in BurstBufferParameters.
 *
 * Returns a SLURM errno.
 */
extern int bb_p_reconfig(void)
{
	char *old_default_pool;

	pthread_mutex_lock(&bb_state.bb_mutex);
	if (bb_state.bb_config.debug_flag)
		info("%s: %s", plugin_type,  __func__);
	old_default_pool = bb_state.bb_config.default_pool;
	bb_state.bb_config.default_pool = NULL;
	bb_load_config(&bb_state, (char *)plugin_type); /* Remove "const" */
	if (!bb_state.bb_config.default_pool)
		bb_state.bb_config.default_pool = old_default_pool;
	else
		xfree(old_default_pool);
	_test_config();
	pthread_mutex_unlock(&bb_state.bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Pack current burst buffer state information for network transmission to
 * user (e.g. "scontrol show burst")
 *
 * Returns a SLURM errno.
 */
extern int bb_p_state_pack(uid_t uid, Buf buffer, uint16_t protocol_version)
{
	uint32_t rec_count = 0;

	pthread_mutex_lock(&bb_state.bb_mutex);
	packstr(bb_state.name, buffer);
	bb_pack_state(&bb_state, buffer, protocol_version);
	if (bb_state.bb_config.private_data == 0)
		uid = 0;	/* Any user can see all data */
	rec_count = bb_pack_bufs(uid, &bb_state, buffer, protocol_version);
	(void) bb_pack_usage(uid, &bb_state, buffer, protocol_version);
	if (bb_state.bb_config.debug_flag) {
		debug("%s: %s: record_count:%u",
		      plugin_type,  __func__, rec_count);
	}
	pthread_mutex_unlock(&bb_state.bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Preliminary validation of a job submit request with respect to burst buffer
 * options. Performed after setting default account + qos, but prior to
 * establishing job ID or creating script file.
 *
 * Returns a SLURM errno.
 */
extern int bb_p_job_validate(struct job_descriptor *job_desc,
			     uid_t submit_uid)
{
	bool have_gres = false, have_persist = false, have_swap = false;
	uint64_t bb_size = 0;
	char *key;
	int i, rc;

	rc = _parse_bb_opts(job_desc, &bb_size, submit_uid);
	if (rc != SLURM_SUCCESS)
		return rc;

	if (job_desc->burst_buffer) {
		if (bb_state.bb_config.debug_flag) {
			info("%s: %s: job_user_id:%u, submit_uid:%d",
			     plugin_type, __func__, job_desc->user_id, submit_uid);
			info("%s: burst_buffer:%s", __func__, job_desc->burst_buffer);
			info("%s: script:%s", __func__, job_desc->script);
		}

		if (job_desc->user_id == 0) {
			info("%s: User root can not allocate burst buffers",
			     __func__);
			return EPERM;
		}

		key = strstr(job_desc->burst_buffer, "SLURM_JOB=SIZE=");
		if (key) {
			bb_size = bb_get_size_num(key + 15,
						bb_state.bb_config.granularity);
		}
		if (strstr(job_desc->burst_buffer, "SLURM_GRES="))
			have_gres = true;
		key = strstr(job_desc->burst_buffer,"SLURM_PERSISTENT_CREATE=");
		if (key) {
			have_persist = true;
			key = strstr(key, "SIZE=");
			if (key) {
				bb_size += bb_get_size_num(key + 5,
						bb_state.bb_config.granularity);
			}
		}
		if (strstr(job_desc->burst_buffer, "SLURM_PERSISTENT_DESTROY="))
			have_persist = true;
		if (strstr(job_desc->burst_buffer, "SLURM_PERSISTENT_USE"))
			have_persist = true;
		if (strstr(job_desc->burst_buffer, "SLURM_SWAP="))
			have_swap = true;
	}
	if ((bb_size == 0) && !have_gres && !have_persist && !have_swap)
		return SLURM_SUCCESS;

	pthread_mutex_lock(&bb_state.bb_mutex);
	if (bb_state.bb_config.allow_users) {
		for (i = 0; bb_state.bb_config.allow_users[i]; i++) {
			if (job_desc->user_id ==
			    bb_state.bb_config.allow_users[i])
				break;
		}
		if (bb_state.bb_config.allow_users[i] == 0) {
			rc = ESLURM_BURST_BUFFER_PERMISSION;
			goto fini;
		}
	}

	if (bb_state.bb_config.deny_users) {
		for (i = 0; bb_state.bb_config.deny_users[i]; i++) {
			if (job_desc->user_id ==
			    bb_state.bb_config.deny_users[i])
				break;
		}
		if (bb_state.bb_config.deny_users[i] != 0) {
			rc = ESLURM_BURST_BUFFER_PERMISSION;
			goto fini;
		}
	}

	if (bb_limit_test(job_desc->user_id, job_desc->account,
			  job_desc->partition, job_desc->qos, bb_size,
			  &bb_state) < 1) {
		rc = ESLURM_BURST_BUFFER_LIMIT;
		goto fini;
	}

fini:	job_desc->shared = 0;	/* Compute nodes can not be shared */
	pthread_mutex_unlock(&bb_state.bb_mutex);

	return rc;
}

static void _purge_job_file(char *job_dir, char *file_name)
{
	char *tmp = NULL;
	xstrfmtcat(tmp, "%s/%s", job_dir, file_name);
	(void) unlink(tmp);
	xfree(tmp);
}

static void _purge_job_files(char *job_dir)
{
	_purge_job_file(job_dir, "setup_env");
	_purge_job_file(job_dir, "data_in_env");
	_purge_job_file(job_dir, "pre_run_env");
	_purge_job_file(job_dir, "post_run_env");
	_purge_job_file(job_dir, "data_out_env");
	_purge_job_file(job_dir, "teardown_env");
}

/* Add key=value pairs from "resp_msg" to the job's environment */
static void _update_job_env(struct job_record *job_ptr, char *file_path)
{
	struct stat stat_buf;
	char *data_buf = NULL, *start, *sep;
	int path_fd, i, inx = 0, env_cnt = 0;
	size_t read_size;

	/* Read the DataWarp generated environment variable file */
	path_fd = open(file_path, 0);
	if (path_fd == -1) {
		error("%s: open error on file %s: %m", __func__, file_path);
		return;
	}
	fd_set_close_on_exec(path_fd);
	if (fstat(path_fd, &stat_buf) == -1) {
		error("%s: stat error on file %s: %m", __func__, file_path);
		stat_buf.st_size = 2048;
	} else if (stat_buf.st_size == 0)
		goto fini;
	data_buf = xmalloc(stat_buf.st_size);
	while (inx < stat_buf.st_size) {
		read_size = read(path_fd, data_buf + inx, stat_buf.st_size);
		if (read_size > 0) {
			inx += read_size;
		} else if (read_size == 0) {	/* EOF */
			break;
		} else if (read_size < 0) {	/* error */
			if ((errno == EAGAIN) || (errno == EINTR))
				continue;
			error("%s: read error on file %s: %m", __func__,
			      file_path);
			break;
		}
	}
	if (bb_state.bb_config.debug_flag)
		info("%s: %s", __func__, data_buf);

	/* Get count of environment variables in the file */
	env_cnt = 0;
	if (data_buf) {
		for (i = 0; data_buf[i]; i++) {
			if (data_buf[i] == '=')
				env_cnt++;
		}
	}

	/* Add to supplemental environment variables (in job record) */
	if (env_cnt) {
		job_ptr->details->env_sup =
			xrealloc(job_ptr->details->env_sup,
				 sizeof(char *) *
				 (job_ptr->details->env_cnt + env_cnt));
		start = data_buf;
		for (i = 0; (i < env_cnt) && start[0]; i++) {
			sep = strchr(start, '\n');
			if (sep)
				sep[0] = '\0';
			job_ptr->details->env_sup[job_ptr->details->env_cnt++] =
				xstrdup(start);
			if (sep)
				start = sep + 1;
			else
				break;
		}
	}

fini:	xfree(data_buf);
	close(path_fd);
}

/*
 * Secondary validation of a job submit request with respect to burst buffer
 * options. Performed after establishing job ID and creating script file.
 *
 * NOTE: We run several DW APIs at job submit time so that we can notify the
 * user immediately if there is some error, although that can be a relatively
 * slow operation.
 *
 * Returns a SLURM errno.
 */
extern int bb_p_job_validate2(struct job_record *job_ptr, char **err_msg,
			      bool is_job_array)
{
	char *hash_dir = NULL, *job_dir = NULL, *script_file = NULL;
	char *path_file = NULL, *resp_msg = NULL, **script_argv;
	char *dw_cli_path;
	int hash_inx, rc = SLURM_SUCCESS, status = 0;
	char jobid_buf[32];
	bb_job_t *bb_job;
	DEF_TIMERS;

	if ((job_ptr->burst_buffer == NULL) ||
	    (job_ptr->burst_buffer[0] == '\0'))
		return rc;

//FIXME: Add support for job arrays
	if (job_ptr->array_recs) {
		if (err_msg) {
			xfree(*err_msg);
			xstrfmtcat(*err_msg,
				   "%s: Burst buffers not currently supported for job arrays",
				   plugin_type);
		}
		return ESLURM_INVALID_BURST_BUFFER_REQUEST;
	}

	/* Initialization */
	pthread_mutex_lock(&bb_state.bb_mutex);
	bb_job = _get_bb_job(job_ptr);
	if (bb_job == NULL) {
		pthread_mutex_unlock(&bb_state.bb_mutex);
		return rc;
	}

	if (bb_state.bb_config.debug_flag) {
		info("%s: %s: %s", plugin_type, __func__,
		     jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
	}
	dw_cli_path = xstrdup(bb_state.bb_config.get_sys_state);
	pthread_mutex_unlock(&bb_state.bb_mutex);

	hash_inx = job_ptr->job_id % 10;
	xstrfmtcat(hash_dir, "%s/hash.%d", state_save_loc, hash_inx);
	(void) mkdir(hash_dir, 0700);
	xstrfmtcat(job_dir, "%s/job.%u", hash_dir, job_ptr->job_id);
	(void) mkdir(job_dir, 0700);
	xstrfmtcat(script_file, "%s/script", job_dir);
	xstrfmtcat(path_file, "%s/pathfile", job_dir);
	if (job_ptr->batch_flag == 0)
		rc = _build_bb_script(job_ptr, script_file);

	/* Run "job_process" function, validates user script */
	script_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	script_argv[0] = xstrdup("dw_wlm_cli");
	script_argv[1] = xstrdup("--function");
	script_argv[2] = xstrdup("job_process");
	script_argv[3] = xstrdup("--job");
	xstrfmtcat(script_argv[4], "%s", script_file);
	START_TIMER;
	resp_msg = bb_run_script("job_process",
				 bb_state.bb_config.get_sys_state,
				 script_argv, 2000, &status);
	END_TIMER;
	if (DELTA_TIMER > 200000)	/* 0.2 secs */
		info("%s: job_process ran for %s", __func__, TIME_STR);
	else if (bb_state.bb_config.debug_flag)
		debug("%s: job_process ran for %s", __func__, TIME_STR);
	_log_script_argv(script_argv, resp_msg);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: job_process for job %u status:%u response:%s",
		      __func__, job_ptr->job_id, status, resp_msg);
		if (err_msg) {
			xfree(*err_msg);
			xstrfmtcat(*err_msg, "%s: %s", plugin_type, resp_msg);
		}
		rc = ESLURM_INVALID_BURST_BUFFER_REQUEST;
	}

	/* Run "paths" function, get DataWarp environment variables */
	script_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	script_argv[0] = xstrdup("dw_wlm_cli");
	script_argv[1] = xstrdup("--function");
	script_argv[2] = xstrdup("paths");
	script_argv[3] = xstrdup("--job");
	xstrfmtcat(script_argv[4], "%s", script_file);
	script_argv[5] = xstrdup("--token");
	xstrfmtcat(script_argv[6], "%u", job_ptr->job_id);
	script_argv[7] = xstrdup("--pathfile");
	script_argv[8] = xstrdup(path_file);
	START_TIMER;
	resp_msg = bb_run_script("paths",
				 bb_state.bb_config.get_sys_state,
				 script_argv, 2000, &status);
	END_TIMER;
	if (DELTA_TIMER > 200000)	/* 0.2 secs */
		info("%s: paths ran for %s", __func__, TIME_STR);
	else if (bb_state.bb_config.debug_flag)
		debug("%s: paths ran for %s", __func__, TIME_STR);
	_log_script_argv(script_argv, resp_msg);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: paths for job %u status:%u response:%s",
		      __func__, job_ptr->job_id, status, resp_msg);
		if (err_msg) {
			xfree(*err_msg);
			xstrfmtcat(*err_msg, "%s: %s", plugin_type, resp_msg);
		}
		rc = ESLURM_INVALID_BURST_BUFFER_REQUEST;
	} else {
		_update_job_env(job_ptr, path_file);
	}
	xfree(resp_msg);
	_free_script_argv(script_argv);
	xfree(path_file);
	if (rc != SLURM_SUCCESS)
		goto fini;

	/* Start buffer allocation and stage-in immediately if space */
	pthread_mutex_lock(&bb_state.bb_mutex);
	if (_test_size_limit(job_ptr, bb_job) == 0)
		(void) _alloc_job_bb(job_ptr, bb_job, false);
	pthread_mutex_unlock(&bb_state.bb_mutex);

fini:	/* Clean-up */
	if (rc != SLURM_SUCCESS) {
		pthread_mutex_lock(&bb_state.bb_mutex);
		bb_job_del(&bb_state, job_ptr->job_id);
		pthread_mutex_unlock(&bb_state.bb_mutex);
	}
	if (is_job_array)
		_purge_job_files(job_dir);

	xfree(hash_dir);
	xfree(job_dir);
	xfree(script_file);
	xfree(dw_cli_path);

	return rc;
}

/*
 * Fill in the tres_cnt (in MB) based off the job record and node_cnt
 * NOTE: Based upon job-specific burst buffers, excludes persistent buffers
 * IN job_ptr - job record, set's tres_cnt field
 * IN node_cnt - number of nodes in the job
 * IN locked - if the assoc_mgr tres read locked is locked or not
 */
extern void bb_p_job_set_tres_cnt(struct job_record *job_ptr,
				  uint32_t node_cnt, bool locked)
{
	bb_job_t *bb_job;

	if (!job_ptr->tres_req_cnt) {
		error("%s: Job %u lacks tres_req_cnt field",
		      __func__, job_ptr->job_id);
	}

	pthread_mutex_lock(&bb_state.bb_mutex);
	if ((bb_job = _get_bb_job(job_ptr))) {
		job_ptr->tres_req_cnt[bb_state.tres_pos] =
			bb_job->total_size / (1024 * 1024);
	}
	pthread_mutex_unlock(&bb_state.bb_mutex);
}

/*
 * For a given job, return our best guess if when it might be able to start
 */
extern time_t bb_p_job_get_est_start(struct job_record *job_ptr)
{
	time_t est_start = time(NULL);
	bb_job_t *bb_job;
	char jobid_buf[32];
	int rc;

	if ((job_ptr->burst_buffer == NULL) ||
	    (job_ptr->burst_buffer[0] == '\0'))
		return est_start;

	if (job_ptr->array_recs && (job_ptr->array_task_id == NO_VAL))
		return est_start;

	pthread_mutex_lock(&bb_state.bb_mutex);
	if ((bb_job = _get_bb_job(job_ptr)) == NULL) {
		pthread_mutex_unlock(&bb_state.bb_mutex);
		return est_start;
	}

	if (bb_state.bb_config.debug_flag) {
		info("%s: %s: %s",
		     plugin_type, __func__,
		     jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
	}

	if ((bb_job->persist_add == 0) && (bb_job->swap_size == 0) &&
	    (bb_job->total_size == 0)) {
		/* Only deleting or using persistent buffers, can run now */
	} else if (bb_job->state == BB_STATE_PENDING) {
		rc = _test_size_limit(job_ptr, bb_job);
		if (rc == 0) {		/* Could start now */
			;
		} else if (rc == 1) {	/* Exceeds configured limits */
			est_start += 365 * 24 * 60 * 60;
		} else {		/* No space currently available */
			est_start = MAX(est_start, bb_state.next_end_time);
		}
	} else {	/* Allocation or staging in progress */
		est_start++;
	}
	pthread_mutex_unlock(&bb_state.bb_mutex);

	return est_start;
}

/*
 * Attempt to allocate resources and begin file staging for pending jobs.
 */
extern int bb_p_job_try_stage_in(List job_queue)
{
	job_queue_rec_t *job_rec;
	List job_candidates;
	ListIterator job_iter;
	struct job_record *job_ptr;
	bb_job_t *bb_job;
	int rc;

	pthread_mutex_lock(&bb_state.bb_mutex);
	if (bb_state.bb_config.debug_flag)
		info("%s: %s", plugin_type,  __func__);

	/* Identify candidates to be allocated burst buffers */
	job_candidates = list_create(_job_queue_del);
	job_iter = list_iterator_create(job_queue);
	while ((job_ptr = list_next(job_iter))) {
		if (!IS_JOB_PENDING(job_ptr) ||
		    (job_ptr->start_time == 0) ||
		    (job_ptr->burst_buffer == NULL) ||
		    (job_ptr->burst_buffer[0] == '\0'))
			continue;
		if (job_ptr->array_recs && (job_ptr->array_task_id == NO_VAL))
			continue;
		bb_job = _get_bb_job(job_ptr);
		if (bb_job == NULL)
			continue;
		job_rec = xmalloc(sizeof(job_queue_rec_t));
		job_rec->job_ptr = job_ptr;
		job_rec->bb_job = bb_job;
		list_push(job_candidates, job_rec);
	}
	list_iterator_destroy(job_iter);

	/* Sort in order of expected start time */
	list_sort(job_candidates, bb_job_queue_sort);

	bb_set_use_time(&bb_state);
	job_iter = list_iterator_create(job_candidates);
	while ((job_rec = list_next(job_iter))) {
		job_ptr = job_rec->job_ptr;
		bb_job = job_rec->bb_job;
		if (bb_job->state >= BB_STATE_STAGING_IN)
			continue;	/* Job was already allocated a buffer */

		rc = _test_size_limit(job_ptr, bb_job);
		if (rc == 0)		/* Could start now */
			(void) _alloc_job_bb(job_ptr, bb_job, true);
		else if (rc == 1)	/* Exceeds configured limits */
			continue;
		else			/* No space currently available */
			break;
	}
	list_iterator_destroy(job_iter);
	pthread_mutex_unlock(&bb_state.bb_mutex);
	FREE_NULL_LIST(job_candidates);

	return SLURM_SUCCESS;
}

/*
 * Determine if a job's burst buffer stage-in is complete
 * job_ptr IN - Job to test
 * test_only IN - If false, then attempt to allocate burst buffer if possible
 *
 * RET: 0 - stage-in is underway
 *      1 - stage-in complete
 *     -1 - stage-in not started or burst buffer in some unexpected state
 */
extern int bb_p_job_test_stage_in(struct job_record *job_ptr, bool test_only)
{
	bb_job_t *bb_job;
	int rc = 1;
	char jobid_buf[32];

	if ((job_ptr->burst_buffer == NULL) ||
	    (job_ptr->burst_buffer[0] == '\0'))
		return 1;

	if (job_ptr->array_recs && (job_ptr->array_task_id == NO_VAL))
		return -1;

	pthread_mutex_lock(&bb_state.bb_mutex);
	if (bb_state.bb_config.debug_flag) {
		info("%s: %s: %s test_only:%d",
		     plugin_type, __func__,
		     jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)),
		     (int) test_only);
	}
	if ((bb_job = _get_bb_job(job_ptr)) == NULL) {
		rc = -1;
	} else if (bb_job->state < BB_STATE_STAGING_IN) {
		/* Job buffer not allocated, create now if space available */
		rc = -1;
		if ((test_only == false) &&
		    (_test_size_limit(job_ptr, bb_job) == 0) &&
		    (_alloc_job_bb(job_ptr, bb_job, false) == SLURM_SUCCESS)) {
			if (bb_job->total_size == 0)
				rc = 1;	/* Persistent only, space available */
			else
				rc = 0;	/* Stage-in job buffer now */
		}
	} else if (bb_job->state == BB_STATE_STAGING_IN) {
		rc = 0;
	} else if (bb_job->state >= BB_STATE_STAGED_IN) {
		rc = 1;
	} else {
		error("%s: Unexpected burst buffer state (%d) for job %u",
		      __func__, bb_job->state, job_ptr->job_id);
		rc = -1;
	}

	pthread_mutex_unlock(&bb_state.bb_mutex);

	return rc;
}

/* Attempt to claim burst buffer resources.
 * At this time, bb_g_job_test_stage_in() should have been run sucessfully AND
 * the compute nodes selected for the job.
 *
 * Returns a SLURM errno.
 */
extern int bb_p_job_begin(struct job_record *job_ptr)
{
	char  *client_nodes_file_nid = NULL;
	pre_run_args_t *pre_run_args;
	char **pre_run_argv = NULL;
	char *job_dir = NULL;
	int hash_inx, rc = SLURM_SUCCESS;
	bb_job_t *bb_job;
	char jobid_buf[64];
	pthread_attr_t pre_run_attr;
	pthread_t pre_run_tid = 0;

	if ((job_ptr->burst_buffer == NULL) ||
	    (job_ptr->burst_buffer[0] == '\0'))
		return SLURM_SUCCESS;

	if (!job_ptr->job_resrcs || !job_ptr->job_resrcs->nodes) {
		error("%s: %s lacks node allocation", __func__,
		      jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
		return SLURM_ERROR;
	}

	pthread_mutex_lock(&bb_state.bb_mutex);
	if (bb_state.bb_config.debug_flag) {
		info("%s: %s: %s",
		     plugin_type, __func__,
		     jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
	}
	bb_job = _get_bb_job(job_ptr);
	if (!bb_job) {
		error("%s: %s: no job record buffer for job %u",
		      plugin_type, __func__, job_ptr->job_id);
		xfree(job_ptr->state_desc);
		job_ptr->state_desc =
			xstrdup("Could not find burst buffer record");
		job_ptr->state_reason = FAIL_BURST_BUFFER_OP;
		_queue_teardown(job_ptr->job_id, job_ptr->user_id, true);
		pthread_mutex_unlock(&bb_state.bb_mutex);
		return SLURM_ERROR;
	}

	/* Confirm that persistent burst buffers work has been completed */
	if ((_create_bufs(job_ptr, bb_job, true) > 0)) {
		xfree(job_ptr->state_desc);
		job_ptr->state_desc =
			xstrdup("Error managing persistent burst buffers");
		job_ptr->state_reason = FAIL_BURST_BUFFER_OP;
		_queue_teardown(job_ptr->job_id, job_ptr->user_id, true);
		pthread_mutex_unlock(&bb_state.bb_mutex);
		return SLURM_ERROR;
	}

	hash_inx = job_ptr->job_id % 10;
	xstrfmtcat(job_dir, "%s/hash.%d/job.%u", state_save_loc, hash_inx,
		   job_ptr->job_id);
	xstrfmtcat(client_nodes_file_nid, "%s/client_nids", job_dir);
	bb_job->state = BB_STATE_RUNNING;
	pthread_mutex_unlock(&bb_state.bb_mutex);

	if (_write_nid_file(client_nodes_file_nid, job_ptr->job_resrcs->nodes,
			    job_ptr->job_id)) {
		xfree(client_nodes_file_nid);
	}

	pre_run_argv = xmalloc(sizeof(char *) * 10);
	pre_run_argv[0] = xstrdup("dw_wlm_cli");
	pre_run_argv[1] = xstrdup("--function");
	pre_run_argv[2] = xstrdup("pre_run");
	pre_run_argv[3] = xstrdup("--token");
	xstrfmtcat(pre_run_argv[4], "%u", job_ptr->job_id);
	pre_run_argv[5] = xstrdup("--job");
	xstrfmtcat(pre_run_argv[6], "%s/script", job_dir);
	if (client_nodes_file_nid) {
#if defined(HAVE_NATIVE_CRAY)
		pre_run_argv[7] = xstrdup("--nidlistfile");
#else
		pre_run_argv[7] = xstrdup("--nodehostnamefile");
#endif
		pre_run_argv[8] = xstrdup(client_nodes_file_nid);
	}
	pre_run_args = xmalloc(sizeof(pre_run_args_t));
	pre_run_args->args    = pre_run_argv;
	pre_run_args->job_id  = job_ptr->job_id;
	pre_run_args->user_id = job_ptr->user_id;
//FIXME: Use prolog_running to delay launch
//	if (job_ptr->details)	/* Prevent launch until "pre_run" completes */
//		job_ptr->details->prolog_running++;

	slurm_attr_init(&pre_run_attr);
	if (pthread_attr_setdetachstate(&pre_run_attr, PTHREAD_CREATE_DETACHED))
		error("pthread_attr_setdetachstate error %m");
	while (pthread_create(&pre_run_tid, &pre_run_attr, _start_pre_run,
			      pre_run_args)) {
		if (errno != EAGAIN) {
			error("%s: pthread_create: %m", __func__);
			_start_pre_run(pre_run_argv);	/* Do in-line */
			break;
		}
		usleep(100000);
	}
	slurm_attr_destroy(&pre_run_attr);

	xfree(job_dir);
	xfree(client_nodes_file_nid);
	return rc;
}

static void *_start_pre_run(void *x)
{
	/* Locks: write job */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK };
	pre_run_args_t *pre_run_args = (pre_run_args_t *) x;
	char *resp_msg = NULL;
	char jobid_buf[64];
	bb_job_t *bb_job;
	int status = 0;
	struct job_record *job_ptr;
	DEF_TIMERS;

//FIXME: Move below after prolog_running use in place
lock_slurmctld(job_write_lock);
pthread_mutex_lock(&bb_state.bb_mutex);
	START_TIMER;
	resp_msg = bb_run_script("dws_pre_run",
				 bb_state.bb_config.get_sys_state,
				 pre_run_args->args, 2000, &status);
	END_TIMER;

//	lock_slurmctld(job_write_lock);
//	pthread_mutex_lock(&bb_state.bb_mutex);
	job_ptr = find_job_record(pre_run_args->job_id);
	if (job_ptr) {
		jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf));
	} else {
		error("%s: Could not find job record for job %u", __func__,
		      pre_run_args->job_id);
		snprintf(jobid_buf, sizeof(jobid_buf), "%u",
			 pre_run_args->job_id);
	}
	if (DELTA_TIMER > 500000) {	/* 0.5 secs */
		info("%s: dws_pre_run for %s ran for %s", __func__,
		     jobid_buf, TIME_STR);
	} else if (bb_state.bb_config.debug_flag) {
		debug("%s: dws_pre_run for %s ran for %s", __func__,
		      jobid_buf, TIME_STR);
	}
	_log_script_argv(pre_run_args->args, resp_msg);
//	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
if (0) { // FIXME: Cray API is always returning an exit code of 1
		time_t now = time(NULL);
		error("%s: dws_pre_run for %s status:%u response:%s", __func__,
		      jobid_buf, status, resp_msg);
		if (job_ptr) {
			xfree(job_ptr->state_desc);
			job_ptr->state_desc =
				xstrdup("Burst buffer pre_run error");
			job_ptr->state_reason = FAIL_BAD_CONSTRAINTS;
			last_job_update = now;
			bb_job = _get_bb_job(job_ptr);
			if (bb_job)
				bb_job->state = BB_STATE_TEARDOWN;
		}
		_queue_teardown(pre_run_args->job_id, pre_run_args->user_id,
				true);
	} else if (job_ptr && job_ptr->details &&
		   job_ptr->details->prolog_running) {
		job_ptr->details->prolog_running--;
	}
	pthread_mutex_unlock(&bb_state.bb_mutex);
	unlock_slurmctld(job_write_lock);

	xfree(resp_msg);
	_free_script_argv(pre_run_args->args);
	xfree(pre_run_args);
	return NULL;
}

/*
 * Trigger a job's burst buffer stage-out to begin
 *
 * Returns a SLURM errno.
 */
extern int bb_p_job_start_stage_out(struct job_record *job_ptr)
{
//FIXME: Test for memory leaks
	bb_job_t *bb_job;
	char jobid_buf[32];

	if ((job_ptr->burst_buffer == NULL) ||
	    (job_ptr->burst_buffer[0] == '\0'))
		return SLURM_SUCCESS;

	pthread_mutex_lock(&bb_state.bb_mutex);
	if (bb_state.bb_config.debug_flag) {
		info("%s: %s: %s", plugin_type, __func__,
		     jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
	}
	bb_job = _get_bb_job(job_ptr);
	if (!bb_job) {
		/* No job buffers. Assuming use of persistent buffers only */
		verbose("%s: %s bb job record not found", __func__,
			jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
	} else if (bb_job->total_size == 0) {
		bb_job->state = BB_STATE_TEARDOWN;
		_queue_teardown(job_ptr->job_id, job_ptr->user_id, false);
	} else if (bb_job->state < BB_STATE_STAGING_OUT) {
		bb_job->state = BB_STATE_STAGING_OUT;
		_queue_stage_out(job_ptr);
	}
	pthread_mutex_unlock(&bb_state.bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Determine if a job's burst buffer stage-out is complete
 *
 * RET: 0 - stage-out is underway
 *      1 - stage-out complete
 *     -1 - fatal error
 */
extern int bb_p_job_test_stage_out(struct job_record *job_ptr)
{
	bb_job_t *bb_job;
	int rc = -1;
	char jobid_buf[32];

	if ((job_ptr->burst_buffer == NULL) ||
	    (job_ptr->burst_buffer[0] == '\0'))
		return 1;

	pthread_mutex_lock(&bb_state.bb_mutex);
	if (bb_state.bb_config.debug_flag) {
		info("%s: %s: %s", plugin_type, __func__,
		     jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
	}
	bb_job = _get_bb_job(job_ptr);
	if (!bb_job) {
		/* No job buffers. Assuming use of persistent buffers only */
		verbose("%s: %s bb job record not found", __func__,
			jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
		rc =  1;
	} else {
		if (bb_job->state < BB_STATE_STAGING_OUT) {
			rc = -1;
		} else if (bb_job->state == BB_STATE_STAGING_OUT) {
			rc =  0;
		} else { /* bb_job->state > BB_STATE_STAGING_OUT) */
			rc =  1;
		}
	}
	pthread_mutex_unlock(&bb_state.bb_mutex);

	return rc;
}

/*
 * Terminate any file staging and completely release burst buffer resources
 *
 * Returns a SLURM errno.
 */
extern int bb_p_job_cancel(struct job_record *job_ptr)
{
	bb_job_t *bb_job;
	bb_alloc_t *bb_alloc;
	char jobid_buf[32];

	pthread_mutex_lock(&bb_state.bb_mutex);
	if (bb_state.bb_config.debug_flag) {
		info("%s: %s: %s", plugin_type, __func__,
		     jobid2fmt(job_ptr, jobid_buf, sizeof(jobid_buf)));
	}

	bb_job = _get_bb_job(job_ptr);
	if (!bb_job || (bb_job->state == BB_STATE_PENDING)) {
		/* Nothing to clean up */
	} else {
		/* Note: Persistent burst buffer actions already completed
		 * for the job are not reversed */
		bb_job->state = BB_STATE_TEARDOWN;
		bb_alloc = bb_find_alloc_rec(&bb_state, job_ptr);
		if (bb_alloc) {
			bb_alloc->state = BB_STATE_TEARDOWN;
			bb_alloc->state_time = time(NULL);
		}
		_queue_teardown(job_ptr->job_id, job_ptr->user_id, true);
	}
	pthread_mutex_unlock(&bb_state.bb_mutex);

	return SLURM_SUCCESS;
}

static void _free_create_args(create_buf_data_t *create_args)
{
	if (create_args) {
		xfree(create_args->access);
		xfree(create_args->job_script);
		xfree(create_args->name);
		xfree(create_args->type);
		xfree(create_args);
	}
}

/* Create/destroy persistent burst buffers
 * job_ptr IN - job to operate upon
 * bb_job IN - job's burst buffer data
 * job_ready IN - if true, job is ready to run now, if false then do not
 *                delete persistent buffers
 * Returns count of buffer create/destroy requests which are pending */
static int _create_bufs(struct job_record *job_ptr, bb_job_t *bb_job,
		        bool job_ready)
{
	pthread_attr_t create_attr;
	pthread_t create_tid = 0;
	create_buf_data_t *create_args;
	bb_buf_t *buf_ptr;
	bb_alloc_t *bb_alloc;
	int i, hash_inx, rc = 0;

	xassert(bb_job);
	for (i = 0, buf_ptr = bb_job->buf_ptr; i < bb_job->buf_cnt;
	     i++, buf_ptr++) {
		if ((buf_ptr->state == BB_STATE_ALLOCATING) ||
		    (buf_ptr->state == BB_STATE_DELETING)) {
			rc++;
		} else if (buf_ptr->state != BB_STATE_PENDING) {
			;	/* Nothing to do */
		} else if (!buf_ptr->destroy) {	/* Create the buffer */
			rc++;
			bb_limit_add(job_ptr->user_id, bb_job->account,
				     bb_job->partition, bb_job->qos,
				     buf_ptr->size, &bb_state);
			bb_job->state = BB_STATE_ALLOCATING;
			buf_ptr->state = BB_STATE_ALLOCATING;
			create_args = xmalloc(sizeof(create_buf_data_t));
			create_args->access = xstrdup(buf_ptr->access);
			create_args->job_id = job_ptr->job_id;
			create_args->name = xstrdup(buf_ptr->name);
			create_args->size = buf_ptr->size;
			create_args->type = xstrdup(buf_ptr->type);
			create_args->user_id = job_ptr->user_id;
			slurm_attr_init(&create_attr);
			if (pthread_attr_setdetachstate(&create_attr,
							PTHREAD_CREATE_DETACHED))
				error("pthread_attr_setdetachstate error %m");
			while (pthread_create(&create_tid, &create_attr,
					      _create_persistent,
					      create_args)) {
				if (errno != EAGAIN) {
					error("%s: pthread_create: %m",
					      __func__);
					_create_persistent(create_args);
					break;
				}
				usleep(100000);
			}
			slurm_attr_destroy(&create_attr);
		} else if (buf_ptr->destroy && job_ready) { /* Delete the buffer */
			bb_alloc = bb_find_name_rec(buf_ptr->name,
						    job_ptr->user_id,
						    &bb_state);
			if (!bb_alloc) {
				/* Ignore request if named buffer not found */
				info(
"%s: destroy_persistent: No burst buffer with name '%s' found for job %u",
				     plugin_type, buf_ptr->name,
				     job_ptr->job_id);
				continue;
			}
			rc++;
			if ((bb_alloc->user_id != job_ptr->user_id) &&
			    !validate_super_user(job_ptr->user_id)) {
				info(
"%s: destroy_persistent: Attempt by user %u job %u to destroy buffer %s owned by user %u",
				     plugin_type, job_ptr->user_id,
				     job_ptr->job_id, buf_ptr->name,
				     bb_alloc->user_id);
				job_ptr->state_reason = FAIL_BURST_BUFFER_OP;
				xstrfmtcat(job_ptr->state_desc,
					   "%s: Delete buffer %s permission denied",
					   plugin_type, buf_ptr->name);
				job_ptr->priority = 0;  /* Hold job */
				continue;
			}

			bb_job->state = BB_STATE_DELETING;
			buf_ptr->state = BB_STATE_DELETING;
			create_args = xmalloc(sizeof(create_buf_data_t));
			create_args->hurry = buf_ptr->hurry;
			create_args->job_id = job_ptr->job_id;
			hash_inx = job_ptr->job_id % 10;
			xstrfmtcat(create_args->job_script,
				   "%s/hash.%d/job.%u/script",
				   state_save_loc, hash_inx, job_ptr->job_id);
			create_args->name = xstrdup(buf_ptr->name);
			create_args->user_id = job_ptr->user_id;
			slurm_attr_init(&create_attr);
			if (pthread_attr_setdetachstate(&create_attr,
							PTHREAD_CREATE_DETACHED))
				error("pthread_attr_setdetachstate error %m");
			while (pthread_create(&create_tid, &create_attr,
					      _destroy_persistent,
					      create_args)) {
				if (errno != EAGAIN) {
					error("%s: pthread_create: %m",
					      __func__);
					_destroy_persistent(create_args);
					break;
				}
				usleep(100000);
			}
			slurm_attr_destroy(&create_attr);
		} else if (buf_ptr->destroy) {
			rc++;
		}
	}

	return rc;
}

static void _reset_buf_state(uint32_t user_id, uint32_t job_id, char *name,
			     int new_state)
{
	bb_buf_t *buf_ptr;
	bb_job_t *bb_job;
	int i, old_state;
	bool active_buf;

	bb_job = bb_job_find(&bb_state, job_id);
	if (!bb_job) {
		error("%s: Could not find job record for %u", __func__, job_id);
		return;
	}

	/* Update the buffer's state in job record */
	for (i = 0, buf_ptr = bb_job->buf_ptr; i < bb_job->buf_cnt;
	     i++, buf_ptr++) {
		if (strcmp(name, buf_ptr->name))
			continue;
		old_state = buf_ptr->state;
		buf_ptr->state = new_state;
		if ((old_state == BB_STATE_ALLOCATING) &&
		    (new_state == BB_STATE_PENDING))
			bb_limit_rem(user_id, bb_job->account,
				      bb_job->partition, bb_job->qos,
				      buf_ptr->size, &bb_state);
		if ((old_state == BB_STATE_DELETING) &&
		    (new_state == BB_STATE_PENDING))
			bb_limit_rem(user_id, bb_job->account,
				     bb_job->partition, bb_job->qos,
				     buf_ptr->size, &bb_state);
		break;
	}

	for (i = 0, buf_ptr = bb_job->buf_ptr; i < bb_job->buf_cnt;
	     i++, buf_ptr++) {
		old_state = buf_ptr->state;
		if ((old_state == BB_STATE_PENDING)    ||
		    (old_state == BB_STATE_ALLOCATING) ||
		    (old_state == BB_STATE_DELETING)   ||
		    (old_state == BB_STATE_TEARDOWN))
			active_buf = true;
		break;
	}
	if (!active_buf) {
		if (bb_job->state == BB_STATE_ALLOCATING)
			bb_job->state = BB_STATE_ALLOCATED;
		else if (bb_job->state == BB_STATE_DELETING)
			bb_job->state = BB_STATE_DELETED;
	}
}

/* Create a persistent burst buffer based upon user specifications. */
static void *_create_persistent(void *x)
{
	slurmctld_lock_t job_write_lock =
		{ NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK };
	create_buf_data_t *create_args = (create_buf_data_t *) x;
	struct job_record *job_ptr;
	bb_alloc_t *bb_alloc;
	char **script_argv, *resp_msg;
	int i, status = 0;
	slurmdb_assoc_rec_t *assoc;
	DEF_TIMERS;

	script_argv = xmalloc(sizeof(char *) * 20);	/* NULL terminated */
	script_argv[0] = xstrdup("dw_wlm_cli");
	script_argv[1] = xstrdup("--function");
	script_argv[2] = xstrdup("create_persistent");
	script_argv[3] = xstrdup("-c");
	script_argv[4] = xstrdup("CLI");
	script_argv[5] = xstrdup("-t");		/* name */
	script_argv[6] = xstrdup(create_args->name);
	script_argv[7] = xstrdup("-u");		/* user iD */
	xstrfmtcat(script_argv[8], "%u", create_args->user_id);
	script_argv[9] = xstrdup("-C");		/* configuration */
	pthread_mutex_lock(&bb_state.bb_mutex);
	xstrfmtcat(script_argv[10], "%s:%"PRIu64"",
		   bb_state.bb_config.default_pool, create_args->size);
	pthread_mutex_unlock(&bb_state.bb_mutex);
	i = 11;
	if (create_args->access) {
		script_argv[i++] = xstrdup("-a");
		script_argv[i++] = xstrdup(create_args->access);
	}
	if (create_args->type) {
		script_argv[i++] = xstrdup("-T");
		script_argv[i++] = xstrdup(create_args->type);
	}
	/* NOTE: There is an optional group ID parameter available and
	 * currently not used by Slurm */

	START_TIMER;
	resp_msg = bb_run_script("create_persistent",
				 bb_state.bb_config.get_sys_state,
				 script_argv, 3000, &status);
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
	END_TIMER;
	if (bb_state.bb_config.debug_flag)
		debug("%s: ran for %s", __func__, TIME_STR);
//	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
if (0) { //FIXME: Cray bug: API exit code NOT 0 on success as documented
		error("%s: For JobID=%u Name=%s status:%u response:%s",
		      __func__, create_args->job_id, create_args->name,
		      status, resp_msg);
		lock_slurmctld(job_write_lock);
		job_ptr = find_job_record(create_args->job_id);
		if (!job_ptr) {
			error("%s: unable to find job record for job %u",
			      __func__, create_args->job_id);
		} else {
			job_ptr->state_reason = FAIL_BAD_CONSTRAINTS;
			job_ptr->priority = 0;
			xfree(job_ptr->state_desc);
			xstrfmtcat(job_ptr->state_desc, "%s: %s: %s",
				   plugin_type, __func__, resp_msg);
			resp_msg = NULL;
		}
		_reset_buf_state(create_args->user_id,
				 create_args->job_id,
				 create_args->name, BB_STATE_PENDING);
		unlock_slurmctld(job_write_lock);
	} else if (resp_msg && strstr(resp_msg, "created")) {
		lock_slurmctld(job_write_lock);
		job_ptr = find_job_record(create_args->job_id);
		if (!job_ptr) {
			error("%s: unable to find job record for job %u",
			      __func__, create_args->job_id);
		}
		pthread_mutex_lock(&bb_state.bb_mutex);
		_reset_buf_state(create_args->user_id,
				 create_args->job_id, create_args->name,
				 BB_STATE_ALLOCATED);
		bb_alloc = bb_alloc_name_rec(&bb_state, create_args->name,
					     create_args->user_id);
		if (bb_state.bb_config.flags & BB_FLAG_EMULATE_CRAY)
			bb_alloc->create_time = time(NULL);
		bb_alloc->size = create_args->size;
		if (job_ptr) {
			bb_alloc->account   = xstrdup(job_ptr->account);
			bb_alloc->assoc_ptr = job_ptr->assoc_ptr;
			assoc = job_ptr->assoc_ptr;
			while (assoc) {
				xstrfmtcat(bb_alloc->assocs, ",%u",
					   assoc->id);
				assoc = assoc->usage->parent_assoc_ptr;
			}
			if (bb_alloc->assocs)
				xstrcat(bb_alloc->assocs, ",");
			if (job_ptr->part_ptr) {
				bb_alloc->partition =
					xstrdup(job_ptr->part_ptr->name);
			}
			if (job_ptr->qos_ptr) {
				slurmdb_qos_rec_t *qos_ptr =
					(slurmdb_qos_rec_t *)job_ptr->qos_ptr;
				bb_alloc->qos = xstrdup(qos_ptr->name);
			}
		}
		pthread_mutex_unlock(&bb_state.bb_mutex);
		unlock_slurmctld(job_write_lock);
	}
	xfree(resp_msg);
	_free_create_args(create_args);
	return NULL;
}

/* Destroy a persistent burst buffer */
static void *_destroy_persistent(void *x)
{
	slurmctld_lock_t job_write_lock =
		{ NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK };
	create_buf_data_t *destroy_args = (create_buf_data_t *) x;
	struct job_record *job_ptr;
	bb_alloc_t *bb_alloc;
	char **script_argv, *resp_msg;
	int status = 0;
	DEF_TIMERS;

	pthread_mutex_lock(&bb_state.bb_mutex);
	bb_alloc = bb_find_name_rec(destroy_args->name, destroy_args->user_id,
				    &bb_state);
	if (!bb_alloc) {
		info("%s: destroy_persistent: No burst buffer with name '%s' found for job %u",
		     plugin_type, destroy_args->name, destroy_args->job_id);
	}
	pthread_mutex_unlock(&bb_state.bb_mutex);

	script_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	script_argv[0] = xstrdup("dw_wlm_cli");
	script_argv[1] = xstrdup("--function");
	script_argv[2] = xstrdup("teardown");
	script_argv[3] = xstrdup("--token");	/* name */
	script_argv[4] = xstrdup(destroy_args->name);
	script_argv[5] = xstrdup("--job");	/* script */
	script_argv[6] = xstrdup(destroy_args->job_script);
	if (destroy_args->hurry)
		script_argv[7] = xstrdup("--hurry");

	resp_msg = bb_run_script("destroy_persistent",
				 bb_state.bb_config.get_sys_state,
				 script_argv, 3000, &status);
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
	END_TIMER;
	if (bb_state.bb_config.debug_flag)
		debug("%s: destroy_persistent ran for %s", __func__, TIME_STR);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: destroy_persistent for JobID=%u Name=%s status:%u response:%s",
		      __func__, destroy_args->job_id, destroy_args->name,
		      status, resp_msg);
		lock_slurmctld(job_write_lock);
		job_ptr = find_job_record(destroy_args->job_id);
		if (!job_ptr) {
			error("%s: unable to find job record for job %u",
			      __func__, destroy_args->job_id);
		} else {
			job_ptr->state_reason = FAIL_BAD_CONSTRAINTS;
			xfree(job_ptr->state_desc);
			job_ptr->state_desc = resp_msg;
			resp_msg = NULL;
			xstrfmtcat(job_ptr->state_desc, "%s: %s: %s",
				   plugin_type, __func__, resp_msg);
		}
		pthread_mutex_lock(&bb_state.bb_mutex);
		_reset_buf_state(destroy_args->user_id,
				 destroy_args->job_id, destroy_args->name,
				 BB_STATE_PENDING);
		pthread_mutex_unlock(&bb_state.bb_mutex);
		unlock_slurmctld(job_write_lock);
	} else {
		pthread_mutex_lock(&bb_state.bb_mutex);
		_reset_buf_state(destroy_args->user_id,
				 destroy_args->job_id, destroy_args->name,
				 BB_STATE_DELETED);

		/* Modify internal buffer record for purging */
		bb_alloc->state = BB_STATE_COMPLETE;
		bb_alloc->job_id = destroy_args->job_id;
		bb_alloc->state_time = time(NULL);
		bb_limit_rem(bb_alloc->user_id, bb_alloc->account,
			     bb_alloc->partition, bb_alloc->qos,
			     bb_alloc->size, &bb_state);
		(void) bb_free_alloc_rec(&bb_state, bb_alloc);
		unlock_slurmctld(job_write_lock);
	}
	xfree(resp_msg);
	_free_create_args(destroy_args);
	return NULL;
}

/* _bb_get_instances()
 *
 * Handle the JSON stream with configuration info (instance use details).
 */
static bb_configs_t *
_bb_get_configs(int *num_ent, bb_state_t *state_ptr)
{
	bb_configs_t *ents = NULL;
	json_object *j;
	json_object_iter iter;
	int status = 0;
	DEF_TIMERS;
	char *resp_msg;
	char **script_argv;

	script_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	script_argv[0] = xstrdup("dw_wlm_cli");
	script_argv[1] = xstrdup("--function");
	script_argv[2] = xstrdup("show_configurations");

	START_TIMER;
	resp_msg = bb_run_script("show_configurations",
				 state_ptr->bb_config.get_sys_state,
				 script_argv, 3000, &status);
	END_TIMER;
	if (bb_state.bb_config.debug_flag)
		debug("%s: show_configurations ran for %s", __func__, TIME_STR);
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
//FIXME: Cray API returning error if no configurations
//	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
if (0) {
		error("%s: show_configurations status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: %s returned no configurations",
		     __func__, state_ptr->bb_config.get_sys_state);
		return ents;
	}


	_python2json(resp_msg);
	j = json_tokener_parse(resp_msg);
	if (j == NULL) {
		error("%s: json parser failed on %s", __func__, resp_msg);
		xfree(resp_msg);
		return ents;
	}
	xfree(resp_msg);

	json_object_object_foreachC(j, iter) {
		ents = _json_parse_configs_array(j, iter.key, num_ent);
	}
	json_object_put(j);	/* Frees json memory */

	return ents;
}

/* _bb_get_instances()
 *
 * Handle the JSON stream with instance info (resource reservations).
 */
static bb_instances_t *
_bb_get_instances(int *num_ent, bb_state_t *state_ptr)
{
	bb_instances_t *ents = NULL;
	json_object *j;
	json_object_iter iter;
	int status = 0;
	DEF_TIMERS;
	char *resp_msg;
	char **script_argv;

	script_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	script_argv[0] = xstrdup("dw_wlm_cli");
	script_argv[1] = xstrdup("--function");
	script_argv[2] = xstrdup("show_instances");

	START_TIMER;
	resp_msg = bb_run_script("show_instances",
				 state_ptr->bb_config.get_sys_state,
				 script_argv, 3000, &status);
	END_TIMER;
	if (bb_state.bb_config.debug_flag)
		debug("%s: show_instances ran for %s", __func__, TIME_STR);
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
//FIXME: Cray API returning error if no instances
//	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
if (0) {
		error("%s: show_instances status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: %s returned no instances",
		     __func__, state_ptr->bb_config.get_sys_state);
		return ents;
	}

	_python2json(resp_msg);
	j = json_tokener_parse(resp_msg);
	if (j == NULL) {
		error("%s: json parser failed on %s", __func__, resp_msg);
		xfree(resp_msg);
		return ents;
	}
	xfree(resp_msg);

	json_object_object_foreachC(j, iter) {
		ents = _json_parse_instances_array(j, iter.key, num_ent);
	}
	json_object_put(j);	/* Frees json memory */

	return ents;
}

/* _bb_get_pools()
 *
 * Handle the JSON stream with resource pool info (available resource type).
 */
static bb_pools_t *
_bb_get_pools(int *num_ent, bb_state_t *state_ptr)
{
	bb_pools_t *ents = NULL;
	json_object *j;
	json_object_iter iter;
	int status = 0;
	DEF_TIMERS;
	char *resp_msg;
	char **script_argv;

	script_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	script_argv[0] = xstrdup("dw_wlm_cli");
	script_argv[1] = xstrdup("--function");
	script_argv[2] = xstrdup("pools");

	START_TIMER;
	resp_msg = bb_run_script("pools",
				 state_ptr->bb_config.get_sys_state,
				 script_argv, 3000, &status);
	END_TIMER;
	if (bb_state.bb_config.debug_flag) {
		/* Only log pools data if different to limit volume of logs */
		static uint32_t last_csum = 0;
		uint32_t i, resp_csum = 0;
		debug("%s: pools ran for %s", __func__, TIME_STR);
		for (i = 0; resp_msg[i]; i++)
			resp_csum += ((i * resp_msg[i]) % 1000000);
		if (last_csum != resp_csum)
			_log_script_argv(script_argv, resp_msg);
		last_csum = resp_csum;
	}
	_free_script_argv(script_argv);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: pools status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		error("%s: %s returned no pools",
		      __func__, state_ptr->bb_config.get_sys_state);
		return ents;
	}

	_python2json(resp_msg);
	j = json_tokener_parse(resp_msg);
	if (j == NULL) {
		error("%s: json parser failed on %s", __func__, resp_msg);
		xfree(resp_msg);
		return ents;
	}
	xfree(resp_msg);

	json_object_object_foreachC(j, iter) {
		ents = _json_parse_pools_array(j, iter.key, num_ent);
	}
	json_object_put(j);	/* Frees json memory */

	return ents;
}

static bb_sessions_t *
_bb_get_sessions(int *num_ent, bb_state_t *state_ptr)
{
	bb_sessions_t *ents = NULL;
	json_object *j;
	json_object_iter iter;
	int status = 0;
	DEF_TIMERS;
	char *resp_msg;
	char **script_argv;

	script_argv = xmalloc(sizeof(char *) * 10);	/* NULL terminated */
	script_argv[0] = xstrdup("dw_wlm_cli");
	script_argv[1] = xstrdup("--function");
	script_argv[2] = xstrdup("show_sessions");

	START_TIMER;
	resp_msg = bb_run_script("show_sessions",
				 state_ptr->bb_config.get_sys_state,
				 script_argv, 3000, &status);
	END_TIMER;
	if (bb_state.bb_config.debug_flag)
		debug("%s: show_sessions ran for %s", __func__, TIME_STR);
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
//FIXME: Cray API returning error if no sessions
//	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
if (0) {
		error("%s: show_sessions status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: %s returned no sessions",
		     __func__, state_ptr->bb_config.get_sys_state);
		_free_script_argv(script_argv);
		return ents;
	}

	_python2json(resp_msg);
	j = json_tokener_parse(resp_msg);
	if (j == NULL) {
		error("%s: json parser failed on %s", __func__, resp_msg);
		xfree(resp_msg);
		return ents;
	}
	xfree(resp_msg);

	json_object_object_foreachC(j, iter) {
		ents = _json_parse_sessions_array(j, iter.key, num_ent);
	}
	json_object_put(j);	/* Frees json memory */

	return ents;
}

/* _bb_free_configs()
 */
static void
_bb_free_configs(bb_configs_t *ents, int num_ent)
{
	xfree(ents);
}

/* _bb_free_instances()
 */
static void
_bb_free_instances(bb_instances_t *ents, int num_ent)
{
	int i;

	for (i = 0; i < num_ent; i++) {
		xfree(ents[i].label);
	}

	xfree(ents);
}

/* _bb_free_pools()
 */
static void
_bb_free_pools(bb_pools_t *ents, int num_ent)
{
	int i;

	for (i = 0; i < num_ent; i++) {
		xfree(ents[i].id);
		xfree(ents[i].units);
	}

	xfree(ents);
}

/* _bb_free_sessions()
 */
static void
_bb_free_sessions(bb_sessions_t *ents, int num_ent)
{
	int i;

	for (i = 0; i < num_ent; i++) {
		xfree(ents[i].token);
	}

	xfree(ents);
}

/* _json_parse_configs_array()
 */
static bb_configs_t *
_json_parse_configs_array(json_object *jobj, char *key, int *num)
{
	json_object *jarray;
	int i;
	json_object *jvalue;
	bb_configs_t *ents;

	jarray = jobj;
	json_object_object_get_ex(jobj, key, &jarray);

	*num = json_object_array_length(jarray);
	ents = xmalloc(*num * sizeof(bb_configs_t));

	for (i = 0; i < *num; i++) {
		jvalue = json_object_array_get_idx(jarray, i);
		_json_parse_configs_object(jvalue, &ents[i]);
	}

	return ents;
}

/* _json_parse_instances_array()
 */
static bb_instances_t *
_json_parse_instances_array(json_object *jobj, char *key, int *num)
{
	json_object *jarray;
	int i;
	json_object *jvalue;
	bb_instances_t *ents;

	jarray = jobj;
	json_object_object_get_ex(jobj, key, &jarray);

	*num = json_object_array_length(jarray);
	ents = xmalloc(*num * sizeof(bb_instances_t));

	for (i = 0; i < *num; i++) {
		jvalue = json_object_array_get_idx(jarray, i);
		_json_parse_instances_object(jvalue, &ents[i]);
	}

	return ents;
}

/* _json_parse_pools_array()
 */
static bb_pools_t *
_json_parse_pools_array(json_object *jobj, char *key, int *num)
{
	json_object *jarray;
	int i;
	json_object *jvalue;
	bb_pools_t *ents;

	jarray = jobj;
	json_object_object_get_ex(jobj, key, &jarray);

	*num = json_object_array_length(jarray);
	ents = xmalloc(*num * sizeof(bb_pools_t));

	for (i = 0; i < *num; i++) {
		jvalue = json_object_array_get_idx(jarray, i);
		_json_parse_pools_object(jvalue, &ents[i]);
	}

	return ents;
}

/* _json_parse_sessions_array()
 */
static bb_sessions_t *
_json_parse_sessions_array(json_object *jobj, char *key, int *num)
{
	json_object *jarray;
	int i;
	json_object *jvalue;
	bb_sessions_t *ents;

	jarray = jobj;
	json_object_object_get_ex(jobj, key, &jarray);

	*num = json_object_array_length(jarray);
	ents = xmalloc(*num * sizeof(bb_pools_t));

	for (i = 0; i < *num; i++) {
		jvalue = json_object_array_get_idx(jarray, i);
		_json_parse_sessions_object(jvalue, &ents[i]);
	}

	return ents;
}

/* Parse "links" object in the "configuration" object */
static void
_parse_config_links(json_object *instance, bb_configs_t *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int x;

	json_object_object_foreachC(instance, iter) {
		type = json_object_get_type(iter.val);
		switch (type) {
			case json_type_int:
				x = json_object_get_int64(iter.val);
				if (!strcmp(iter.key, "instance"))
					ent->instance = x;
				break;
			default:
				break;
		}
	}
}

/* _json_parse_configs_object()
 */
static void
_json_parse_configs_object(json_object *jobj, bb_configs_t *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int64_t x;

	json_object_object_foreachC(jobj, iter) {
		type = json_object_get_type(iter.val);
		switch (type) {
			case json_type_object:
				if (strcmp(iter.key, "links") == 0)
					_parse_config_links(iter.val, ent);
				break;
			case json_type_int:
				x = json_object_get_int64(iter.val);
				if (strcmp(iter.key, "id") == 0) {
					ent->id = x;
				}
				break;
			default:
				break;
		}
	}
}

/* Parse "capacity" object in the "instance" object */
static void
_parse_instance_capacity(json_object *instance, bb_instances_t *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int x;

	json_object_object_foreachC(instance, iter) {
		type = json_object_get_type(iter.val);
		switch (type) {
			case json_type_int:
				x = json_object_get_int64(iter.val);
				if (!strcmp(iter.key, "bytes"))
					ent->bytes = x;
				break;
			default:
				break;
		}
	}
}

/* _json_parse_instances_object()
 */
static void
_json_parse_instances_object(json_object *jobj, bb_instances_t *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int64_t x;
	const char *p;

	json_object_object_foreachC(jobj, iter) {
		type = json_object_get_type(iter.val);
		switch (type) {
			case json_type_object:
				if (strcmp(iter.key, "capacity") == 0)
					_parse_instance_capacity(iter.val, ent);
				break;
			case json_type_int:
				x = json_object_get_int64(iter.val);
				if (strcmp(iter.key, "id") == 0) {
					ent->id = x;
				}
				break;
			case json_type_string:
				p = json_object_get_string(iter.val);
				if (strcmp(iter.key, "label") == 0) {
					ent->label = xstrdup(p);
				}
				break;
			default:
				break;
		}
	}
}

/* _json_parse_pools_object()
 */
static void
_json_parse_pools_object(json_object *jobj, bb_pools_t *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int64_t x;
	const char *p;

	json_object_object_foreachC(jobj, iter) {
		type = json_object_get_type(iter.val);
		switch (type) {
			case json_type_int:
				x = json_object_get_int64(iter.val);
				if (strcmp(iter.key, "granularity") == 0) {
					ent->granularity = x;
				} else if (strcmp(iter.key, "quantity") == 0) {
					ent->quantity = x;
				} else if (strcmp(iter.key, "free") == 0) {
					ent->free = x;
				}
				break;
			case json_type_string:
				p = json_object_get_string(iter.val);
				if (strcmp(iter.key, "id") == 0) {
					ent->id = xstrdup(p);
				} else if (strcmp(iter.key, "units") == 0) {
					ent->units = xstrdup(p);
				}
				break;
			default:
				break;
		}
	}
}

/* _json_parse_session_object()
 */
static void
_json_parse_sessions_object(json_object *jobj, bb_sessions_t *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int64_t x;
	const char *p;

	json_object_object_foreachC(jobj, iter) {
		type = json_object_get_type(iter.val);
		switch (type) {
			case json_type_int:
				x = json_object_get_int64(iter.val);
				if (strcmp(iter.key, "id") == 0) {
					ent->id = x;
				} else if (strcmp(iter.key, "owner") == 0) {
					ent->user_id = x;
				}
				break;
			case json_type_string:
				p = json_object_get_string(iter.val);
				if (strcmp(iter.key, "token") == 0) {
					ent->token = xstrdup(p);
				}
			default:
				break;
		}
	}
}

/*
 * Translate a burst buffer string to it's equivalent TRES string
 * Caller must xfree the return value
 */
extern char *bb_p_xlate_bb_2_tres_str(char *burst_buffer)
{
	char *save_ptr = NULL, *sep, *tmp, *tok;
	char *result = NULL;
	uint64_t size, total = 0;

	if (!burst_buffer || (bb_state.tres_pos < 1))
		return result;

	tmp = xstrdup(burst_buffer);
	tok = strtok_r(tmp, ",", &save_ptr);
	while (tok) {
		sep = strchr(tok, ':');
		if (sep) {
			if (!strncmp(tok, "cray:", 5))
				tok += 5;
			else
				tok = NULL;
		}

		if (tok) {
			uint64_t mb_xlate = 1024 * 1024;
			size = bb_get_size_num(tok,
					       bb_state.bb_config.granularity);
			total += (size + mb_xlate - 1) / mb_xlate;
		}

		tok = strtok_r(NULL, ",", &save_ptr);
	}

	if (total)
		xstrfmtcat(result, "%d=%"PRIu64, bb_state.tres_pos, total);

	return result;
}
