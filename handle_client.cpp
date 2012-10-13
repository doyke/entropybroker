// SVN: $Revision$
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <vector>
#include <string>
#include <map>

#include "error.h"
#include "random_source.h"
#include "log.h"
#include "math.h"
#include "ivec.h"
#include "hasher.h"
#include "stirrer.h"
#include "fips140.h"
#include "hasher_type.h"
#include "stirrer_type.h"
#include "users.h"
#include "encrypt_stream.h"
#include "encrypt_stream_blowfish.h"
#include "pool_crypto.h"
#include "pool.h"
#include "config.h"
#include "scc.h"
#include "pools.h"
#include "statistics.h"
#include "handle_client.h"
#include "utils.h"
#include "signals.h"
#include "auth.h"
#include "protocol.h"
#include "hc_protocol.h"

const char *pipe_cmd_str[] = { NULL, "have data (1)", "need data (2)", "is full (3)", "quit" };
extern const char *pid_file;

void forget_client_index(std::vector<client_t *> *clients, int nr, bool force)
{
	client_t *p = clients -> at(nr);

	close(p -> socket_fd);
	close(p -> to_thread[1]);
	close(p -> to_thread[0]);
	close(p -> to_main[0]);

	my_yield();

	if (force)
	{
		int ok[] = { ESRCH, 0 };
		pthread_check(pthread_cancel(p -> th), "pthread_cancel", ok);
	}

	void *value_ptr = NULL;
	pthread_check(pthread_join(p -> th, &value_ptr), "pthread_join");

	pthread_check(pthread_mutex_destroy(&p -> stats_lck), "pthread_mutex_destroy");

	delete p -> pfips140;
	delete p -> pscc;

	free(p -> username);
	free(p -> password);

	delete p -> pc;

	delete p;
	clients -> erase(clients -> begin() + nr);
}

void forget_client_thread_id(std::vector<client_t *> *clients, pthread_t *tid, bool force)
{
	for(unsigned int index=0; index<clients -> size(); index++)
	{
		if (pthread_equal(clients -> at(index) -> th, *tid) != 0)
		{
			forget_client_index(clients, index, force);

			break;
		}
	}
}

int send_pipe_command(int fd, unsigned char command)
{
	// printf("SEND TO MAIN %s\n", pipe_cmd_str[command]);
	for(;;)
	{
		int rc = write(fd, &command, 1);

		if (rc == 0)
			return -1;

		if (rc == -1)
		{
			if (errno == EINTR)
				continue;

			return -1;
		}

		break;
	}

	return 0;
}

int send_request_result_to_main_thread(bool is_server, int fd, bool no_bits, bool new_bits, bool is_full)
{
	int rc_pipe = 0;

	if (is_server)
	{
		if (new_bits)
			rc_pipe |= send_pipe_command(fd, PIPE_CMD_HAVE_DATA);

		if (is_full)
			rc_pipe |= send_pipe_command(fd, PIPE_CMD_IS_FULL);
	}
	else
	{
		if (no_bits)
			rc_pipe |= send_pipe_command(fd, PIPE_CMD_NEED_DATA);
	}

	return rc_pipe != 0 ? -1 : 0;
}

