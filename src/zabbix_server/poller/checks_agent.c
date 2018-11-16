/*
** Zabbix
** Copyright (C) 2001-2018 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "comms.h"
#include "log.h"
#include "../../libs/zbxcrypto/tls_tcp_active.h"

#include "checks_agent.h"

#if !(defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL))
extern unsigned char	program_type;
#endif

/******************************************************************************
 *                                                                            *
 * Function: get_value_agent                                                  *
 *                                                                            *
 * Purpose: retrieve data from Zabbix agent                                   *
 *                                                                            *
 * Parameters: item - item we are interested in                               *
 *                                                                            *
 * Return value: SUCCEED - data successfully retrieved and stored in result   *
 *                         and result_str (as string)                         *
 *               NETWORK_ERROR - network related error occurred               *
 *               NOTSUPPORTED - item not supported by the agent               *
 *               AGENT_ERROR - uncritical error on agent side occurred        *
 *               FAIL - otherwise                                             *
 *                                                                            *
 * Author: Alexei Vladishev                                                   *
 *                                                                            *
 * Comments: error will contain error message                                 *
 *                                                                            *
 ******************************************************************************/
int	get_value_agent(DC_ITEM *item, AGENT_RESULT *result)
{
	const char	*__function_name = "get_value_agent";
	zbx_socket_t	s;
	char		*tls_arg1, *tls_arg2;
	int		ret = SUCCEED;
	ssize_t		received_len;

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "In %s() host:'%s' addr:'%s' key:'%s' conn:'%s'", __function_name,
				item->host.host, item->interface.addr, item->key,
				zbx_tcp_connection_type_name(item->host.tls_connect));
	}

	switch (item->host.tls_connect)
	{
		case ZBX_TCP_SEC_UNENCRYPTED:
			tls_arg1 = NULL;
			tls_arg2 = NULL;
			break;
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
		case ZBX_TCP_SEC_TLS_CERT:
			tls_arg1 = item->host.tls_issuer;
			tls_arg2 = item->host.tls_subject;
			break;
		case ZBX_TCP_SEC_TLS_PSK:
			tls_arg1 = item->host.tls_psk_identity;
			tls_arg2 = item->host.tls_psk;
			break;
#else
		case ZBX_TCP_SEC_TLS_CERT:
		case ZBX_TCP_SEC_TLS_PSK:
			SET_MSG_RESULT(result, zbx_dsprintf(NULL, "A TLS connection is configured to be used with agent"
					" but support for TLS was not compiled into %s.",
					get_program_type_string(program_type)));
			ret = CONFIG_ERROR;
			goto out;
#endif
		default:
			THIS_SHOULD_NEVER_HAPPEN;
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid TLS connection parameters."));
			ret = CONFIG_ERROR;
			goto out;
	}

	if (SUCCEED == (ret = zbx_tcp_connect(&s, CONFIG_SOURCE_IP, item->interface.addr, item->interface.port, 0,
			item->host.tls_connect, tls_arg1, tls_arg2)))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "Sending [%s]", item->key);

		if (SUCCEED != zbx_tcp_send(&s, item->key))
			ret = NETWORK_ERROR;
		else if (FAIL != (received_len = zbx_tcp_recv_ext(&s, 0)))
			ret = SUCCEED;
		else if (SUCCEED == zbx_alarm_timed_out())
			ret = TIMEOUT_ERROR;
		else
			ret = NETWORK_ERROR;
	}
	else
		ret = NETWORK_ERROR;

	if (SUCCEED == ret)
	{
		zbx_rtrim(s.buffer, " \r\n");
		zbx_ltrim(s.buffer, " ");

		zabbix_log(LOG_LEVEL_DEBUG, "get value from agent result: '%s'", s.buffer);

		if (0 == strcmp(s.buffer, ZBX_NOTSUPPORTED))
		{
			/* 'ZBX_NOTSUPPORTED\0<error message>' */
			if (sizeof(ZBX_NOTSUPPORTED) < s.read_bytes)
				SET_MSG_RESULT(result, zbx_dsprintf(NULL, "%s", s.buffer + sizeof(ZBX_NOTSUPPORTED)));
			else
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Not supported by Zabbix Agent"));

			ret = NOTSUPPORTED;
		}
		else if (0 == strcmp(s.buffer, ZBX_ERROR))
		{
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Zabbix Agent non-critical error"));
			ret = AGENT_ERROR;
		}
		else if (0 == received_len)
		{
			SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Received empty response from Zabbix Agent at [%s]."
					" Assuming that agent dropped connection because of access permissions.",
					item->interface.addr));
			ret = NETWORK_ERROR;
		}
		else
			set_result_type(result, ITEM_VALUE_TYPE_TEXT, s.buffer);
	}
	else
		SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Get value from agent failed: %s", zbx_socket_strerror()));

	zbx_tcp_close(&s);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

