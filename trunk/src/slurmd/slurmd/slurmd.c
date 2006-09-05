/*****************************************************************************\
 *  src/slurmd/slurmd/slurmd.c - main slurm node server daemon
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>.
 *  UCRL-CODE-217948.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
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

#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <dlfcn.h>

#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/pack.h"
#include "src/common/read_config.h"
#include "src/common/switch.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/common/xsignal.h"
#include "src/common/daemonize.h"
#include "src/common/slurm_cred.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/parse_spec.h"
#include "src/common/hostlist.h"
#include "src/common/macros.h"
#include "src/common/fd.h"
#include "src/common/forward.h"

#include "src/slurmd/slurmd/slurmd.h"
#include "src/slurmd/slurmd/req.h"
#include "src/slurmd/slurmd/get_mach_stat.h"
#include "src/slurmd/common/stepd_api.h"
#include "src/slurmd/common/setproctitle.h"
#include "src/slurmd/common/proctrack.h"
#include "src/slurmd/common/task_plugin.h"

#define GETOPT_ARGS	"L:Dvhcf:MN:V"

#ifndef MAXHOSTNAMELEN
#  define MAXHOSTNAMELEN	64
#endif

#define MAX_THREADS		130

/* global, copied to STDERR_FILENO in tasks before the exec */
int devnull = -1;

typedef struct connection {
	slurm_fd fd;
	slurm_addr *cli_addr;
} conn_t;

/*
 * count of active threads
 */
static int             active_threads = 0;
static pthread_mutex_t active_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  active_cond    = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t fork_mutex     = PTHREAD_MUTEX_INITIALIZER;



/*
 * static shutdown and reconfigure flags:
 */
static sig_atomic_t _shutdown = 0;
static sig_atomic_t _reconfig = 0;
static pthread_t msg_pthread = (pthread_t) 0;

static void      _term_handler(int);
static void      _hup_handler(int);
static void      _process_cmdline(int ac, char **av);
static void      _create_msg_socket();
static void      _msg_engine();
static int       _slurmd_init();
static int       _slurmd_fini();
static void      _init_conf();
static void      _destroy_conf();
static void      _print_conf();
static void      _read_config();
static void 	 _kill_old_slurmd();
static void      _reconfigure();
static int       _restore_cred_state(slurm_cred_ctx_t ctx);
static void      _increment_thd_count();
static void      _decrement_thd_count();
static void      _wait_for_all_threads();
static int       _set_slurmd_spooldir(void);
static void      _usage();
static void      _handle_connection(slurm_fd fd, slurm_addr *client);
static void     *_service_connection(void *);
static void      _fill_registration_msg(slurm_node_registration_status_msg_t *);
static void      _update_logging(void);
static void      _atfork_prepare(void);
static void      _atfork_final(void);
static void      _install_fork_handlers(void);