int send_request_from_main_to_clients(client_t *p)
{
	bool need_data = false, have_data = false, is_full = false;

	for(;;)
	{
		unsigned char cmd = 0;

		int rc_pipe = read(p -> to_thread[0], &cmd, 1);
		if (rc_pipe == 0)
			return -1;
		if (rc_pipe == -1)
		{
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;

			dolog(LOG_CRIT, "Thread connection to main thread lost (2)");

			return -1;
		}

		if (cmd == PIPE_CMD_NEED_DATA)
		{
			need_data = true;
			have_data = false;
			is_full = false;
		}
		else if (cmd == PIPE_CMD_HAVE_DATA)
		{
			need_data = false;
			have_data = true;
			is_full = false;
		}
		else if (cmd == PIPE_CMD_IS_FULL)
		{
			need_data = false;
			have_data = false;
			is_full = true;
		}
		else if (cmd == PIPE_CMD_QUIT)
		{
			return -1;
		}
		else
		{
			error_exit("Unknown interprocess command %02x", cmd);
		}
	}

///
	{
		static int err1=0, err2=0, err3=0;

		if (need_data && !p -> is_server)
		{
			err1++;
			fprintf(stderr, "%d %d %d\n", err1, err2, err3);
		}
		if (have_data && p -> is_server)
		{
			err2++;
			fprintf(stderr, "%d %d %d\n", err1, err2, err3);
		}
		if (is_full && !p -> is_server)
		{
			err3++;
			fprintf(stderr, "%d %d %d\n", err1, err2, err3);
		}
	}
///

	int rc_client = 0;
	if (need_data)
		rc_client |= notify_server_data_needed(p -> socket_fd, p -> stats, p -> config);

	if (have_data)
		rc_client |= notify_client_data_available(p -> socket_fd, p -> ppools, p -> stats, p -> config);

	if (is_full)
		rc_client |= notify_server_full(p -> socket_fd, p -> stats, p -> config);

	if (rc_client)
	{
		dolog(LOG_INFO, "Connection with %s lost", p -> host.c_str());

		return -1;
	}

	return 0;
}

