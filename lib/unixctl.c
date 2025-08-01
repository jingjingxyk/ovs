/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2014, 2016 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "unixctl.h"
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include "command-line.h"
#include "coverage.h"
#include "dirs.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/json.h"
#include "jsonrpc.h"
#include "openvswitch/list.h"
#include "openvswitch/poll-loop.h"
#include "openvswitch/shash.h"
#include "stream.h"
#include "stream-provider.h"
#include "svec.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(unixctl);

COVERAGE_DEFINE(unixctl_received);
COVERAGE_DEFINE(unixctl_replied);

struct unixctl_command {
    const char *usage;
    int min_args, max_args;
    unixctl_cb_func *cb;
    void *aux;
};

struct unixctl_conn {
    struct ovs_list node;
    struct jsonrpc *rpc;

    /* Only one request can be in progress at a time.  While the request is
     * being processed, 'request_id' is populated, otherwise it is null. */
    struct json *request_id;   /* ID of the currently active request. */

    enum unixctl_output_fmt fmt; /* Output format of current connection. */
};

/* Server for control connection. */
struct unixctl_server {
    struct pstream *listener;
    struct ovs_list conns;
    char *path;
};

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 5);

static struct shash commands = SHASH_INITIALIZER(&commands);

const char *
unixctl_output_fmt_to_string(enum unixctl_output_fmt fmt)
{
    switch (fmt) {
    case UNIXCTL_OUTPUT_FMT_TEXT: return "text";
    case UNIXCTL_OUTPUT_FMT_JSON: return "json";
    default: return "<unknown>";
    }
}

bool
unixctl_output_fmt_from_string(const char *string,
                               enum unixctl_output_fmt *fmt)
{
    if (!strcasecmp(string, "text")) {
        *fmt = UNIXCTL_OUTPUT_FMT_TEXT;
    } else if (!strcasecmp(string, "json")) {
        *fmt = UNIXCTL_OUTPUT_FMT_JSON;
    } else {
        return false;
    }
    return true;
}

static void
unixctl_list_commands(struct unixctl_conn *conn, int argc OVS_UNUSED,
                      const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    if (unixctl_command_get_output_format(conn) == UNIXCTL_OUTPUT_FMT_JSON) {
        struct json *json_commands = json_object_create();
        const struct shash_node *node;

        SHASH_FOR_EACH (node, &commands) {
            const struct unixctl_command *command = node->data;

            if (command->usage) {
                json_object_put_string(json_commands, node->name,
                                       command->usage);
            }
        }
        unixctl_command_reply_json(conn, json_commands);
    } else {
        struct ds ds = DS_EMPTY_INITIALIZER;
        const struct shash_node **nodes = shash_sort(&commands);
        size_t i;

        ds_put_cstr(&ds, "The available commands are:\n");

        for (i = 0; i < shash_count(&commands); ++i) {
            const struct shash_node *node = nodes[i];
            const struct unixctl_command *command = node->data;

            if (command->usage) {
                ds_put_format(&ds, "  %-23s %s\n", node->name,
                              command->usage);
            }
        }
        free(nodes);

        unixctl_command_reply(conn, ds_cstr(&ds));
        ds_destroy(&ds);
    }
}

static void
unixctl_version(struct unixctl_conn *conn, int argc OVS_UNUSED,
                const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    unixctl_command_reply(conn, ovs_get_program_version());
}

static void
unixctl_set_options(struct unixctl_conn *conn, int argc, const char *argv[],
                    void *aux OVS_UNUSED)
{
    struct ovs_cmdl_parsed_option *parsed_options = NULL;
    size_t n_parsed_options;
    char *error = NULL;

    static const struct option options[] = {
        {"format", required_argument, NULL, 'f'},
        {NULL, 0, NULL, 0},
    };

    error = ovs_cmdl_parse_all(argc--, (char **) (argv++), options,
                               &parsed_options, &n_parsed_options);
    if (error) {
        goto error;
    }

    for (size_t i = 0; i < n_parsed_options; i++) {
        struct ovs_cmdl_parsed_option *parsed_option = &parsed_options[i];

        switch (parsed_option->o->val) {
        case 'f':
            if (!unixctl_output_fmt_from_string(parsed_option->arg,
                                                &conn->fmt)) {
                error = xasprintf("option format has invalid value %s",
                                  parsed_option->arg);
                goto error;
            }
            break;

        default:
            OVS_NOT_REACHED();
        }
    }

    unixctl_command_reply(conn, NULL);
    free(parsed_options);
    return;
error:
    unixctl_command_reply_error(conn, error);
    free(error);
    free(parsed_options);
}