int 
main (int argc, char *argv[])
{
	int pidfd;
	int blocked_signals[] = {SIGPIPE, 0};

	/*
	 * Create and set default values for the slurmd global
	 * config variable "conf"
	 */
	conf = xmalloc(sizeof(slurmd_conf_t));
	_init_conf();
	conf->argv = &argv;
	conf->argc = &argc;

	init_setproctitle(argc, argv);

	/* NOTE: conf->logfile always NULL at this point */
	log_init(argv[0], conf->log_opts, LOG_DAEMON, conf->logfile);

	xsignal(SIGTERM, &_term_handler);
	xsignal(SIGINT,  &_term_handler);
	xsignal(SIGHUP,  &_hup_handler );
	xsignal_block(blocked_signals);

	/* 
	 * Run slurmd_init() here in order to report early errors
	 * (with public keyfile)
	 */
	if (_slurmd_init() < 0) {
		error( "slurmd initialization failed" );
		fflush( NULL );
		exit(1);
	}

	debug3("slurmd initialization successful");

	/* 
	 * Become a daemon if desired.
	 * Do not chdir("/") or close all fd's
	 */
	if (conf->daemonize) 
		daemon(1,1);
	info("slurmd version %s started", SLURM_VERSION);
	debug3("finished daemonize");

	_kill_old_slurmd();

	if (conf->mlock_pages) {
		/*
		 * Call mlockall() if available to ensure slurmd
		 *  doesn't get swapped out
		 */
#ifdef _POSIX_MEMLOCK
		if (mlockall (MCL_FUTURE | MCL_CURRENT) < 0)
			error ("failed to mlock() slurmd pages: %m");
#else
		error ("mlockall() system call does not appear to be available");
#endif /* _POSIX_MEMLOCK */
	}


	/* 
	 * Restore any saved revoked credential information
	 */
	if (!conf->cleanstart && (_restore_cred_state(conf->vctx) < 0))
		return SLURM_FAILURE;
	
	if (interconnect_node_init() < 0)
		fatal("Unable to initialize interconnect.");
	if (conf->cleanstart && switch_g_clear_node_state())
		fatal("Unable to clear interconnect state.");
	switch_g_slurmd_init();

	_create_msg_socket();

	conf->pid = getpid();
	pidfd = create_pidfile(conf->pidfile);
	if (pidfd >= 0)
		fd_set_close_on_exec(pidfd);

	info("%s started on %T", xbasename(argv[0]));

        if (send_registration_msg(SLURM_SUCCESS, true) < 0) 
		error("Unable to register with slurm controller");

	_install_fork_handlers();
	list_install_fork_handlers();
	slurm_conf_install_fork_handlers();
	
	_msg_engine();

	/*
	 * Close fd here, otherwise we'll deadlock since create_pidfile()
	 * flocks the pidfile.
	 */
	if (pidfd >= 0)			/* valid pidfd, non-error */
		(void) close(pidfd);	/* Ignore errors */
	if (unlink(conf->pidfile) < 0)
		error("Unable to remove pidfile `%s': %m", conf->pidfile);

	_wait_for_all_threads();

	interconnect_node_fini();

	_slurmd_fini();
	_destroy_conf();
       	return 0;
}


static void
_msg_engine()
{
	slurm_fd sock;

	msg_pthread = pthread_self();
	while (!_shutdown) {
		slurm_addr *cli = xmalloc (sizeof (slurm_addr));
		if ((sock = slurm_accept_msg_conn(conf->lfd, cli)) >= 0) {
			_handle_connection(sock, cli);
			continue;
		}
		/*
		 *  Otherwise, accept() failed.
		 */
		xfree (cli);
		if (errno == EINTR) {
			if (_reconfig) {
				verbose("got reconfigure request");
				_reconfigure();
			}
			continue;
		} 
		error("accept: %m");
	}
	verbose("got shutdown request");
	slurm_shutdown_msg_engine(conf->lfd);
	return;
}

static void
_decrement_thd_count(void)
{
	slurm_mutex_lock(&active_mutex);
	active_threads--;
	pthread_cond_signal(&active_cond);
	slurm_mutex_unlock(&active_mutex);
}

static void
_increment_thd_count(void)
{
	bool logged = false;

	slurm_mutex_lock(&active_mutex);
	while (active_threads >= MAX_THREADS) {
		if (!logged) {
			info("active_threads == MAX_THREADS(%d)", 
				MAX_THREADS);
			logged = true;
		}
		pthread_cond_wait(&active_cond, &active_mutex);
	}
	active_threads++;
	slurm_mutex_unlock(&active_mutex);
}

static void
_wait_for_all_threads()
{
	slurm_mutex_lock(&active_mutex);
	while (active_threads > 0) {
		verbose("waiting on %d active threads", active_threads);
		pthread_cond_wait(&active_cond, &active_mutex);
	}
	slurm_mutex_unlock(&active_mutex);
	verbose("all threads complete.");
}