void * thread(void *data)
{
	client_t *p = (client_t *)data;

	set_thread_name(p -> host);

	pthread_check(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL), "pthread_setcancelstate");

	if (p -> config -> disable_nagle)
		disable_nagle(p -> socket_fd);

	if (p -> config -> enable_keepalive)
		enable_tcp_keepalive(p -> socket_fd);

	encrypt_stream *es = encrypt_stream::select_cipher(p -> config -> stream_cipher);
	p -> stream_cipher = es;

	hasher *mh = hasher::select_hasher(p -> config -> mac_hasher);
	p -> mac_hasher = mh;

	unsigned char *ivec = reinterpret_cast<unsigned char *>(malloc(es -> get_ivec_size()));
	if (!ivec)
		error_exit("malloc failure");

	for(;;)
	{
		long long unsigned int auth_rnd = 1;
		std::string password, username;
		bool ok = auth_eb(p -> socket_fd, p -> config -> communication_timeout, p -> pu, username, password, &auth_rnd, &p -> is_server, p -> type, p -> config -> rs, es, mh, p -> config -> hash_hasher, p -> config -> max_get_put_size) == 0;

		if (!ok)
		{
			dolog(LOG_WARNING, "main|client: %s (fd: %d) authentication failed", p -> host.c_str(), p -> socket_fd);
			break;
		}

		dolog(LOG_INFO, "Thread id: %d, fd: %d, user: %s, type: %s, host: %s", gettid(), p -> socket_fd, username.c_str(), p -> type.c_str(), p -> host.c_str());

		p -> challenge = auth_rnd;
		p -> ivec_counter = 0;
		calc_ivec(password.c_str(), p -> challenge, p -> ivec_counter, es -> get_ivec_size(), ivec);
		// printf("IVEC: "); hexdump(ivec, 8);

		p -> username = strdup(username.c_str());
		set_thread_name(username + "/" + p -> host);

		p -> password = strdup(password.c_str());

		unsigned char *pw_char = reinterpret_cast<unsigned char *>(const_cast<char *>(password.c_str()));
		if (!es -> init(pw_char, password.length(), ivec))
		{
			dolog(LOG_CRIT, "Password for %s too weak (fd: %d)", username.c_str(), p -> socket_fd);
			break;
		}

		for(;;)
		{
			dolog(LOG_DEBUG, "loop %d, %d", gettid(), p -> socket_fd);

			struct timeval tv;

			tv.tv_sec = p -> config -> communication_session_timeout;
			tv.tv_usec = (p -> config -> communication_session_timeout - double(tv.tv_sec)) * 999999.0 + 1.0;
			my_assert(tv.tv_sec >= 0);
			my_assert(tv.tv_usec >= 0);
			if (tv.tv_sec == 0 && tv.tv_usec == 0)
				tv.tv_usec = 1000;

			int max_fd = -1;
			fd_set rfds;
			FD_ZERO(&rfds);

			FD_SET(p -> socket_fd, &rfds);
			max_fd = mymax(max_fd, p -> socket_fd);
			FD_SET(p -> to_thread[0], &rfds);
			max_fd = mymax(max_fd, p -> to_thread[0]);

			int rc = select(max_fd + 1, &rfds, NULL, NULL, &tv);

			// if (rc == -1)
			// 	dolog(LOG_DEBUG, "select: -1, %s (%d)", strerror(errno), errno);
			// else
			// 	dolog(LOG_DEBUG, "select: %d, ls %d|%d", rc, FD_ISSET(p -> socket_fd, &rfds), FD_ISSET(p -> to_thread[0], &rfds));

			if (rc == -1)
			{
				if (errno == EINTR)
					continue;

				dolog(LOG_CRIT, "select() failed for thread %s", p -> host.c_str());
				break;
			}
			else if (rc == 0)
			{
				dolog(LOG_CRIT, "host %s fell asleep", p -> host.c_str());
				break;
			}

			if (FD_ISSET(p -> socket_fd, &rfds))
			{
				bool no_bits = false, new_bits = false, is_full = false;

				dolog(LOG_DEBUG, "process fd: %d", p -> socket_fd);

				if (do_client(p, &no_bits, &new_bits, &is_full) == -1)
				{
					dolog(LOG_INFO, "Terminating connection with %s (fd: %d)", p -> host.c_str(), p -> socket_fd);
					break;
				}

				if (send_request_result_to_main_thread(p -> is_server, p -> to_main[1], no_bits, new_bits, is_full) != 0)
				{
					dolog(LOG_CRIT, "Thread connection to main thread lost (1)");
					break;
				}

				dolog(LOG_DEBUG, "finished processing fd: %d", p -> socket_fd);
			}

			if (FD_ISSET(p -> to_thread[0], &rfds))
			{
				dolog(LOG_DEBUG, "process from main to fd: %d", p -> socket_fd);
				if (send_request_from_main_to_clients(p) != 0)
					break;
				dolog(LOG_DEBUG, "process finished from main to fd: %d", p -> socket_fd);
			}
		}

		dolog(LOG_DEBUG, "End of thread imminent (fd: %d)", p -> socket_fd);

		break;
	}

	free(ivec);

	close(p -> to_main[1]);

	delete es;

	dolog(LOG_DEBUG, "End of thread (fd: %d)", p -> socket_fd);

	return NULL;
}

