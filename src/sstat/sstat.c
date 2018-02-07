/*****************************************************************************\
 *  sstat.c - job accounting reports for SLURM's slurmdb/log plugin
 *****************************************************************************
 *  Copyright (C) 2008 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
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

#include "config.h"

#include "sstat.h"

int _do_stat(uint32_t jobid, uint32_t stepid, char *nodelist,
	     uint32_t req_cpufreq_min, uint32_t req_cpufreq_max,
	     uint32_t req_cpufreq_gov,
	     uint16_t use_protocol_ver);

/*
 * Globals
 */
sstat_parameters_t params;
print_field_t fields[] = {
	{10, "AveCPU", print_fields_str, PRINT_AVECPU},
	{10, "AveCPUFreq", print_fields_str, PRINT_ACT_CPUFREQ},
	{12, "AveDiskRead", print_fields_str, PRINT_AVEDISKREAD},
	{12, "AveDiskWrite", print_fields_str, PRINT_AVEDISKWRITE},
	{10, "AvePages", print_fields_str, PRINT_AVEPAGES},
	{10, "AveRSS", print_fields_str, PRINT_AVERSS},
	{10, "AveVMSize", print_fields_str, PRINT_AVEVSIZE},
	{14, "ConsumedEnergy", print_fields_str, PRINT_CONSUMED_ENERGY},
	{17, "ConsumedEnergyRaw", print_fields_uint64,
	 PRINT_CONSUMED_ENERGY_RAW},
	{-12, "JobID", print_fields_str, PRINT_JOBID},
	{12, "MaxDiskRead", print_fields_str, PRINT_MAXDISKREAD},
	{15, "MaxDiskReadNode", print_fields_str, PRINT_MAXDISKREADNODE},
	{15, "MaxDiskReadTask", print_fields_uint, PRINT_MAXDISKREADTASK},
	{12, "MaxDiskWrite", print_fields_str, PRINT_MAXDISKWRITE},
	{16, "MaxDiskWriteNode", print_fields_str, PRINT_MAXDISKWRITENODE},
	{16, "MaxDiskWriteTask", print_fields_uint, PRINT_MAXDISKWRITETASK},
	{8, "MaxPages", print_fields_str, PRINT_MAXPAGES},
	{12, "MaxPagesNode", print_fields_str, PRINT_MAXPAGESNODE},
	{14, "MaxPagesTask", print_fields_uint, PRINT_MAXPAGESTASK},
	{10, "MaxRSS", print_fields_str, PRINT_MAXRSS},
	{10, "MaxRSSNode", print_fields_str, PRINT_MAXRSSNODE},
	{10, "MaxRSSTask", print_fields_uint, PRINT_MAXRSSTASK},
	{10, "MaxVMSize", print_fields_str, PRINT_MAXVSIZE},
	{14, "MaxVMSizeNode", print_fields_str, PRINT_MAXVSIZENODE},
	{14, "MaxVMSizeTask", print_fields_uint, PRINT_MAXVSIZETASK},
	{10, "MinCPU", print_fields_str, PRINT_MINCPU},
	{10, "MinCPUNode", print_fields_str, PRINT_MINCPUNODE},
	{10, "MinCPUTask", print_fields_uint, PRINT_MINCPUTASK},
	{20, "Nodelist", print_fields_str, PRINT_NODELIST},
	{8, "NTasks", print_fields_uint, PRINT_NTASKS},
	{20, "Pids", print_fields_str, PRINT_PIDS},
	{10, "ReqCPUFreq", print_fields_str, PRINT_REQ_CPUFREQ_MIN}, /*vestigial*/
	{13, "ReqCPUFreqMin", print_fields_str, PRINT_REQ_CPUFREQ_MIN},
	{13, "ReqCPUFreqMax", print_fields_str, PRINT_REQ_CPUFREQ_MAX},
	{13, "ReqCPUFreqGov", print_fields_str, PRINT_REQ_CPUFREQ_GOV},
	{14, "TRESUsageInAve", print_fields_str, PRINT_TRESUIA},
	{14, "TRESUsageInMax", print_fields_str, PRINT_TRESUIM},
	{18, "TRESUsageInMaxNode", print_fields_str, PRINT_TRESUIMN},
	{18, "TRESUsageInMaxTask", print_fields_str, PRINT_TRESUIMT},
	{15, "TRESUsageOutAve", print_fields_str, PRINT_TRESUOA},
	{15, "TRESUsageOutMax", print_fields_str, PRINT_TRESUOM},
	{19, "TRESUsageOutMaxNode", print_fields_str, PRINT_TRESUOMN},
	{19, "TRESUsageOutMaxTask", print_fields_str, PRINT_TRESUOMT},
	{0, NULL, NULL, 0}};