static void
_handle_connection(slurm_fd fd, slurm_addr *cli)
{
	int            rc;
	pthread_attr_t attr;
	pthread_t      id;
	conn_t         *arg = xmalloc(sizeof(conn_t));

	arg->fd       = fd;
	arg->cli_addr = cli;

	slurm_attr_init(&attr);
	rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (rc != 0) {
		errno = rc;
		xfree(arg);
		error("Unable to set detachstate on attr: %m");
		slurm_attr_destroy(&attr);
		return;
	}

	fd_set_close_on_exec(fd);

	_increment_thd_count();
	rc = pthread_create(&id, &attr, &_service_connection, (void *) arg);
	slurm_attr_destroy(&attr);
	if (rc != 0) {
		error("msg_engine: pthread_create: %s", slurm_strerror(rc));
		_service_connection((void *) arg);
		return;
	}
	return;
}

static void *
_service_connection(void *arg)
{
	conn_t *con = (conn_t *) arg;
	List ret_list = NULL;
	slurm_msg_t *msg = xmalloc(sizeof(slurm_msg_t));
		
	debug3("in the service_connection");
	slurm_init_slurm_msg(msg, NULL);
	msg->conn_fd = con->fd;
	/* this could change if being forwarded to */
	memcpy(&msg->orig_addr, con->cli_addr, sizeof(slurm_addr));
	
	ret_list = slurm_receive_msg(con->fd, msg, 0);	
	if(!ret_list || errno != SLURM_SUCCESS) {
		error("service_connection: slurm_receive_msg: %m");
		goto cleanup;
	}

	/* set msg connection fd to accepted fd. This allows 
	 *  possibility for slurmd_req () to close accepted connection
	 */
	/* this always is the connection */
	memcpy(&msg->address, con->cli_addr, sizeof(slurm_addr));

	debug2("got this type of message %d with %d other responses",
	     msg->msg_type, list_count(ret_list));
	msg->ret_list = ret_list;
	slurmd_req(msg);
	
cleanup:
	if ((msg->conn_fd >= 0) && slurm_close_accepted_conn(msg->conn_fd) < 0)
		error ("close(%d): %m", con->fd);

	xfree(con->cli_addr);
	xfree(con);
	slurm_free_msg(msg);
	_decrement_thd_count();
	return NULL;
}

int
send_registration_msg(uint32_t status, bool startup)
{
	int retval = SLURM_SUCCESS;
	slurm_msg_t req;
	slurm_msg_t resp;
	slurm_node_registration_status_msg_t *msg = 
		xmalloc (sizeof (slurm_node_registration_status_msg_t));
	
	slurm_init_slurm_msg(&req, NULL);
	slurm_init_slurm_msg(&resp, NULL);
	
	msg->startup = (uint16_t) startup;
	_fill_registration_msg(msg);
	msg->status  = status;

	req.msg_type = MESSAGE_NODE_REGISTRATION_STATUS;
	req.data     = msg;

	if (slurm_send_recv_controller_msg(&req, &resp) < 0) {
		error("Unable to register: %m");
		retval = SLURM_FAILURE;
	} else
		slurm_free_return_code_msg(resp.data);	
	slurm_free_node_registration_status_msg (msg);

	/* XXX look at response msg
	 */

	return SLURM_SUCCESS;
}