void register_new_client(int listen_socket_fd, std::vector<client_t *> *clients, users *user_map, config_t *config, pools *ppools, statistics *stats, fips140 *output_fips140, scc *output_scc)
{
	int new_socket_fd = accept(listen_socket_fd, NULL, NULL);

	if (new_socket_fd != -1)
	{
		std::string host = get_endpoint_name(new_socket_fd);
		dolog(LOG_INFO, "main|new client: %s (fd: %d)", host.c_str(), new_socket_fd);

		client_t *p = new client_t;
		if (!p)
			error_exit("memory allocation error");

		p -> username = NULL;
		p -> password = NULL;

		p -> socket_fd = new_socket_fd;
		p -> host = host;
		p -> type = "?";
		p -> is_server = false;

		p -> pfips140 = new fips140();
		p -> pscc = new scc();
		if (!p -> pfips140 || !p -> pscc)
			error_exit("failed allocating fips140/scc object");

		p -> pfips140 -> set_user(p -> host.c_str());
		p -> pscc     -> set_user(p -> host.c_str());
		p -> pscc -> set_threshold(config -> scc_threshold);

		double now = get_ts();
		p -> last_message = now;
		p -> connected_since = now;
		p -> last_put_message = now;

		p -> ivec_counter = 0;
		p -> password = NULL;

		p -> bits_sent = p -> bits_recv = 0;

		p -> max_bits_per_interval = config -> default_max_bits_per_interval;
		p -> ignore_rngtest_fips140 = config -> ignore_rngtest_fips140;
		p -> ignore_rngtest_scc = config -> ignore_rngtest_scc;
		p -> allow_prng = config -> allow_prng;

		p -> pc = new pool_crypto(config -> st, config -> ht, config -> rs);
		if (!p -> pc)
			error_exit("failed allocating pool_crypto object");

		pthread_check(pthread_mutex_init(&p -> stats_lck, &global_mutex_attr), "pthread_mutex_init");

		// globals
		p -> pu = user_map;
		p -> config = config;
		p -> ppools = ppools;
		p -> stats = stats;
		p -> output_fips140 = output_fips140;
		p -> output_scc = output_scc;

		if (pipe(p -> to_thread) == -1)
			error_exit("Error creating pipes");

		set_fd_nonblocking(p -> to_thread[0]);

		if (pipe(p -> to_main) == -1)
			error_exit("Error creating pipes");

		set_fd_nonblocking(p -> to_main[0]);

		pthread_check(pthread_create(&p -> th, NULL, thread, p), "pthread_create");

		clients -> push_back(p);
	}
}

int process_pipe_from_client_thread(client_t *p, std::vector<msg_pair_t> *msgs_clients, std::vector<msg_pair_t> *msgs_servers)
{
	int rc = 0;

	for(;;)
	{
		unsigned char cmd = 0;

		int rc_pipe = read(p -> to_main[0], &cmd, 1);
		if (rc_pipe == 0) // means closed!
			return -1;
		if (rc_pipe == -1)
		{
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;

			rc = -1;

			break;
		}

		// printf("RECV %s FROM %d\n", pipe_cmd_str[cmd], p -> socket_fd);

		msg_pair_t queue_entry = { p -> socket_fd, cmd };

		if (cmd == PIPE_CMD_HAVE_DATA)
			msgs_clients -> push_back(queue_entry);
		else if (cmd == PIPE_CMD_NEED_DATA)
			msgs_servers -> push_back(queue_entry);
		else if (cmd == PIPE_CMD_IS_FULL)
			msgs_servers -> push_back(queue_entry);
		else
			error_exit("Message %02x from thread %s/%s is not known", cmd, p -> host.c_str(), p -> type.c_str());
	}

	return rc;
}

void send_to_client_threads(std::vector<client_t *> *clients, std::vector<msg_pair_t> *msgs, bool is_server_in, bool *send_have_data, bool *send_need_data, bool *send_is_full)
{
	// printf("send_to_client_threads\n");
	for(unsigned int loop=0; loop<msgs -> size(); loop++)
	{
		msg_pair_t *cur_msg = &msgs -> at(loop);

		if (cur_msg -> cmd == PIPE_CMD_HAVE_DATA)
		{
			if (*send_have_data == true)
				continue;

			*send_have_data = true;
			*send_need_data = false;
		}
		else if (cur_msg -> cmd == PIPE_CMD_NEED_DATA)
		{
			if (*send_need_data == true)
				continue;

			*send_need_data = true;
			*send_have_data = false;
		}
		else if (cur_msg -> cmd == PIPE_CMD_IS_FULL)
		{
			if (*send_is_full == true)
				continue;

			*send_is_full = true;
			*send_need_data = false;
		}

		for(unsigned int index=0; index<clients -> size(); index++)
		{
			client_t *cur_cl = clients -> at(index);

			if (cur_cl -> is_server == is_server_in && cur_cl -> socket_fd != cur_msg -> fd_sender)
{
			// printf("SEND TO thread: %s to %d\n", pipe_cmd_str[cur_msg -> cmd], cur_cl -> socket_fd);
				(void)send_pipe_command(cur_cl -> to_thread[1], cur_msg -> cmd);
}
		}
	}
}

