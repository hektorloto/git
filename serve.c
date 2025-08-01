#include "git-compat-util.h"
#include "repository.h"
#include "config.h"
#include "hash.h"
#include "pkt-line.h"
#include "version.h"
#include "ls-refs.h"
#include "protocol-caps.h"
#include "serve.h"
#include "upload-pack.h"
#include "bundle-uri.h"
#include "trace2.h"
#include "promisor-remote.h"

static int advertise_sid = -1;
static int advertise_object_info = -1;
static int client_hash_algo = GIT_HASH_SHA1_LEGACY;

static int always_advertise(struct repository *r UNUSED,
			    struct strbuf *value UNUSED)
{
	return 1;
}

static int agent_advertise(struct repository *r UNUSED,
			   struct strbuf *value)
{
	if (value)
		strbuf_addstr(value, git_user_agent_sanitized());
	return 1;
}

static int promisor_remote_advertise(struct repository *r,
				     struct strbuf *value)
{
	if (value) {
		char *info = promisor_remote_info(r);
		if (!info)
			return 0;
		strbuf_addstr(value, info);
		free(info);
	}
	return 1;
}

static void promisor_remote_receive(struct repository *r,
				    const char *remotes)
{
	mark_promisor_remotes_as_accepted(r, remotes);
}


static int object_format_advertise(struct repository *r,
				   struct strbuf *value)
{
	if (value)
		strbuf_addstr(value, r->hash_algo->name);
	return 1;
}

static void object_format_receive(struct repository *r UNUSED,
				  const char *algo_name)
{
	if (!algo_name)
		die("object-format capability requires an argument");

	client_hash_algo = hash_algo_by_name(algo_name);
	if (client_hash_algo == GIT_HASH_UNKNOWN)
		die("unknown object format '%s'", algo_name);
}

static int session_id_advertise(struct repository *r, struct strbuf *value)
{
	if (advertise_sid == -1 &&
	    repo_config_get_bool(r, "transfer.advertisesid", &advertise_sid))
		advertise_sid = 0;
	if (!advertise_sid)
		return 0;
	if (value)
		strbuf_addstr(value, trace2_session_id());
	return 1;
}

static void session_id_receive(struct repository *r UNUSED,
			       const char *client_sid)
{
	if (!client_sid)
		client_sid = "";
	trace2_data_string("transfer", NULL, "client-sid", client_sid);
}

static int object_info_advertise(struct repository *r, struct strbuf *value UNUSED)
{
	if (advertise_object_info == -1 &&
	    repo_config_get_bool(r, "transfer.advertiseobjectinfo",
				 &advertise_object_info)) {
		/* disabled by default */
		advertise_object_info = 0;
	}
	return advertise_object_info;
}

struct protocol_capability {
	/*
	 * The name of the capability.  The server uses this name when
	 * advertising this capability, and the client uses this name to
	 * specify this capability.
	 */
	const char *name;

	/*
	 * Function queried to see if a capability should be advertised.
	 * Optionally a value can be specified by adding it to 'value'.
	 * If a value is added to 'value', the server will advertise this
	 * capability as "<name>=<value>" instead of "<name>".
	 */
	int (*advertise)(struct repository *r, struct strbuf *value);

	/*
	 * Function called when a client requests the capability as a command.
	 * Will be provided a struct packet_reader 'request' which it should
	 * use to read the command specific part of the request.  Every command
	 * MUST read until a flush packet is seen before sending a response.
	 *
	 * This field should be NULL for capabilities which are not commands.
	 */
	int (*command)(struct repository *r, struct packet_reader *request);

	/*
	 * Function called when a client requests the capability as a
	 * non-command. This may be NULL if the capability does nothing.
	 *
	 * For a capability of the form "foo=bar", the value string points to
	 * the content after the "=" (i.e., "bar"). For simple capabilities
	 * (just "foo"), it is NULL.
	 */
	void (*receive)(struct repository *r, const char *value);
};

static struct protocol_capability capabilities[] = {
	{
		.name = "agent",
		.advertise = agent_advertise,
	},
	{
		.name = "ls-refs",
		.advertise = ls_refs_advertise,
		.command = ls_refs,
	},
	{
		.name = "fetch",
		.advertise = upload_pack_advertise,
		.command = upload_pack_v2,
	},
	{
		.name = "server-option",
		.advertise = always_advertise,
	},
	{
		.name = "object-format",
		.advertise = object_format_advertise,
		.receive = object_format_receive,
	},
	{
		.name = "session-id",
		.advertise = session_id_advertise,
		.receive = session_id_receive,
	},
	{
		.name = "object-info",
		.advertise = object_info_advertise,
		.command = cap_object_info,
	},
	{
		.name = "bundle-uri",
		.advertise = bundle_uri_advertise,
		.command = bundle_uri_command,
	},
	{
		.name = "promisor-remote",
		.advertise = promisor_remote_advertise,
		.receive = promisor_remote_receive,
	},
};