static void
_fill_registration_msg(slurm_node_registration_status_msg_t *msg)
{
	List steps;
	ListIterator i;
	step_loc_t *stepd;
	int          n;
	slurm_ctl_conf_t *cf;

	msg->node_name = xstrdup (conf->node_name);

	get_procs(&msg->cpus);
	get_memory(&msg->real_memory_size);
	cf = slurm_conf_lock();
	get_tmp_disk(&msg->temporary_disk_space, cf->tmp_fs);
	slurm_conf_unlock();
	debug3("Procs=%u RealMemory=%u, TmpDisk=%u", msg->cpus, 
	       msg->real_memory_size, msg->temporary_disk_space);

	if (msg->startup) {
		if (switch_g_alloc_node_info(&msg->switch_nodeinfo))
			error("switch_g_alloc_node_info: %m");
		if (switch_g_build_node_info(msg->switch_nodeinfo))
			error("switch_g_build_node_info: %m");
	}

	steps = stepd_available(conf->spooldir, conf->node_name);
	msg->job_count = list_count(steps);
	msg->job_id    = xmalloc(msg->job_count * sizeof(*msg->job_id));
	/* Note: Running batch jobs will have step_id == NO_VAL */
	msg->step_id   = xmalloc(msg->job_count * sizeof(*msg->step_id));

	i = list_iterator_create(steps);
	n = 0;
	while ((stepd = list_next(i))) {
		int fd;
		fd = stepd_connect(stepd->directory, stepd->nodename,
				   stepd->jobid, stepd->stepid);
		if (fd == -1) {
			--(msg->job_count);
			continue;
		}
		if (stepd_state(fd) == SLURMSTEPD_NOT_RUNNING) {
			debug("stale domain socket for stepd %u.%u ",
			      stepd->jobid, stepd->stepid);
			--(msg->job_count);
			close(fd);
			continue;
		}
		close(fd);
		if (stepd->stepid == NO_VAL)
			debug("found apparently running job %u", stepd->jobid);
		else
			debug("found apparently running step %u.%u", 
			      stepd->jobid, stepd->stepid);
		msg->job_id[n]  = stepd->jobid;
		msg->step_id[n] = stepd->stepid;
		n++;
	}
	list_iterator_destroy(i);
	list_destroy(steps);

	msg->timestamp = time(NULL);

	return;
}

static inline int
_free_and_set(char **confvar, char *newval)
{
	if (newval) {
		if (*confvar)
			xfree(*confvar);
		*confvar = newval;
		return 1;
	} else
		return 0;
}

/* Replace first "%h" in path string with actual hostname.
 * Replace first "%n" in path string with NodeName.
 *
 * Make sure to call _massage_pathname AFTER conf->node_name has been
 * fully initialized.
 */
static void
_massage_pathname(char **path)
{
	if (conf->logfile == NULL)
		return;

	xstrsubstitute(*path, "%h", conf->hostname);
	xstrsubstitute(*path, "%n", conf->node_name);
}

/*
 * Read the slurm configuration file (slurm.conf) and substitute some
 * values into the slurmd configuration in preference of the defaults.
 */
static void
_read_config()
{
        char *path_pubkey = NULL;
	slurm_ctl_conf_t *cf = NULL;
	slurm_conf_reinit(conf->conffile);
	cf = slurm_conf_lock();
	
	slurm_mutex_lock(&conf->config_mutex);

	if (conf->conffile == NULL)
		conf->conffile = xstrdup(cf->slurm_conf);

	conf->slurm_user_id =  cf->slurm_user_id;

	path_pubkey = xstrdup(cf->job_credential_public_certificate);

	if (!conf->logfile)
		conf->logfile = xstrdup(cf->slurmd_logfile);

	slurm_conf_unlock();
	/* node_name may already be set from a command line parameter */
	if (conf->node_name == NULL)
		conf->node_name = slurm_conf_get_nodename(conf->hostname);
	if (conf->node_name == NULL)
		conf->node_name = slurm_conf_get_nodename("localhost");
	if (conf->node_name == NULL)
		fatal("Unable to determine this slurmd's NodeName");

	_massage_pathname(&conf->logfile);

	conf->port = slurm_conf_get_port(conf->node_name);

	cf = slurm_conf_lock();
	_free_and_set(&conf->epilog,   xstrdup(cf->epilog));
	_free_and_set(&conf->prolog,   xstrdup(cf->prolog));
	_free_and_set(&conf->tmpfs,    xstrdup(cf->tmp_fs));
	_free_and_set(&conf->spooldir, xstrdup(cf->slurmd_spooldir));
	_massage_pathname(&conf->spooldir);
	_free_and_set(&conf->pidfile,  xstrdup(cf->slurmd_pidfile));
	_massage_pathname(&conf->pidfile);
	_free_and_set(&conf->task_prolog, xstrdup(cf->task_prolog));
	_free_and_set(&conf->task_epilog, xstrdup(cf->task_epilog));
	_free_and_set(&conf->pubkey,   path_pubkey);
	
	conf->job_acct_freq = cf->job_acct_freq;

	if ( (conf->node_name == NULL) ||
	     (conf->node_name[0] == '\0') )
		fatal("Node name lookup failure");
 
	if (cf->control_addr == NULL)
		fatal("Unable to establish controller machine");
	if (cf->slurmctld_port == 0)
		fatal("Unable to establish controller port");
	conf->use_pam = cf->use_pam;
	
	slurm_mutex_unlock(&conf->config_mutex);
	slurm_conf_unlock();
}