#define INIT			10
#define SKIPPED			11
#define SOCKET_CREATED	12
#define CONNECT_SENT	13
#define REQ_SENT		14
#define	CLOSED			15
#define ZBX_AGENT_MAX_RESPONSE_TIME 2

//this function follows the socket status and 
//handles operations according to the socket state
void handle_socket_operation(zbx_socket_t *socket, DC_ITEM * item, int *errcode, int *conn_status, 
						 AGENT_RESULT *result, int *active_agents) 
{
	
	ZBX_SOCKADDR	servaddr_in;
	struct hostent	*hp;
	ssize_t		received_len;
	
	int status;

	switch (*conn_status) {
		case SOCKET_CREATED:
			zabbix_log(LOG_LEVEL_DEBUG,"Starting connect to item");

			if (NULL == (hp = gethostbyname(item->interface.addr)))
			{
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Cannot get hostname for the ip."));
				*conn_status=FAIL;
				*errcode=CONFIG_ERROR;
				break;	
			}
#if !defined(_WINDOWS) && !SOCK_CLOEXEC
			fcntl(socket->socket, F_SETFD, FD_CLOEXEC);
#endif
			servaddr_in.sin_family = AF_INET;
			servaddr_in.sin_addr.s_addr = ((struct in_addr *)(hp->h_addr))->s_addr;
			servaddr_in.sin_port = htons(item->interface.port);
			
			zabbix_log(LOG_LEVEL_DEBUG,"Doing connect to host %s",item->interface.addr);

			status = connect(socket->socket, (const struct sockaddr *)&servaddr_in, sizeof(servaddr_in));
			
			//async connects will return immediately with the error status and EINPROGRESS as errno
			if (ZBX_PROTO_ERROR == status && EINPROGRESS != errno )
			{	
				zabbix_log(LOG_LEVEL_DEBUG,"Connect fail");
				*conn_status = FAIL;
				*errcode = CONFIG_ERROR;
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Cannot  connect to the host"));
				break;	
			}

			*conn_status=CONNECT_SENT;
			break;

		case CONNECT_SENT:
			zabbix_log(LOG_LEVEL_DEBUG,"Sending data to the socket");

			if (SUCCEED != zbx_tcp_send(socket, item->key))
			{
				*errcode = NETWORK_ERROR;
				*conn_status = FAIL;
				SET_MSG_RESULT(result, zbx_strdup(NULL, "Cannot send request to the agent"));
				zabbix_log(LOG_LEVEL_DEBUG,"Data send fail, aborting session");
			}
			
			*conn_status=REQ_SENT;
			break;

		case REQ_SENT:
			if (FAIL != (received_len = zbx_tcp_recv_ext(socket, 0)))
			{
				*errcode = SUCCEED;
				zabbix_log(LOG_LEVEL_DEBUG, "get value from agent result: '%s'", socket->buffer);

				zbx_rtrim(socket->buffer, " \r\n");
				zbx_ltrim(socket->buffer, " ");

				if (0 == strcmp(socket->buffer, ZBX_NOTSUPPORTED))
				{
					/* 'ZBX_NOTSUPPORTED\0<error message>' */
					if (sizeof(ZBX_NOTSUPPORTED) < socket->read_bytes)
						SET_MSG_RESULT(result, zbx_dsprintf(NULL, "%s", socket->buffer + sizeof(ZBX_NOTSUPPORTED)));
					else
						SET_MSG_RESULT(result, zbx_strdup(NULL, "Not supported by Zabbix Agent"));
						*errcode = NOTSUPPORTED;
				}
				else if (0 == strcmp(socket->buffer, ZBX_ERROR))
				{
					SET_MSG_RESULT(result, zbx_strdup(NULL, "Zabbix Agent non-critical error"));
					*errcode = AGENT_ERROR;
				}
				else if (0 == received_len)
				{
					SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Received empty response from Zabbix Agent at [%s]."
						" Assuming that agent dropped connection because of access permissions.", item->interface.addr));
					*errcode = NETWORK_ERROR;
				}
				else
					set_result_type(result, ITEM_VALUE_TYPE_TEXT, socket->buffer);
			} else 
			{
					zabbix_log(LOG_LEVEL_DEBUG, "Get value from agent failed: %s", zbx_socket_strerror());
					SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Get value from agent failed: %s", zbx_socket_strerror()));
					*errcode=NETWORK_ERROR;
			}					
		
			zbx_tcp_close(socket);				
			
			socket->socket=0;
			*conn_status=FAIL;
			
			*active_agents=*active_agents-1;
			
			zabbix_log(LOG_LEVEL_DEBUG, "finished socket processing %d",*active_agents);
			break;
	} 

}