List jobs = NULL;
slurmdb_job_rec_t job;
slurmdb_step_rec_t step;
List print_fields_list = NULL;
ListIterator print_fields_itr = NULL;
int field_count = 0;
List g_tres_list = NULL;
void *acct_db_conn = NULL;
bool db_access = true;

int _do_stat(uint32_t jobid, uint32_t stepid, char *nodelist,
	     uint32_t req_cpufreq_min, uint32_t req_cpufreq_max,
	     uint32_t req_cpufreq_gov, uint16_t use_protocol_ver)
{
	job_step_stat_response_msg_t *step_stat_response = NULL;
	int rc = SLURM_SUCCESS;
	ListIterator itr;
	slurmdb_stats_t temp_stats;
	job_step_stat_t *step_stat = NULL;
	int ntasks = 0;
	int tot_tasks = 0;
	hostlist_t hl = NULL;
	char *ave_usage_in = NULL;
	char *ave_usage_out = NULL;
	char *tmp_string = NULL;

	debug("requesting info for job %u.%u", jobid, stepid);
	if ((rc = slurm_job_step_stat(jobid, stepid, nodelist, use_protocol_ver,
				      &step_stat_response)) != SLURM_SUCCESS) {
		if (rc == ESLURM_INVALID_JOB_ID) {
			debug("job step %u.%u has already completed",
			      jobid, stepid);
		} else {
			error("problem getting step_layout for %u.%u: %s",
			      jobid, stepid, slurm_strerror(rc));
		}
		slurm_job_step_pids_response_msg_free(step_stat_response);
		return rc;
	}

	memset(&job, 0, sizeof(slurmdb_job_rec_t));
	job.jobid = jobid;

	memset(&step, 0, sizeof(slurmdb_step_rec_t));

	memset(&temp_stats, 0, sizeof(slurmdb_stats_t));
	temp_stats.cpu_min = NO_VAL;

	memset(&step.stats, 0, sizeof(slurmdb_stats_t));
	step.stats.cpu_min = NO_VAL;

	step.job_ptr = &job;
	step.stepid = stepid;
	step.nodes = xmalloc(BUF_SIZE);
	step.req_cpufreq_min = req_cpufreq_min;
	step.req_cpufreq_max = req_cpufreq_max;
	step.req_cpufreq_gov = req_cpufreq_gov;
	step.stepname = NULL;
	step.state = JOB_RUNNING;
	hl = hostlist_create(NULL);
	itr = list_iterator_create(step_stat_response->stats_list);
	while ((step_stat = list_next(itr))) {
		if (!step_stat->step_pids || !step_stat->step_pids->node_name)
			continue;
		if (step_stat->step_pids->pid_cnt > 0 ) {
			int i;
			for(i=0; i<step_stat->step_pids->pid_cnt; i++) {
				if (step.pid_str)
					xstrcat(step.pid_str, ",");
				xstrfmtcat(step.pid_str, "%u",
					   step_stat->step_pids->pid[i]);
			}
		}

		if (params.pid_format) {
			step.nodes = step_stat->step_pids->node_name;
			print_fields(&step);
			xfree(step.pid_str);
		} else {
			hostlist_push_host(hl, step_stat->step_pids->node_name);
			ntasks += step_stat->num_tasks;
			if (step_stat->jobacct) {
				jobacctinfo_2_stats(&temp_stats,
						    step_stat->jobacct);
				aggregate_stats(&step.stats, &temp_stats);
			}
		}
	}
	list_iterator_destroy(itr);
	slurm_job_step_pids_response_msg_free(step_stat_response);
	/* we printed it out already */
	if (params.pid_format)
		goto getout;

	hostlist_sort(hl);
	hostlist_ranged_string(hl, BUF_SIZE, step.nodes);
	hostlist_destroy(hl);
	tot_tasks += ntasks;

	if (tot_tasks) {
		step.stats.cpu_ave /= (double)tot_tasks;
		step.stats.rss_ave /= (double)tot_tasks;
		step.stats.vsize_ave /= (double)tot_tasks;
		step.stats.pages_ave /= (double)tot_tasks;
		step.stats.disk_read_ave /= (double)tot_tasks;
		step.stats.disk_write_ave /= (double)tot_tasks;
		step.stats.act_cpufreq /= (double)tot_tasks;

		ave_usage_in =
			slurmdb_ave_tres_usage(step.stats.tres_usage_in_ave,
			ave_usage_in, TRES_USAGE_DISK, tot_tasks);
		ave_usage_out =
			slurmdb_ave_tres_usage(step.stats.tres_usage_out_ave,
			ave_usage_out, TRES_USAGE_DISK, tot_tasks);
		tmp_string =
			slurmdb_ave_tres_usage(step.stats.tres_usage_in_ave,
			ave_usage_in, TRES_USAGE_FS_LUSTRE, tot_tasks);
		ave_usage_in = xstrdup(tmp_string);
		xfree(tmp_string);
		tmp_string =
			slurmdb_ave_tres_usage(step.stats.tres_usage_out_ave,
			ave_usage_out, TRES_USAGE_FS_LUSTRE, tot_tasks);
		ave_usage_out = xstrdup(tmp_string);
		xfree(tmp_string);
		xfree(step.stats.tres_usage_in_ave);
		step.stats.tres_usage_in_ave = xstrdup(ave_usage_in);
		xfree(step.stats.tres_usage_out_ave);
		step.stats.tres_usage_out_ave = xstrdup(ave_usage_out);
		xfree(ave_usage_in);
		xfree(ave_usage_out);

		step.ntasks = tot_tasks;

		xfree(temp_stats.tres_usage_in_max);
		xfree(temp_stats.tres_usage_out_max);
		xfree(temp_stats.tres_usage_in_max_taskid);
		xfree(temp_stats.tres_usage_out_max_taskid);
		xfree(temp_stats.tres_usage_in_max_nodeid);
		xfree(temp_stats.tres_usage_out_max_nodeid);
	}

	print_fields(&step);

getout:

	xfree(step.stats.tres_usage_in_max);
	xfree(step.stats.tres_usage_out_max);
	xfree(step.stats.tres_usage_in_max_taskid);
	xfree(step.stats.tres_usage_out_max_taskid);
	xfree(step.stats.tres_usage_in_max_nodeid);
	xfree(step.stats.tres_usage_out_max_nodeid);
	xfree(step.stats.tres_usage_in_ave);
	xfree(step.stats.tres_usage_out_ave);

	return rc;
}