static void
_reconfigure(void)
{
	slurm_ctl_conf_t *cf;
	
	_reconfig = 0;
	_read_config();
	
	_update_logging();
	_print_conf();
	
	/*
	 * Make best effort at changing to new public key
	 */
	slurm_cred_ctx_key_update(conf->vctx, conf->pubkey);

	/*
	 * Reinitialize the groups cache
	 */
	cf = slurm_conf_lock();
	init_gids_cache(cf->cache_groups);
	slurm_conf_unlock();

	/*
	 * XXX: reopen slurmd port?
	 */
}

static void
_print_conf()
{
	slurm_ctl_conf_t *cf;

	cf = slurm_conf_lock();
	debug3("CacheGroups = %d",       cf->cache_groups);
	debug3("Confile     = `%s'",     conf->conffile);
	debug3("Debug       = %d",       cf->slurmd_debug);
	debug3("Epilog      = `%s'",     conf->epilog);
	debug3("Logfile     = `%s'",     cf->slurmd_logfile);
	debug3("NodeName    = %s",       conf->node_name);
	debug3("Port        = %u",       conf->port);
	debug3("Prolog      = `%s'",     conf->prolog);
	debug3("TmpFS       = `%s'",     conf->tmpfs);
	debug3("Public Cert = `%s'",     conf->pubkey);
	debug3("Spool Dir   = `%s'",     conf->spooldir);
	debug3("Pid File    = `%s'",     conf->pidfile);
	debug3("Slurm UID   = %u",       conf->slurm_user_id);
	debug3("TaskProlog  = `%s'",     conf->task_prolog);
	debug3("TaskEpilog  = `%s'",     conf->task_epilog);
	slurm_conf_unlock();
}

static void
_init_conf()
{
	char  host[MAXHOSTNAMELEN];
	log_options_t lopts = LOG_OPTS_INITIALIZER;

	if (getnodename(host, MAXHOSTNAMELEN) < 0) {
		error("Unable to get my hostname: %m");
		exit(1);
	}
	conf->hostname    = xstrdup(host);
	conf->node_name   = NULL;
	conf->conffile    = NULL;
	conf->epilog      = NULL;
	conf->logfile     = NULL;
	conf->pubkey      = NULL;
	conf->prolog      = NULL;
	conf->task_prolog = NULL;
	conf->task_epilog = NULL;
	conf->port        =  0;
	conf->daemonize   =  1;
	conf->lfd         = -1;
	conf->cleanstart  =  0;
	conf->mlock_pages =  0;
	conf->log_opts    = lopts;
	conf->debug_level = LOG_LEVEL_INFO;
	conf->pidfile     = xstrdup(DEFAULT_SLURMD_PIDFILE);
	conf->spooldir	  = xstrdup(DEFAULT_SPOOLDIR);
	conf->use_pam	  =  0;

	slurm_mutex_init(&conf->config_mutex);
	return;
}