int	get_value_agent_async(DC_ITEM *items, AGENT_RESULT *results, int *errcodes, int num)
{
	const char	*__function_name = "get_value_agent_async";
	zbx_socket_t	*s;	
	char		*tls_arg1, *tls_arg2;
	int			i,	ret=SUCCEED,	max_socket;
	// connects=0;
	int 		*conn_status;
	unsigned int active_agents=0;
	unsigned int starttime;


	zabbix_log(LOG_LEVEL_DEBUG,"Started async agent polling for %d items", num);

	if (NULL == (s=zbx_malloc(NULL, num*sizeof(zbx_socket_t)))) {
		zabbix_log(LOG_LEVEL_WARNING,"Couldn't allocate memory for sockets");
		return FAIL;
	};

	memset(s, 0, num*sizeof(zbx_socket_t));
	
	if (NULL == (conn_status=zbx_malloc(NULL, num*sizeof(unsigned int))))
	{
		zabbix_log(LOG_LEVEL_WARNING,"Couldn't allocate memory for sockets");
		return FAIL;
	};

	//starting connections
	for ( i = 0; i < num; i++ ) 
	{
		conn_status[i]=INIT;
		s[i].buf_type = ZBX_BUF_TYPE_STAT;

		//cheick if the item is agent type
		if (  ITEM_TYPE_ZABBIX != items[i].type )	
		{	
			conn_status[i]=SKIPPED;
			continue;
		}
			
		zabbix_log(LOG_LEVEL_TRACE, "In %s() host:'%s' addr:'%s' key:'%s' conn:'%s'", __function_name,
				items[i].host.host, items[i].interface.addr, items[i].key,
				zbx_tcp_connection_type_name(items[i].host.tls_connect));

		switch (items[i].host.tls_connect)
		{
			case ZBX_TCP_SEC_UNENCRYPTED:
				tls_arg1 = NULL;
				tls_arg2 = NULL;
				break;
#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
			case ZBX_TCP_SEC_TLS_CERT:
				tls_arg1 = item->host.tls_issuer;
				tls_arg2 = item->host.tls_subject;
				break;
			case ZBX_TCP_SEC_TLS_PSK:
				tls_arg1 = items[i].host.tls_psk_identity;
				tls_arg2 = items[i].host.tls_psk;
				break;
#else
			case ZBX_TCP_SEC_TLS_CERT:
			case ZBX_TCP_SEC_TLS_PSK:
				SET_MSG_RESULT(&results[i], zbx_dsprintf(NULL, "A TLS connection is configured to be used with agent"
					" but support for TLS was not compiled into %s.",
					get_program_type_string(program_type)));
				conn_status[i]=SKIPPED;
				errcodes[i]=CONFIG_ERROR;
				continue;
#endif
			default:
				THIS_SHOULD_NEVER_HAPPEN;
				SET_MSG_RESULT(&results[i], zbx_strdup(NULL, "Invalid TLS connection parameters."));
				conn_status[i]=SKIPPED;
				errcodes[i]=CONFIG_ERROR;
				continue;
		}

				
		if (ZBX_SOCKET_ERROR == (s[i].socket = socket(AF_INET,  SOCK_STREAM | SOCK_NONBLOCK |  SOCK_CLOEXEC, 0)))
		{
			conn_status[i]=SKIPPED;
			errcodes[i]=CONFIG_ERROR;
			SET_MSG_RESULT(&results[i], zbx_strdup(NULL, "Couldn't create socket"));
			continue;
		}
		conn_status[i]=SOCKET_CREATED;
		max_socket=s[i].socket;
		active_agents++;
		
		//todo: binding to the source ip code
				
		handle_socket_operation(&s[i],&items[i],&errcodes[i],&conn_status[i],&results[i],&active_agents);
		
	}
			
	starttime=time(NULL);
	zabbix_log(LOG_LEVEL_DEBUG,"Starting waiting for %d sockets to connect",active_agents);

	while (active_agents>0 && (time(NULL)-starttime)< ZBX_AGENT_MAX_RESPONSE_TIME *2) 
	{
		
		//this was the simplest and compact way to implement async io
		//i has tried select() + FD_ISSET, while its seems to work
		//it requires much more CPU for large batches of hosts
		//probably it's worth of trying libevent if some problems arise, especially it's already 
		//used and linked to the daemon, but one usleep hanles all that 
		
		usleep(10000);
		
		for ( i=0; i<num; i++) 
		{
			if (SKIPPED == conn_status[i] || FAIL == conn_status[i] || CLOSED == conn_status[i]) 
				continue;

			//the socket is in connection phase, checking that it's ready to be written to
			if (CONNECT_SENT == conn_status[i]) {
				int result,ret;
				socklen_t result_len = sizeof(result);
				
				ret=getsockopt(s[i].socket, SOL_SOCKET, SO_ERROR, &result, &result_len); 
				if (ret<0)
				{
    				zabbix_log(LOG_LEVEL_DEBUG, "Connection is not ready yet %d", ret);
    				continue;
				}
				if ( 0!= result ) {
					zabbix_log(LOG_LEVEL_DEBUG, "Connection %d has failed", i);
					SET_MSG_RESULT(&results[i], zbx_strdup(NULL, "Connection to the host failed: check firewall rules and agent is running"));
					conn_status[i]==CLOSED;
					errcodes[i]=NETWORK_ERROR;
					continue;
				}
				
			} else 	if (REQ_SENT == conn_status[i] ) 
			{ 
				//checking if there are some data waiting for us in the socket
				int count;
				ioctl(s[i].socket, FIONREAD, &count);
			
				if ( 0 == count) continue;  
				
			} else {
				//self-check zabbix team syle: this should never happen :)
				THIS_SHOULD_NEVER_HAPPEN;
				continue;
			}

			handle_socket_operation(&s[i],&items[i],&errcodes[i],&conn_status[i],&results[i],&active_agents);
		}
		
	} 
	
	zabbix_log(LOG_LEVEL_DEBUG,"There are %d active connections timed-out",active_agents); 

	//closing sockets for timed out items
	for ( i = 0; i < num; i++) 
	{
		if (s[i].socket) zbx_tcp_close(&s[i]);

		if (REQ_SENT == conn_status[i] || CONNECT_SENT ==conn_status[i]) {
			zabbix_log(LOG_LEVEL_DEBUG, "Connection %d has timed out while waiting for responce", num);
			SET_MSG_RESULT(&results[i], zbx_strdup(NULL, "Waiting for responce timed out"));
			errcodes[i]=TIMEOUT_ERROR;
			continue;
		}
	}

	zbx_free(s);
	zbx_free(conn_status);
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s: %d agents, %d succesifull",__function_name, num, num-active_agents);
}

