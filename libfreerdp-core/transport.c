/**
 * FreeRDP: A Remote Desktop Protocol Client
 * Network Transport Layer
 *
 * Copyright 2011 Vic Lee
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freerdp/utils/sleep.h>
#include <freerdp/utils/stream.h>
#include <freerdp/utils/memory.h>
#include <freerdp/utils/hexdump.h>

#include <time.h>
#include <errno.h>
#include <fcntl.h>

#ifndef _WIN32
#include <netdb.h>
#include <sys/socket.h>
#endif

#include "tpkt.h"
#include "fastpath.h"
#include "credssp.h"
#include "transport.h"

#define BUFFER_SIZE 16384

STREAM* transport_recv_stream_init(rdpTransport* transport, int size)
{
	STREAM* s = transport->recv_stream;
	stream_check_size(s, size);
	stream_set_pos(s, 0);
	return s;
}

STREAM* transport_send_stream_init(rdpTransport* transport, int size)
{
	STREAM* s = transport->send_stream;
	stream_check_size(s, size);
	stream_set_pos(s, 0);
	return s;
}

tbool transport_connect(rdpTransport* transport, const char* hostname, uint16 port)
{
	return tcp_connect(transport->tcp, hostname, port);
}

void transport_attach(rdpTransport* transport, int sockfd)
{
	transport->tcp->sockfd = sockfd;
}

tbool transport_disconnect(rdpTransport* transport)
{
	if (transport->layer == TRANSPORT_LAYER_TLS)
		tls_disconnect(transport->tls);
	return tcp_disconnect(transport->tcp);
}

tbool transport_connect_rdp(rdpTransport* transport)
{
	/* RDP encryption */

	return true;
}

tbool transport_connect_tls(rdpTransport* transport)
{
	if (transport->tls == NULL)
		transport->tls = tls_new(transport->settings);

	transport->layer = TRANSPORT_LAYER_TLS;
	transport->tls->sockfd = transport->tcp->sockfd;

	if (tls_connect(transport->tls) == false)
		return false;

	return true;
}

tbool transport_connect_nla(rdpTransport* transport)
{
	if (transport->tls == NULL)
		transport->tls = tls_new(transport->settings);

	transport->layer = TRANSPORT_LAYER_TLS;
	transport->tls->sockfd = transport->tcp->sockfd;

	if (tls_connect(transport->tls) == false)
		return false;

	/* Network Level Authentication */

	if (transport->settings->authentication == false)
		return true;

	if (transport->credssp == NULL)
		transport->credssp = credssp_new(transport);

	if (credssp_authenticate(transport->credssp) < 0)
	{
		printf("Authentication failure, check credentials.\n"
			"If credentials are valid, the NTLMSSP implementation may be to blame.\n");

		credssp_free(transport->credssp);
		return false;
	}

	credssp_free(transport->credssp);

	return true;
}

tbool transport_accept_rdp(rdpTransport* transport)
{
	/* RDP encryption */

	return true;
}

tbool transport_accept_tls(rdpTransport* transport)
{
	if (transport->tls == NULL)
		transport->tls = tls_new(transport->settings);

	transport->layer = TRANSPORT_LAYER_TLS;
	transport->tls->sockfd = transport->tcp->sockfd;

	if (tls_accept(transport->tls, transport->settings->cert_file, transport->settings->privatekey_file) == false)
		return false;

	return true;
}

tbool transport_accept_nla(rdpTransport* transport)
{
	if (transport->tls == NULL)
		transport->tls = tls_new(transport->settings);

	transport->layer = TRANSPORT_LAYER_TLS;
	transport->tls->sockfd = transport->tcp->sockfd;

	if (tls_accept(transport->tls, transport->settings->cert_file, transport->settings->privatekey_file) == false)
		return false;

	/* Network Level Authentication */

	if (transport->settings->authentication == false)
		return true;

	/* Blocking here until NLA is complete */

	return true;
}