/* Registers a unixctl command with the given 'name'.  'usage' describes the
 * arguments to the command; it is used only for presentation to the user in
 * "list-commands" output.  (If 'usage' is NULL, then the command is hidden.)
 *
 * 'cb' is called when the command is received.  It is passed an array
 * containing the command name and arguments, plus a copy of 'aux'.  Normally
 * 'cb' should reply by calling unixctl_command_reply() or
 * unixctl_command_reply_error() before it returns, but if the command cannot
 * be handled immediately then it can defer the reply until later.  A given
 * connection can only process a single request at a time, so a reply must be
 * made eventually to avoid blocking that connection. */
void
unixctl_command_register(const char *name, const char *usage,
                         int min_args, int max_args,
                         unixctl_cb_func *cb, void *aux)
{
    struct unixctl_command *command;
    struct unixctl_command *lookup = shash_find_data(&commands, name);

    ovs_assert(!lookup || lookup->cb == cb);

    if (lookup) {
        return;
    }

    command = xmalloc(sizeof *command);
    command->usage = usage;
    command->min_args = min_args;
    command->max_args = max_args;
    command->cb = cb;
    command->aux = aux;
    shash_add(&commands, name, command);
}

enum unixctl_output_fmt
unixctl_command_get_output_format(struct unixctl_conn *conn)
{
    return conn->fmt;
}

/* Takes ownership of the 'body'. */
static void
unixctl_command_reply__(struct unixctl_conn *conn,
                        bool success, struct json *body)
{
    struct jsonrpc_msg *reply;

    COVERAGE_INC(unixctl_replied);
    ovs_assert(conn->request_id);

    if (success) {
        reply = jsonrpc_create_reply(body, conn->request_id);
    } else {
        reply = jsonrpc_create_error(body, conn->request_id);
    }

    if (VLOG_IS_DBG_ENABLED()) {
        char *id = json_to_string(conn->request_id, 0);
        char *msg = json_to_string(body, JSSF_SORT);

        VLOG_DBG("replying with %s, id=%s: \"%s\"",
                 success ? "success" : "error", id, msg);
        free(msg);
        free(id);
    }

    /* If jsonrpc_send() returns an error, the run loop will take care of the
     * problem eventually. */
    jsonrpc_send(conn->rpc, reply);
    json_destroy(conn->request_id);
    conn->request_id = NULL;
}

/* Replies to the active unixctl connection 'conn'.  'result' is sent to the
 * client indicating the command was processed successfully.  'result' should
 * be plain-text; use unixctl_command_reply_json() to return a JSON document
 * when JSON output has been requested.  Only one call to
 * unixctl_command_reply*() functions may be made per request. */
void
unixctl_command_reply(struct unixctl_conn *conn, const char *result)
{
    struct json *json_result = json_string_create(result ? result : "");

    if (conn->fmt == UNIXCTL_OUTPUT_FMT_JSON) {
        /* Wrap plain-text reply in provisional JSON document when JSON output
         * has been requested. */
        struct json *json_reply = json_object_create();

        json_object_put_string(json_reply, "reply-format", "plain");
        json_object_put(json_reply, "reply", json_result);

        json_result = json_reply;
    }

    unixctl_command_reply__(conn, true, json_result);
}

/* Replies to the active unixctl connection 'conn'.  'body' is sent to the
 * client indicating the command was processed successfully.  Use this function
 * when JSON output has been requested; otherwise use unixctl_command_reply()
 * for plain-text output.  Only one call to unixctl_command_reply*() functions
 * may be made per request.
 *
 * Takes ownership of the 'body'. */
void
unixctl_command_reply_json(struct unixctl_conn *conn, struct json *body)
{
    ovs_assert(conn->fmt == UNIXCTL_OUTPUT_FMT_JSON);
    unixctl_command_reply__(conn, true, body);
}

/* Replies to the active unixctl connection 'conn'. 'error' is sent to the
 * client indicating an error occurred processing the command.  'error' should
 * be plain-text.  Only one call to unixctl_command_reply*() functions may be
 * made per request. */
void
unixctl_command_reply_error(struct unixctl_conn *conn, const char *error)
{
    unixctl_command_reply__(conn, false,
                            json_string_create(error ? error : ""));
}

/* Creates a unixctl server listening on 'path', which for POSIX may be:
 *
 *      - NULL, in which case <rundir>/<program>.<pid>.ctl is used.
 *
 *      - A name that does not start with '/', in which case it is put in
 *        <rundir>.
 *
 *      - An absolute path (starting with '/') that gives the exact name of
 *        the Unix domain socket to listen on.
 *
 * For Windows, a local named pipe is used. A file is created in 'path'
 * which may be:
 *
 *      - NULL, in which case <rundir>/<program>.ctl is used.
 *
 *      - An absolute path that gives the name of the file.
 *
 * For both POSIX and Windows, if the path is "none", the function will
 * return successfully but no socket will actually be created.
 *
 * A program that (optionally) daemonizes itself should call this function
 * *after* daemonization, so that the socket name contains the pid of the
 * daemon instead of the pid of the program that exited.  (Otherwise,
 * "ovs-appctl --target=<program>" will fail.)
 *
 * Returns 0 if successful, otherwise a positive errno value.  If successful,
 * sets '*serverp' to the new unixctl_server (or to NULL if 'path' was "none"),
 * otherwise to NULL. */