void terminate_threads(std::vector<client_t *> *clients)
{
	for(unsigned int index=0; index<clients -> size(); index++)
	{
		client_t *p = clients -> at(index);

		(void)send_pipe_command(p -> to_thread[1], PIPE_CMD_QUIT);

		my_yield();

		close(p -> socket_fd);
		close(p -> to_thread[0]);
		close(p -> to_thread[1]);
		close(p -> to_main[0]);
		close(p -> to_main[1]);
	}

	while(!clients -> empty())
	{
		client_t *p = clients -> at(0);

		dolog(LOG_DEBUG, "... %s/%s (fd: %d)", p -> host.c_str(), p -> type.c_str(), p -> socket_fd);

		forget_client_index(clients, 0, true);
	}
}

void main_loop(pools *ppools, config_t *config, fips140 *eb_output_fips140, scc *eb_output_scc, pool_crypto *pc)
{
	std::vector<client_t *> clients;
	double last_counters_reset = get_ts();
	double last_statistics_emit = get_ts();
	event_state_t event_state;
	int listen_socket_fd = start_listen(config -> listen_adapter, config -> listen_port, config -> listen_queue_size);
	statistics stats(config -> stats_file, eb_output_fips140, eb_output_scc, ppools);

	memset(&event_state, 0x00, sizeof event_state);

	dolog(LOG_INFO, "main|main-loop started");

	users *user_map = new users(*config -> user_map);
	if (!user_map)
		error_exit("failed allocating users-object");

	bool send_have_data = false, send_need_data = false, send_is_full = false;
	for(;;)
	{
		dolog(LOG_DEBUG, "main-loop");

		fd_set rfds;
		double now = get_ts();
		struct timespec tv;
		int max_fd = -1;
		bool force_stats = false;
		sigset_t sig_set;

		if (sigemptyset(&sig_set) == -1)
			error_exit("sigemptyset");

		FD_ZERO(&rfds);

		double time_left = 300.0, dummy1_time = -1.0;

		if (config -> statistics_interval > 0)
		{
			dummy1_time = mymax(0, (last_statistics_emit + config -> statistics_interval) - now);
			time_left = mymin(time_left, dummy1_time);
		}

		if (config -> reset_counters_interval > 0)
		{
			dummy1_time = mymax(0, (last_counters_reset + config -> reset_counters_interval) - now);
			time_left = mymin(time_left, dummy1_time);
		}

		for(unsigned int loop=0; loop<clients.size(); loop++)
		{
			FD_SET(clients.at(loop) -> to_main[0], &rfds);
			max_fd = mymax(max_fd, clients.at(loop) -> to_main[0]);
		}

		FD_SET(listen_socket_fd, &rfds);
		max_fd = mymax(max_fd, listen_socket_fd);

		tv.tv_sec = time_left;
		tv.tv_nsec = (time_left - double(tv.tv_sec)) * 1000000000.0;
		my_assert(tv.tv_sec >= 0);
		my_assert(tv.tv_nsec >= 0);
		if (tv.tv_sec == 0 && tv.tv_nsec == 0)
			tv.tv_nsec = 1000;

		int rc = pselect(max_fd + 1, &rfds, NULL, NULL, &tv, &sig_set);

		// if (rc == -1)
		// 	dolog(LOG_DEBUG, "pselect: -1, %s (%d) %f", strerror(errno), errno, time_left);
		// else
		// 	dolog(LOG_DEBUG, "pselect: %d, ls %d %f", rc, FD_ISSET(listen_socket_fd, &rfds), time_left);

		if (is_SIGHUP())
		{
			dolog(LOG_DEBUG, "Got SIGHUP");
			reset_SIGHUP();
			user_map -> reload();
			force_stats = true;
		}

		if (is_SIGEXIT())
		{
			dolog(LOG_INFO, "Program stopping due to either SIGTERM, SIGQUIT or SIGINT");
			break;
		}

		if (rc == -1)
		{
			if (errno == EBADF || errno == ENOMEM || errno == EINVAL)
				error_exit("pselect() failed");

			if (errno == EINTR)
				continue;

			dolog(LOG_DEBUG, "select returned with -1, errno: %s (%d)", strerror(errno), errno);
		}

		if ((last_counters_reset + double(config -> reset_counters_interval)) <= get_ts() || force_stats)
		{
			stats.emit_statistics_log(clients.size(), force_stats, config -> reset_counters_interval);

			if (!force_stats)
			{
				for(unsigned int loop=0; loop<clients.size(); loop++)
				{
					client_t *p = clients.at(loop);

					my_mutex_lock(&p -> stats_lck);
					p -> bits_recv = p -> bits_sent = 0;
					my_mutex_unlock(&p -> stats_lck);
				}
			}

			last_counters_reset = get_ts();
		}

		if ((config -> statistics_interval != 0 && (last_statistics_emit + double(config -> statistics_interval)) <= get_ts()) || force_stats)
		{
			stats.emit_statistics_file(clients.size());

			last_statistics_emit = get_ts();
		}

		if (force_stats)
		{
			now = get_ts();

			for(unsigned int loop=0; loop<clients.size(); loop++)
			{
				client_t *p = clients.at(loop);

				my_mutex_lock(&p -> stats_lck);
				dolog(LOG_DEBUG, "stats|%s (%s): %s, scc: %s | sent: %d, recv: %d | last msg: %ld seconds ago, %lds connected",
						p -> host.c_str(), p -> type.c_str(), p -> pfips140 -> stats(),
						p -> pscc -> stats(),
						p -> bits_sent, p -> bits_recv, (long int)(now - p -> last_message), (long int)(now - p -> connected_since));
				my_mutex_unlock(&p -> stats_lck);
			}
		}

		if (rc == 0)
			continue;

		if (config -> allow_event_entropy_addition)
		{
			now = get_ts();

			int event_bits = ppools -> add_event(now, reinterpret_cast<unsigned char *>(&rfds), sizeof rfds, double(config -> communication_timeout) * 0.05, pc);

			if (event_bits > 0)
				dolog(LOG_DEBUG, "main|added %d bits of event-entropy to pool", event_bits);
		}

		std::vector<pthread_t *> delete_ids;
		std::vector<msg_pair_t> msgs_clients;
		std::vector<msg_pair_t> msgs_servers;
		for(unsigned int loop=0; loop<clients.size(); loop++)
		{
			// this way we go through each fd in the process_pipe_from_client_thread part
			// so that we detect closed fds
			if (rc > 0 && !FD_ISSET(clients.at(loop) -> to_main[0], &rfds))
				continue;

			if (process_pipe_from_client_thread(clients.at(loop), &msgs_clients, &msgs_servers) == -1)
			{
				dolog(LOG_INFO, "main|connection with %s/%s lost", clients.at(loop) -> host.c_str(), clients.at(loop) -> type.c_str());

				delete_ids.push_back(&clients.at(loop) -> th);
			}
		}

		for(unsigned int loop=0; loop<delete_ids.size(); loop++)
			forget_client_thread_id(&clients, delete_ids.at(loop), true);

		send_to_client_threads(&clients, &msgs_clients, false, &send_have_data, &send_need_data, &send_is_full);
		send_to_client_threads(&clients, &msgs_servers, true, &send_have_data, &send_need_data, &send_is_full);

		if (rc > 0 && FD_ISSET(listen_socket_fd, &rfds))
		{
			register_new_client(listen_socket_fd, &clients, user_map, config, ppools, &stats, eb_output_fips140, eb_output_scc);
			send_have_data = send_need_data = send_is_full = false;
		}
	}

	dolog(LOG_INFO, "Terminating %d threads...", clients.size());

	terminate_threads(&clients);

	delete user_map;

	close(listen_socket_fd);

	dolog(LOG_INFO, "main|end of main loop");
}