int main(int argc, char **argv)
{
	ListIterator itr = NULL;
	uint32_t req_cpufreq_min = NO_VAL;
	uint32_t req_cpufreq_max = NO_VAL;
	uint32_t req_cpufreq_gov = NO_VAL;
	uint32_t stepid = NO_VAL;
	slurmdb_selected_step_t *selected_step = NULL;

#ifdef HAVE_ALPS_CRAY
	error("The sstat command is not supported on Cray systems");
	return 1;
#endif

	slurm_conf_init(NULL);
	print_fields_list = list_create(NULL);
	print_fields_itr = list_iterator_create(print_fields_list);

	parse_command_line(argc, argv);
	if (!params.opt_job_list || !list_count(params.opt_job_list)) {
		error("You didn't give me any jobs to stat.");
		return 1;
	}

	if (slurm_acct_storage_init(NULL) != SLURM_SUCCESS ) {
		error("failed to initialize accounting_storage plugin");
		return 1;
	} else {
		acct_db_conn = slurmdb_connection_get();
		if (errno != SLURM_SUCCESS) {
			info("Problem talking to the database: %m, no TRES "
			     "stats will be displayed");
			db_access = false;
		}
	}

	print_fields_header(print_fields_list);
	itr = list_iterator_create(params.opt_job_list);
	while ((selected_step = list_next(itr))) {
		char *nodelist = NULL;
		bool free_nodelist = false;
		uint16_t use_protocol_ver = NO_VAL16;
		if (selected_step->stepid == SSTAT_BATCH_STEP) {
			/* get the batch step info */
			job_info_msg_t *job_ptr = NULL;
			hostlist_t hl;

			if (slurm_load_job(
				    &job_ptr, selected_step->jobid, SHOW_ALL)) {
				error("couldn't get info for job %u",
				      selected_step->jobid);
				continue;
			}

			use_protocol_ver =
				job_ptr->job_array[0].start_protocol_ver;
			stepid = SLURM_BATCH_SCRIPT;
			hl = hostlist_create(job_ptr->job_array[0].nodes);
			nodelist = hostlist_shift(hl);
			free_nodelist = true;
			hostlist_destroy(hl);
			slurm_free_job_info_msg(job_ptr);
		} else if (selected_step->stepid == SSTAT_EXTERN_STEP) {
			/* get the extern step info */
			job_info_msg_t *job_ptr = NULL;

			if (slurm_load_job(
				    &job_ptr, selected_step->jobid, SHOW_ALL)) {
				error("couldn't get info for job %u",
				      selected_step->jobid);
				continue;
			}
			use_protocol_ver =
				job_ptr->job_array[0].start_protocol_ver;
			stepid = SLURM_EXTERN_CONT;
			nodelist = job_ptr->job_array[0].nodes;
			slurm_free_job_info_msg(job_ptr);
		} else if (selected_step->stepid != NO_VAL) {
			stepid = selected_step->stepid;
		} else if (params.opt_all_steps) {
			job_step_info_response_msg_t *step_ptr = NULL;
			int i = 0;
			if (slurm_get_job_steps(
				    0, selected_step->jobid, NO_VAL,
				    &step_ptr, SHOW_ALL)) {
				error("couldn't get steps for job %u",
				      selected_step->jobid);
				continue;
			}

			for (i = 0; i < step_ptr->job_step_count; i++) {
				_do_stat(selected_step->jobid,
					 step_ptr->job_steps[i].step_id,
					 step_ptr->job_steps[i].nodes,
					 step_ptr->job_steps[i].cpu_freq_min,
					 step_ptr->job_steps[i].cpu_freq_max,
					 step_ptr->job_steps[i].cpu_freq_gov,
					 step_ptr->job_steps[i].
					 start_protocol_ver);
			}
			slurm_free_job_step_info_response_msg(step_ptr);
			continue;
		} else {
			/* get the first running step to query against. */
			job_step_info_response_msg_t *step_ptr = NULL;
			job_step_info_t *step_info;

			if (slurm_get_job_steps(
				    0, selected_step->jobid, NO_VAL,
				    &step_ptr, SHOW_ALL)) {
				error("couldn't get steps for job %u",
				      selected_step->jobid);
				continue;
			}
			if (!step_ptr->job_step_count) {
				error("no steps running for job %u",
				      selected_step->jobid);
				continue;
			}

			/* If the first step is the extern step lets
			 * just skip it.  They should ask for it
			 * directly.
			 */
			if ((step_ptr->job_steps[0].step_id ==
			    SLURM_EXTERN_CONT) && step_ptr->job_step_count > 1)
				step_info = ++step_ptr->job_steps;
			else
				step_info = step_ptr->job_steps;
			stepid = step_info->step_id;
			nodelist = step_info->nodes;
			req_cpufreq_min = step_info->cpu_freq_min;
			req_cpufreq_max = step_info->cpu_freq_max;
			req_cpufreq_gov = step_info->cpu_freq_gov;
			use_protocol_ver = step_info->start_protocol_ver;
		}
		_do_stat(selected_step->jobid, stepid, nodelist,
			 req_cpufreq_min, req_cpufreq_max, req_cpufreq_gov,
			 use_protocol_ver);
		if (free_nodelist && nodelist)
			free(nodelist);
	}
	list_iterator_destroy(itr);

	xfree(params.opt_field_list);
	FREE_NULL_LIST(params.opt_job_list);
	if (print_fields_itr)
		list_iterator_destroy(print_fields_itr);
	FREE_NULL_LIST(print_fields_list);

	if (db_access) {
		FREE_NULL_LIST(g_tres_list);
		slurmdb_connection_close(&acct_db_conn);
	}
	slurm_acct_storage_fini();

	return 0;
}