void protocol_v2_advertise_capabilities(struct repository *r)
{
	struct strbuf capability = STRBUF_INIT;
	struct strbuf value = STRBUF_INIT;

	/* serve by default supports v2 */
	packet_write_fmt(1, "version 2\n");

	for (size_t i = 0; i < ARRAY_SIZE(capabilities); i++) {
		struct protocol_capability *c = &capabilities[i];

		if (c->advertise(r, &value)) {
			strbuf_addstr(&capability, c->name);

			if (value.len) {
				strbuf_addch(&capability, '=');
				strbuf_addbuf(&capability, &value);
			}

			strbuf_addch(&capability, '\n');
			packet_write(1, capability.buf, capability.len);
		}

		strbuf_reset(&capability);
		strbuf_reset(&value);
	}

	packet_flush(1);
	strbuf_release(&capability);
	strbuf_release(&value);
}

static struct protocol_capability *get_capability(const char *key, const char **value)
{
	if (!key)
		return NULL;

	for (size_t i = 0; i < ARRAY_SIZE(capabilities); i++) {
		struct protocol_capability *c = &capabilities[i];
		const char *out;
		if (!skip_prefix(key, c->name, &out))
			continue;
		if (!*out) {
			*value = NULL;
			return c;
		}
		if (*out++ == '=') {
			*value = out;
			return c;
		}
	}

	return NULL;
}

static int receive_client_capability(struct repository *r, const char *key)
{
	const char *value;
	const struct protocol_capability *c = get_capability(key, &value);

	if (!c || c->command || !c->advertise(r, NULL))
		return 0;

	if (c->receive)
		c->receive(r, value);
	return 1;
}

static int parse_command(struct repository *r, const char *key, struct protocol_capability **command)
{
	const char *out;

	if (skip_prefix(key, "command=", &out)) {
		const char *value;
		struct protocol_capability *cmd = get_capability(out, &value);

		if (*command)
			die("command '%s' requested after already requesting command '%s'",
			    out, (*command)->name);
		if (!cmd || !cmd->advertise(r, NULL) || !cmd->command || value)
			die("invalid command '%s'", out);

		*command = cmd;
		return 1;
	}

	return 0;
}

enum request_state {
	PROCESS_REQUEST_KEYS,
	PROCESS_REQUEST_DONE,
};

static int process_request(struct repository *r)
{
	enum request_state state = PROCESS_REQUEST_KEYS;
	struct packet_reader reader;
	int seen_capability_or_command = 0;
	struct protocol_capability *command = NULL;

	packet_reader_init(&reader, 0, NULL, 0,
			   PACKET_READ_CHOMP_NEWLINE |
			   PACKET_READ_GENTLE_ON_EOF |
			   PACKET_READ_DIE_ON_ERR_PACKET);

	/*
	 * Check to see if the client closed their end before sending another
	 * request.  If so we can terminate the connection.
	 */
	if (packet_reader_peek(&reader) == PACKET_READ_EOF)
		return 1;
	reader.options &= ~PACKET_READ_GENTLE_ON_EOF;

	while (state != PROCESS_REQUEST_DONE) {
		switch (packet_reader_peek(&reader)) {
		case PACKET_READ_EOF:
			BUG("Should have already died when seeing EOF");
		case PACKET_READ_NORMAL:
			if (parse_command(r, reader.line, &command) ||
			    receive_client_capability(r, reader.line))
				seen_capability_or_command = 1;
			else
				die("unknown capability '%s'", reader.line);

			/* Consume the peeked line */
			packet_reader_read(&reader);
			break;
		case PACKET_READ_FLUSH:
			/*
			 * If no command and no keys were given then the client
			 * wanted to terminate the connection.
			 */
			if (!seen_capability_or_command)
				return 1;

			/*
			 * The flush packet isn't consume here like it is in
			 * the other parts of this switch statement.  This is
			 * so that the command can read the flush packet and
			 * see the end of the request in the same way it would
			 * if command specific arguments were provided after a
			 * delim packet.
			 */
			state = PROCESS_REQUEST_DONE;
			break;
		case PACKET_READ_DELIM:
			/* Consume the peeked line */
			packet_reader_read(&reader);

			state = PROCESS_REQUEST_DONE;
			break;
		case PACKET_READ_RESPONSE_END:
			BUG("unexpected response end packet");
		}
	}

	if (!command)
		die("no command requested");

	if (client_hash_algo != hash_algo_by_ptr(r->hash_algo))
		die("mismatched object format: server %s; client %s",
		    r->hash_algo->name,
		    hash_algos[client_hash_algo].name);

	command->command(r, &reader);

	return 0;
}

void protocol_v2_serve_loop(struct repository *r, int stateless_rpc)
{
	if (!stateless_rpc)
		protocol_v2_advertise_capabilities(r);

	/*
	 * If stateless-rpc was requested then exit after
	 * a single request/response exchange
	 */
	if (stateless_rpc) {
		process_request(r);
	} else {
		for (;;)
			if (process_request(r))
				break;
	}
}