static void
_destroy_conf()
{
	if(conf) {
		xfree(conf->hostname);
		xfree(conf->node_name);
		xfree(conf->conffile);
		xfree(conf->prolog);
		xfree(conf->epilog);
		xfree(conf->logfile);
		xfree(conf->pubkey);
		xfree(conf->task_prolog);
		xfree(conf->task_epilog);
		xfree(conf->pidfile);
		xfree(conf->spooldir);
		xfree(conf->tmpfs);
		slurm_mutex_destroy(&conf->config_mutex);
		slurm_cred_ctx_destroy(conf->vctx);
		xfree(conf);
	}
	return;
}

static void
_process_cmdline(int ac, char **av)
{
	int c;

	conf->prog = xbasename(av[0]);

	while ((c = getopt(ac, av, GETOPT_ARGS)) > 0) {
		switch (c) {
		case 'D': 
			conf->daemonize = 0;
			break;
		case 'v':
			conf->debug_level++;
			break;
		case 'f':
			conf->conffile = xstrdup(optarg);
			break;
		case 'h':
			_usage();
			exit(0);
			break;
		case 'L':
			conf->logfile = xstrdup(optarg);
			break;
		case 'c':
			conf->cleanstart = 1;
			break;
		case 'M':
			conf->mlock_pages = 1;
			break;
		case 'N':
			conf->node_name = xstrdup(optarg);
			break;
		case 'V':
			printf("%s %s\n", PACKAGE, SLURM_VERSION);
			exit(0);
			break;
		default:
			_usage(c);
			exit(1);
			break;
		}
	}
}


static void
_create_msg_socket()
{
	slurm_fd ld = slurm_init_msg_engine_port(conf->port);

	if (ld < 0) {
		error("Unable to bind listen port (%d): %m", conf->port);
		exit(1);
	}

	fd_set_close_on_exec(ld);

	conf->lfd = ld;

	debug3("succesfully opened slurm listen port %d", conf->port);

	return;
}


static int
_slurmd_init()
{
	struct rlimit rlim;
	slurm_ctl_conf_t *cf;

	/*
	 * Process commandline arguments first, since one option may be
	 * an alternate location for the slurm config file.
	 */
	_process_cmdline(*conf->argc, *conf->argv);

	/*
	 * Read global slurm config file, ovverride necessary values from
	 * defaults and command line.
	 *
	 */
	_read_config();

	/* 
	 * Update location of log messages (syslog, stderr, logfile, etc.),
	 * print current configuration (if in debug mode), and 
	 * load appropriate plugin(s).
	 */
	_update_logging();
	_print_conf();
	if (slurm_proctrack_init() != SLURM_SUCCESS)
		return SLURM_FAILURE;
	if (slurmd_task_init() != SLURM_SUCCESS)
		return SLURM_FAILURE;

	if (getrlimit(RLIMIT_NOFILE,&rlim) == 0) {
		rlim.rlim_cur = rlim.rlim_max;
		setrlimit(RLIMIT_NOFILE,&rlim);
	}

#ifndef NDEBUG
	if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
		rlim.rlim_cur = rlim.rlim_max;
		setrlimit(RLIMIT_CORE, &rlim);
	}