int
unixctl_server_create(const char *path, struct unixctl_server **serverp)
{
    *serverp = NULL;
    if (path && !strcmp(path, "none")) {
        return 0;
    }

#ifdef _WIN32
    enum { WINDOWS = 1 };
#else
    enum { WINDOWS = 0 };
#endif

    long int pid = getpid();
    char *abs_path
        = (path ? abs_file_name(ovs_rundir(), path)
           : WINDOWS ? xasprintf("%s/%s.ctl", ovs_rundir(), program_name)
           : xasprintf("%s/%s.%ld.ctl", ovs_rundir(), program_name, pid));

    struct pstream *listener;
    char *punix_path = xasprintf("punix:%s", abs_path);
    int error = pstream_open(punix_path, &listener, 0);
    free(punix_path);

    if (error) {
        ovs_error(error, "%s: could not initialize control socket", abs_path);
        free(abs_path);
        return error;
    }

    unixctl_command_register("list-commands", "", 0, 0, unixctl_list_commands,
                             NULL);
    unixctl_command_register("version", "", 0, 0, unixctl_version, NULL);
    unixctl_command_register("set-options", "[--format text|json]", 1, 2,
                             unixctl_set_options, NULL);

    struct unixctl_server *server = xmalloc(sizeof *server);
    server->listener = listener;
    server->path = abs_path;
    ovs_list_init(&server->conns);
    *serverp = server;
    return 0;
}

static void
process_command(struct unixctl_conn *conn, struct jsonrpc_msg *request)
{
    char *error = NULL;

    struct unixctl_command *command;
    const struct json *params;

    COVERAGE_INC(unixctl_received);
    conn->request_id = json_clone(request->id);

    if (VLOG_IS_DBG_ENABLED()) {
        char *params_s = json_to_string(request->params, 0);
        char *id_s = json_to_string(request->id, 0);
        VLOG_DBG("received request %s%s, id=%s",
                 request->method, params_s, id_s);
        free(params_s);
        free(id_s);
    }

    params = request->params;
    command = shash_find_data(&commands, request->method);
    if (!command) {
        error = xasprintf("\"%s\" is not a valid command (use "
                          "\"list-commands\" to see a list of valid commands)",
                          request->method);
    } else if (json_array_size(params) < command->min_args) {
        error = xasprintf("\"%s\" command requires at least %d arguments",
                          request->method, command->min_args);
    } else if (json_array_size(params) > command->max_args) {
        error = xasprintf("\"%s\" command takes at most %d arguments",
                          request->method, command->max_args);
    } else {
        struct svec argv = SVEC_EMPTY_INITIALIZER;
        int  i, n = json_array_size(params);

        svec_add(&argv, request->method);
        for (i = 0; i < n; i++) {
            const struct json *elem = json_array_at(params, i);

            if (elem->type != JSON_STRING) {
                error = xasprintf("\"%s\" command has non-string argument",
                                  request->method);
                break;
            }
            svec_add(&argv, json_string(elem));
        }
        svec_terminate(&argv);

        if (!error) {
            command->cb(conn, argv.n, (const char **) argv.names,
                        command->aux);
        }

        svec_destroy(&argv);
    }

    if (error) {
        unixctl_command_reply_error(conn, error);
        free(error);
    }
}

static int
run_connection(struct unixctl_conn *conn)
{
    int error, i;

    jsonrpc_run(conn->rpc);
    error = jsonrpc_get_status(conn->rpc);
    if (error || jsonrpc_get_backlog(conn->rpc)) {
        return error;
    }

    for (i = 0; i < 10; i++) {
        struct jsonrpc_msg *msg;

        if (error || conn->request_id) {
            break;
        }

        jsonrpc_recv(conn->rpc, &msg);
        if (msg) {
            if (msg->type == JSONRPC_REQUEST) {
                process_command(conn, msg);
            } else {
                VLOG_WARN_RL(&rl, "%s: received unexpected %s message",
                             jsonrpc_get_name(conn->rpc),
                             jsonrpc_msg_type_to_string(msg->type));
                error = EINVAL;
            }
            jsonrpc_msg_destroy(msg);
        }
        error = error ? error : jsonrpc_get_status(conn->rpc);
    }

    return error;
}

static void
kill_connection(struct unixctl_conn *conn)
{
    ovs_list_remove(&conn->node);
    jsonrpc_close(conn->rpc);
    json_destroy(conn->request_id);
    free(conn);
}