int transport_read_layer(rdpTransport* transport, uint8* data, int bytes)
{
	int read = 0;
	int status = -1;

	while (read < bytes)
	{
		if (transport->layer == TRANSPORT_LAYER_TLS)
			status = tls_read(transport->tls, data + read, bytes - read);
		else if (transport->layer == TRANSPORT_LAYER_TCP)
			status = tcp_read(transport->tcp, data + read, bytes - read);
		//else if (transport->layer == TRANSPORT_LAYER_TSG)
		//	status = tsg_read(transport->tsg, data + read, bytes - read);

		/* blocking means that we can't continue until this is read
		   it's not tcp blocking */

		if (transport->blocking == false)
			return status;

		if (status < 0)
			return status;

		read += status;

		if (status == 0)
		{
			if (transport->layer == TRANSPORT_LAYER_TLS)
				tcp_can_recv(transport->tls->sockfd, 100);
			else if (transport->layer == TRANSPORT_LAYER_TCP)
				tcp_can_recv(transport->tcp->sockfd, 100);
			else
				freerdp_usleep(transport->usleep_interval);
		}
	}

	return read;
}

int transport_read(rdpTransport* transport, STREAM* s)
{
	int status;
	int pdu_bytes;
	int stream_bytes;
	int transport_status;

	transport_status = 0;

	/* first check if we have header */
	stream_bytes = stream_get_length(s);

	if (stream_bytes < 4)
	{
		status = transport_read_layer(transport, s->data + stream_bytes,
				4 - stream_bytes);

		if (status < 0)
			return status;

		transport_status += status;

		if ((status + stream_bytes) < 4)
			return transport_status;

		stream_bytes += status;
	}

	pdu_bytes = 0;
	/* if header is present, read in exactly one PDU */
	if (s->data[0] == 0x03)
	{
		/* TPKT header */
		pdu_bytes = (s->data[2] << 8) | s->data[3];
	}
	else if (s->data[0] == 0x30)
	{
		/* TSRequest (NLA) */
		if (s->data[1] & 0x80)
		{
			if ((s->data[1] & ~(0x80)) == 1)
			{
				pdu_bytes = s->data[2];
				pdu_bytes += 3;
			}
			else if ((s->data[1] & ~(0x80)) == 2)
			{
				pdu_bytes = (s->data[2] << 8) | s->data[3];
				pdu_bytes += 4;
			}
			else
			{
				printf("Error reading TSRequest!\n");
			}
		}
		else
		{
			pdu_bytes = s->data[1];
			pdu_bytes += 2;
		}
	}
	else
	{
		/* Fast-Path Header */
		if (s->data[1] & 0x80)
			pdu_bytes = ((s->data[1] & 0x7f) << 8) | s->data[2];
		else
			pdu_bytes = s->data[1];
	}

	status = transport_read_layer(transport, s->data + stream_bytes,
			pdu_bytes - stream_bytes);

	if (status < 0)
		return status;

	transport_status += status;

#ifdef WITH_DEBUG_TRANSPORT
	/* dump when whole PDU is read */
	if (stream_bytes + status >= pdu_bytes)
	{
		printf("Local < Remote\n");
		freerdp_hexdump(s->data, pdu_bytes);
	}
#endif

	return transport_status;
}

static int transport_read_nonblocking(rdpTransport* transport)
{
	int status;

	stream_check_size(transport->recv_buffer, 32 * 1024);
	status = transport_read(transport, transport->recv_buffer);

	if (status <= 0)
		return status;

	stream_seek(transport->recv_buffer, status);

	return status;
}

int transport_write(rdpTransport* transport, STREAM* s)
{
	int status = -1;
	int length;

	length = stream_get_length(s);
	stream_set_pos(s, 0);

#ifdef WITH_DEBUG_TRANSPORT
	if (length > 0)
	{
		printf("Local > Remote\n");
		freerdp_hexdump(s->data, length);
	}
#endif

	while (length > 0)
	{
		if (transport->layer == TRANSPORT_LAYER_TLS)
			status = tls_write(transport->tls, stream_get_tail(s), length);
		else if (transport->layer == TRANSPORT_LAYER_TCP)
			status = tcp_write(transport->tcp, stream_get_tail(s), length);

		if (status < 0)
			break; /* error occurred */

		if (status == 0)
		{
			/* blocking while sending */
			freerdp_usleep(transport->usleep_interval);

		}

		length -= status;
		stream_seek(s, status);
	}

	if (status < 0)
	{
		/* A write error indicates that the peer has dropped the connection */
		transport->layer = TRANSPORT_LAYER_CLOSED;
	}

	return status;
}