#endif /* !NDEBUG */

	/*
	 * Create a context for verifying slurm job credentials
	 */
	if (!(conf->vctx = slurm_cred_verifier_ctx_create(conf->pubkey)))
		return SLURM_FAILURE;

	/* 
	 * Create slurmd spool directory if necessary.
	 */
	if (_set_slurmd_spooldir() < 0) {
		error("Unable to initialize slurmd spooldir");
		return SLURM_FAILURE;
	}

	if (conf->cleanstart) {
		/* 
		 * Need to kill any running slurmd's here
		 */
		_kill_old_slurmd();

		stepd_cleanup_sockets(conf->spooldir, conf->node_name);
	}

	if (conf->daemonize && (chdir("/tmp") < 0)) {
		error("Unable to chdir to /tmp");
		return SLURM_FAILURE;
	}

	/*
	 * Cache the group access list
	 */
	cf = slurm_conf_lock();
	init_gids_cache(cf->cache_groups);
	slurm_conf_unlock();

	if ((devnull = open("/dev/null", O_RDWR)) < 0) {
		error("Unable to open /dev/null: %m");
		return SLURM_FAILURE;
	}
	fd_set_close_on_exec(devnull);

	return SLURM_SUCCESS;
}

static int
_restore_cred_state(slurm_cred_ctx_t ctx)
{
	char *file_name = NULL, *data = NULL;
	uint32_t data_size = 0;
	int cred_fd, data_allocated, data_read = 0;
	Buf buffer = NULL;

	if ( (mkdir(conf->spooldir, 0755) < 0) && (errno != EEXIST) ) {
		fatal("mkdir(%s): %m", conf->spooldir);
		return SLURM_ERROR;
	}

	file_name = xstrdup(conf->spooldir);
	xstrcat(file_name, "/cred_state");
	cred_fd = open(file_name, O_RDONLY);
	if (cred_fd < 0) 
		goto cleanup;

	data_allocated = 1024;
	data = xmalloc(sizeof(char)*data_allocated);
	while ((data_read = read(cred_fd, &data[data_size], 1024)) == 1024) {
		data_size += data_read;
		data_allocated += 1024;
		xrealloc(data, data_allocated);
	}
	data_size += data_read;
	close(cred_fd);
	buffer = create_buf(data, data_size);

	slurm_cred_ctx_unpack(ctx, buffer);

      cleanup:
	xfree(file_name);
	if (buffer)
		free_buf(buffer);
	return SLURM_SUCCESS;
}

static int
_slurmd_fini()
{
	save_cred_state(conf->vctx);
	slurmd_task_fini(); 
	slurm_conf_destroy();
	
	return SLURM_SUCCESS;
}

/*
 * save_cred_state - save the current credential list to a file
 * IN list - list of credentials
 * RET int - zero or error code
 */
int save_cred_state(slurm_cred_ctx_t ctx)
{
	char *old_file, *new_file, *reg_file;
	int cred_fd = -1, error_code = SLURM_SUCCESS;
	Buf buffer = NULL;
	static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

	old_file = xstrdup(conf->spooldir);
	xstrcat(old_file, "/cred_state.old");
	reg_file = xstrdup(conf->spooldir);
	xstrcat(reg_file, "/cred_state");
	new_file = xstrdup(conf->spooldir);
	xstrcat(new_file, "/cred_state.new");

	slurm_mutex_lock(&state_mutex);
	if ((cred_fd = creat(new_file, 0600)) < 0) {
		error("creat(%s): %m", new_file);
		error_code = errno;
		goto cleanup;
	}
	buffer = init_buf(1024);
	slurm_cred_ctx_pack(ctx, buffer);
	if (write(cred_fd, get_buf_data(buffer), 
		  get_buf_offset(buffer)) != get_buf_offset(buffer)) {
		error("write %s error %m", new_file);
		(void) unlink(new_file);
		error_code = errno;
		goto cleanup;
	}
	(void) unlink(old_file);
	(void) link(reg_file, old_file);
	(void) unlink(reg_file);
	(void) link(new_file, reg_file);
	(void) unlink(new_file);

      cleanup:
	slurm_mutex_unlock(&state_mutex);
	xfree(old_file);
	xfree(reg_file);
	xfree(new_file);
	if (buffer)
		free_buf(buffer);
	if (cred_fd > 0)
		close(cred_fd);
	return error_code;
}