void
unixctl_server_run(struct unixctl_server *server)
{
    if (!server) {
        return;
    }

    for (int i = 0; i < 10; i++) {
        struct stream *stream;
        int error;

        error = pstream_accept(server->listener, &stream);
        if (!error) {
            struct unixctl_conn *conn = xzalloc(sizeof *conn);
            ovs_list_push_back(&server->conns, &conn->node);
            conn->rpc = jsonrpc_open(stream);
            conn->fmt = UNIXCTL_OUTPUT_FMT_TEXT;
        } else if (error == EAGAIN) {
            break;
        } else {
            VLOG_WARN_RL(&rl, "%s: accept failed: %s",
                         pstream_get_name(server->listener),
                         ovs_strerror(error));
        }
    }

    struct unixctl_conn *conn;
    LIST_FOR_EACH_SAFE (conn, node, &server->conns) {
        int error = run_connection(conn);
        if (error && error != EAGAIN) {
            kill_connection(conn);
        }
    }
}

void
unixctl_server_wait(struct unixctl_server *server)
{
    struct unixctl_conn *conn;

    if (!server) {
        return;
    }

    pstream_wait(server->listener);
    LIST_FOR_EACH (conn, node, &server->conns) {
        jsonrpc_wait(conn->rpc);
        if (!jsonrpc_get_backlog(conn->rpc) && !conn->request_id) {
            jsonrpc_recv_wait(conn->rpc);
        }
    }
}

/* Destroys 'server' and stops listening for connections. */
void
unixctl_server_destroy(struct unixctl_server *server)
{
    if (server) {
        struct unixctl_conn *conn;

        LIST_FOR_EACH_SAFE (conn, node, &server->conns) {
            kill_connection(conn);
        }

        free(server->path);
        pstream_close(server->listener);
        free(server);
    }
}

const char *
unixctl_server_get_path(const struct unixctl_server *server)
{
    return server ? server->path : NULL;
}

/* On POSIX based systems, connects to a unixctl server socket.  'path' should
 * be the name of a unixctl server socket.  If it does not start with '/', it
 * will be prefixed with the rundir (e.g. /usr/local/var/run/openvswitch).
 *
 * On Windows, connects to a local named pipe. A file which resides in
 * 'path' is used to mimic the behavior of a Unix domain socket.
 * 'path' should be an absolute path of the file.
 *
 * Returns 0 if successful, otherwise a positive errno value.  If successful,
 * sets '*client' to the new jsonrpc, otherwise to NULL. */
int
unixctl_client_create(const char *path, struct jsonrpc **client)
{
    struct stream *stream;
    int error;

    char *abs_path = abs_file_name(ovs_rundir(), path);
    char *unix_path = xasprintf("unix:%s", abs_path);

    *client = NULL;

    error = stream_open_block(stream_open(unix_path, &stream, DSCP_DEFAULT),
                              -1, &stream);
    free(unix_path);
    free(abs_path);

    if (error) {
        VLOG_WARN("failed to connect to %s", path);
        return error;
    }

    *client = jsonrpc_open(stream);
    return 0;
}

/* Executes 'command' on the server with an argument vector 'argv' containing
 * 'argc' elements.  If successfully communicated with the server, returns 0
 * and sets '*result', or '*err' (not both) to the result or error the server
 * returned.  Otherwise, sets '*result' and '*err' to NULL and returns a
 * positive errno value.  The caller is responsible for freeing '*result' or
 * '*err' if not NULL. */
int
unixctl_client_transact(struct jsonrpc *client, const char *command, int argc,
                        char *argv[], struct json **result, struct json **err)
{
    struct jsonrpc_msg *request, *reply;
    struct json **json_args, *params;
    int error, i;

    *result = NULL;
    *err = NULL;

    json_args = xmalloc(argc * sizeof *json_args);
    for (i = 0; i < argc; i++) {
        json_args[i] = json_string_create(argv[i]);
    }
    params = json_array_create(json_args, argc);
    request = jsonrpc_create_request(command, params, NULL);

    error = jsonrpc_transact_block(client, request, &reply);
    if (error) {
        VLOG_WARN("error communicating with %s: %s", jsonrpc_get_name(client),
                  ovs_retval_to_string(error));
        return error;
    }

    if (reply->result && reply->error) {
        VLOG_WARN("unexpected response when communicating with %s: %s\n %s",
                  jsonrpc_get_name(client),
                  json_to_string(reply->result, JSSF_SORT),
                  json_to_string(reply->error, JSSF_SORT));
        error = EINVAL;
    } else {
        *result = json_nullable_clone(reply->result);
        *err = json_nullable_clone(reply->error);
    }

    jsonrpc_msg_destroy(reply);
    return error;
}