void transport_get_fds(rdpTransport* transport, void** rfds, int* rcount)
{
	rfds[*rcount] = (void*)(long)(transport->tcp->sockfd);
	(*rcount)++;
}

int transport_check_fds(rdpTransport* transport)
{
	int pos;
	int status;
	uint16 length;

	if (transport->level != 0)
	{
		printf("transport_check_fds: error, nested calls\n");
		return -1;
	}

	status = transport_read_nonblocking(transport);

	if (status < 0)
		return status;

	if ((pos = stream_get_pos(transport->recv_buffer)) > 0)
	{
		stream_set_pos(transport->recv_buffer, 0);
		if (tpkt_verify_header(transport->recv_buffer)) /* TPKT */
		{
			/* Ensure the TPKT header is available. */
			if (pos <= 4)
			{
				stream_set_pos(transport->recv_buffer, pos);
				return 0;
			}
			length = tpkt_read_header(transport->recv_buffer);
		}
		else /* Fast Path */
		{
			/* Ensure the Fast Path header is available. */
			if (pos <= 2)
			{
				stream_set_pos(transport->recv_buffer, pos);
				return 0;
			}
			/* Fastpath header can be two or three bytes long. */
			length = fastpath_header_length(transport->recv_buffer);
			if (pos < length)
			{
				stream_set_pos(transport->recv_buffer, pos);
				return 0;
			}
			length = fastpath_read_header(NULL, transport->recv_buffer);
		}

		if (length == 0)
		{
			printf("transport_check_fds: protocol error, not a TPKT or Fast Path header.\n");
			freerdp_hexdump(stream_get_head(transport->recv_buffer), pos);
			return -1;
		}

		if (pos < length)
		{
			stream_set_pos(transport->recv_buffer, pos);
			return 0; /* Packet is not yet completely received. */
		}

		stream_set_pos(transport->recv_buffer, length);
		stream_seal(transport->recv_buffer);
		stream_set_pos(transport->recv_buffer, 0);

		transport->level++;
		if (transport->recv_callback(transport, transport->recv_buffer, transport->recv_extra) == false)
			status = -1;
		transport->level--;

		stream_set_pos(transport->recv_buffer, 0);

		if (status < 0)
			return status;
	}

	return 0;
}

tbool transport_set_blocking_mode(rdpTransport* transport, tbool blocking)
{
	transport->blocking = blocking;
	return tcp_set_blocking_mode(transport->tcp, blocking);
}

rdpTransport* transport_new(rdpSettings* settings)
{
	rdpTransport* transport;

	transport = (rdpTransport*) xzalloc(sizeof(rdpTransport));

	if (transport != NULL)
	{
		transport->tcp = tcp_new(settings);
		transport->settings = settings;

		/* a small 0.1ms delay when transport is blocking. */
		transport->usleep_interval = 100;

		/* receive buffer for non-blocking read. */
		transport->recv_buffer = stream_new(BUFFER_SIZE);

		/* buffers for blocking read/write */
		transport->recv_stream = stream_new(BUFFER_SIZE);
		transport->send_stream = stream_new(BUFFER_SIZE);

		transport->blocking = true;

		transport->layer = TRANSPORT_LAYER_TCP;
	}

	return transport;
}

void transport_free(rdpTransport* transport)
{
	if (transport != NULL)
	{
		stream_free(transport->recv_buffer);
		stream_free(transport->recv_stream);
		stream_free(transport->send_stream);
		if (transport->tls)
			tls_free(transport->tls);
		tcp_free(transport->tcp);
		xfree(transport);
	}
}