static void
_term_handler(int signum)
{
	if (signum == SIGTERM || signum == SIGINT) { 
		_shutdown = 1;
		if (msg_pthread && (pthread_self() != msg_pthread))
			pthread_kill(msg_pthread, SIGTERM);
	}
}

static void 
_hup_handler(int signum)
{
	if (signum == SIGHUP) {
		_reconfig = 1;
	}
}


static void 
_usage()
{
	fprintf(stderr, "\
Usage: %s [OPTIONS]\n\
   -L logfile  Log messages to the file `logfile'\n\
   -v          Verbose mode. Multiple -v's increase verbosity.\n\
   -D          Run daemon in foreground.\n\
   -M          Use mlock() to lock slurmd pages into memory.\n\
   -c          Force cleanup of slurmd shared memory.\n\
   -h          Print this help message.\n", conf->prog);
	return;
}

/* 
 * create spool directory as needed and "cd" to it 
 */
static int
_set_slurmd_spooldir(void)
{
	debug3("initializing slurmd spool directory");

	if (mkdir(conf->spooldir, 0755) < 0) {
		if (errno != EEXIST) {
			fatal("mkdir(%s): %m", conf->spooldir);
			return SLURM_ERROR;
		}
	}

	/* 
	 * Ensure spool directory permissions are correct.
	 */
	if (chmod(conf->spooldir, 0755) < 0) {
		error("chmod(%s, 0755): %m", conf->spooldir);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

/* Kill the currently running slurmd 
 *
 * Returns file descriptor for the existing pidfile so that the
 * current slurmd can wait on termination of the old.
 */
static void 
_kill_old_slurmd(void)
{
	int fd;
	pid_t oldpid = read_pidfile(conf->pidfile, &fd);
	if (oldpid != (pid_t) 0) {
		info ("killing old slurmd[%lu]", (unsigned long) oldpid);
		kill(oldpid, SIGTERM);

		/* 
		 * Wait for previous daemon to terminate
		 */
		if (fd_get_readw_lock(fd) < 0) 
			fatal ("unable to wait for readw lock: %m");
		(void) close(fd); /* Ignore errors */ 
	}
}

/* Reset slurmctld logging based upon configuration parameters */
static void _update_logging(void) 
{
	log_options_t *o = &conf->log_opts;
	slurm_ctl_conf_t *cf;

	/* 
	 * Initialize debug level if not already set
	 */
	cf = slurm_conf_lock();
	if ( (conf->debug_level == LOG_LEVEL_INFO)
	    && (cf->slurmd_debug != (uint16_t) NO_VAL) )
		conf->debug_level = cf->slurmd_debug; 
	slurm_conf_unlock();

	o->stderr_level  = conf->debug_level;
	o->logfile_level = conf->debug_level;
	o->syslog_level  = conf->debug_level;

	/*
	 * If daemonizing, turn off stderr logging -- also, if
	 * logging to a file, turn off syslog.
	 *
	 * Otherwise, if remaining in foreground, turn off logging
	 * to syslog (but keep logfile level)
	 */
	if (conf->daemonize) {
		o->stderr_level = LOG_LEVEL_QUIET;
		if (conf->logfile)
			o->syslog_level = LOG_LEVEL_QUIET;
	} else 
		o->syslog_level  = LOG_LEVEL_QUIET;

	log_alter(conf->log_opts, SYSLOG_FACILITY_DAEMON, conf->logfile);
}

/*
 *  Lock the fork mutex to protect fork-critical regions
 */
static void _atfork_prepare(void)
{
	slurm_mutex_lock(&fork_mutex);
}

/*
 *  Unlock  fork mutex to allow fork-critical functions to continue
 */
static void _atfork_final(void)
{
	slurm_mutex_unlock(&fork_mutex);
}

static void _install_fork_handlers(void) 
{
	int err;

	err = pthread_atfork(&_atfork_prepare, &_atfork_final, &_atfork_final);
	if (err) error ("pthread_atfork: %m");

	return;
}

