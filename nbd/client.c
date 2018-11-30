/*
 *  Copyright (C) 2016-2018 Red Hat, Inc.
 *  Copyright (C) 2005  Anthony Liguori <anthony@codemonkey.ws>
 *
 *  Network Block Device Client Side
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "trace.h"
#include "nbd-internal.h"
#include "qemu/cutils.h"

/* Definitions for opaque data types */

static QTAILQ_HEAD(, NBDExport) exports = QTAILQ_HEAD_INITIALIZER(exports);

/* That's all folks */

/* Basic flow for negotiation

   Server         Client
   Negotiate

   or

   Server         Client
   Negotiate #1
                  Option
   Negotiate #2

   ----

   followed by

   Server         Client
                  Request
   Response
                  Request
   Response
                  ...
   ...
                  Request (type == 2)

*/

/* Send an option request.
 *
 * The request is for option @opt, with @data containing @len bytes of
 * additional payload for the request (@len may be -1 to treat @data as
 * a C string; and @data may be NULL if @len is 0).
 * Return 0 if successful, -1 with errp set if it is impossible to
 * continue. */
static int nbd_send_option_request(QIOChannel *ioc, uint32_t opt,
                                   uint32_t len, const char *data,
                                   Error **errp)
{
    NBDOption req;
    QEMU_BUILD_BUG_ON(sizeof(req) != 16);

    if (len == -1) {
        req.length = len = strlen(data);
    }
    trace_nbd_send_option_request(opt, nbd_opt_lookup(opt), len);

    stq_be_p(&req.magic, NBD_OPTS_MAGIC);
    stl_be_p(&req.option, opt);
    stl_be_p(&req.length, len);

    if (nbd_write(ioc, &req, sizeof(req), errp) < 0) {
        error_prepend(errp, "Failed to send option request header: ");
        return -1;
    }

    if (len && nbd_write(ioc, (char *) data, len, errp) < 0) {
        error_prepend(errp, "Failed to send option request data: ");
        return -1;
    }

    return 0;
}

/* Send NBD_OPT_ABORT as a courtesy to let the server know that we are
 * not going to attempt further negotiation. */
static void nbd_send_opt_abort(QIOChannel *ioc)
{
    /* Technically, a compliant server is supposed to reply to us; but
     * older servers disconnected instead. At any rate, we're allowed
     * to disconnect without waiting for the server reply, so we don't
     * even care if the request makes it to the server, let alone
     * waiting around for whether the server replies. */
    nbd_send_option_request(ioc, NBD_OPT_ABORT, 0, NULL, NULL);
}


/* Receive the header of an option reply, which should match the given
 * opt.  Read through the length field, but NOT the length bytes of
 * payload. Return 0 if successful, -1 with errp set if it is
 * impossible to continue. */
static int nbd_receive_option_reply(QIOChannel *ioc, uint32_t opt,
                                    NBDOptionReply *reply, Error **errp)
{
    QEMU_BUILD_BUG_ON(sizeof(*reply) != 20);
    if (nbd_read(ioc, reply, sizeof(*reply), errp) < 0) {
        error_prepend(errp, "failed to read option reply: ");
        nbd_send_opt_abort(ioc);
        return -1;
    }
    reply->magic = be64_to_cpu(reply->magic);
    reply->option = be32_to_cpu(reply->option);
    reply->type = be32_to_cpu(reply->type);
    reply->length = be32_to_cpu(reply->length);

    trace_nbd_receive_option_reply(reply->option, nbd_opt_lookup(reply->option),
                                   reply->type, nbd_rep_lookup(reply->type),
                                   reply->length);

    if (reply->magic != NBD_REP_MAGIC) {
        error_setg(errp, "Unexpected option reply magic");
        nbd_send_opt_abort(ioc);
        return -1;
    }
    if (reply->option != opt) {
        error_setg(errp, "Unexpected option type %u (%s) expected %u (%s)",
                   reply->option, nbd_opt_lookup(reply->option),
                   opt, nbd_opt_lookup(opt));
        nbd_send_opt_abort(ioc);
        return -1;
    }
    return 0;
}

/* If reply represents success, return 1 without further action.
 * If reply represents an error, consume the optional payload of
 * the packet on ioc.  Then return 0 for unsupported (so the client
 * can fall back to other approaches), or -1 with errp set for other
 * errors.
 */
static int nbd_handle_reply_err(QIOChannel *ioc, NBDOptionReply *reply,
                                Error **errp)
{
    char *msg = NULL;
    int result = -1;

    if (!(reply->type & (1 << 31))) {
        return 1;
    }

    if (reply->length) {
        if (reply->length > NBD_MAX_BUFFER_SIZE) {
            error_setg(errp, "server error %" PRIu32
                       " (%s) message is too long",
                       reply->type, nbd_rep_lookup(reply->type));
            goto cleanup;
        }
        msg = g_malloc(reply->length + 1);
        if (nbd_read(ioc, msg, reply->length, errp) < 0) {
            error_prepend(errp, "failed to read option error %" PRIu32
                          " (%s) message: ",
                          reply->type, nbd_rep_lookup(reply->type));
            goto cleanup;
        }
        msg[reply->length] = '\0';
    }

    switch (reply->type) {
    case NBD_REP_ERR_UNSUP:
        trace_nbd_reply_err_unsup(reply->option, nbd_opt_lookup(reply->option));
        result = 0;
        goto cleanup;

    case NBD_REP_ERR_POLICY:
        error_setg(errp, "Denied by server for option %" PRIu32 " (%s)",
                   reply->option, nbd_opt_lookup(reply->option));
        break;

    case NBD_REP_ERR_INVALID:
        error_setg(errp, "Invalid parameters for option %" PRIu32 " (%s)",
                   reply->option, nbd_opt_lookup(reply->option));
        break;

    case NBD_REP_ERR_PLATFORM:
        error_setg(errp, "Server lacks support for option %" PRIu32 " (%s)",
                   reply->option, nbd_opt_lookup(reply->option));
        break;

    case NBD_REP_ERR_TLS_REQD:
        error_setg(errp, "TLS negotiation required before option %" PRIu32
                   " (%s)", reply->option, nbd_opt_lookup(reply->option));
        break;

    case NBD_REP_ERR_UNKNOWN:
        error_setg(errp, "Requested export not available");
        break;

    case NBD_REP_ERR_SHUTDOWN:
        error_setg(errp, "Server shutting down before option %" PRIu32 " (%s)",
                   reply->option, nbd_opt_lookup(reply->option));
        break;

    case NBD_REP_ERR_BLOCK_SIZE_REQD:
        error_setg(errp, "Server requires INFO_BLOCK_SIZE for option %" PRIu32
                   " (%s)", reply->option, nbd_opt_lookup(reply->option));
        break;

    default:
        error_setg(errp, "Unknown error code when asking for option %" PRIu32
                   " (%s)", reply->option, nbd_opt_lookup(reply->option));
        break;
    }

    if (msg) {
        error_append_hint(errp, "server reported: %s\n", msg);
    }

 cleanup:
    g_free(msg);
    if (result < 0) {
        nbd_send_opt_abort(ioc);
    }
    return result;
}

/* Process another portion of the NBD_OPT_LIST reply.  If @want, then
 * set *@match if the current reply matches @want or if the server
 * does not support NBD_OPT_LIST, otherwise leave @match alone.
 * Otherwise, @nameout and @description are malloc'd to contain
 * NUL-terminated copies of the reply.  Return 0 if iteration is
 * complete, positive if more replies are expected, or negative with
 * @errp set if an unrecoverable error occurred. */
static int nbd_receive_list(QIOChannel *ioc, const char *want, bool *match,
                            char **nameout, char **description, Error **errp)
{
    NBDOptionReply reply;
    uint32_t len;
    uint32_t namelen;
    char array[NBD_MAX_NAME_SIZE + 1];
    char *name = array;
    int error;

    if (nbd_receive_option_reply(ioc, NBD_OPT_LIST, &reply, errp) < 0) {
        return -1;
    }
    error = nbd_handle_reply_err(ioc, &reply, errp);
    if (error <= 0) {
        /* The server did not support NBD_OPT_LIST, so set *match on
         * the assumption that any name will be accepted.  */
        if (want) {
            *match = true;
        } else if (!error) {
            error_setg(errp, "Server does not support export lists");
            error = -1;
        }
        return error;
    }
    len = reply.length;

    if (reply.type == NBD_REP_ACK) {
        if (len != 0) {
            error_setg(errp, "length too long for option end");
            nbd_send_opt_abort(ioc);
            return -1;
        }
        return 0;
    } else if (reply.type != NBD_REP_SERVER) {
        error_setg(errp, "Unexpected reply type %u (%s) expected %u (%s)",
                   reply.type, nbd_rep_lookup(reply.type),
                   NBD_REP_SERVER, nbd_rep_lookup(NBD_REP_SERVER));
        nbd_send_opt_abort(ioc);
        return -1;
    }

    if (len < sizeof(namelen) || len > NBD_MAX_BUFFER_SIZE) {
        error_setg(errp, "incorrect option length %" PRIu32, len);
        nbd_send_opt_abort(ioc);
        return -1;
    }
    if (nbd_read(ioc, &namelen, sizeof(namelen), errp) < 0) {
        error_prepend(errp, "failed to read option name length: ");
        nbd_send_opt_abort(ioc);
        return -1;
    }
    namelen = be32_to_cpu(namelen);
    len -= sizeof(namelen);
    if (len < namelen) {
        error_setg(errp, "incorrect option name length");
        nbd_send_opt_abort(ioc);
        return -1;
    }
    if (want) {
        if (namelen != strlen(want)) {
            if (nbd_drop(ioc, len, errp) < 0) {
                error_prepend(errp,
                              "failed to skip export name with wrong length: ");
                nbd_send_opt_abort(ioc);
                return -1;
            }
            return 1;
        }
        assert(namelen < sizeof(array));
    } else {
        assert(nameout);
        *nameout = name = g_new(char, namelen + 1);
    }

    if (nbd_read(ioc, name, namelen, errp) < 0) {
        error_prepend(errp, "failed to read export name: ");
        nbd_send_opt_abort(ioc);
        if (!want) {
            free(name);
        }
        return -1;
    }
    name[namelen] = '\0';
    len -= namelen;
    if (!want) {
        assert(description);
        *description = g_new(char, len + 1);
        if (nbd_read(ioc, *description, len, errp) < 0) {
            error_prepend(errp, "failed to read export description: ");
            nbd_send_opt_abort(ioc);
            free(name);
            free(*description);
            return -1;
        }
        (*description)[len] = '\0';
    } else if (nbd_drop(ioc, len, errp) < 0) {
        error_prepend(errp, "failed to read export description: ");
        nbd_send_opt_abort(ioc);
        return -1;
    }
    if (want && !strcmp(name, want)) {
        *match = true;
    }
    return 1;
}


/* Returns -1 if NBD_OPT_GO proves the export @info->name cannot be
 * used, 0 if NBD_OPT_GO is unsupported (fall back to NBD_OPT_LIST and
 * NBD_OPT_EXPORT_NAME in that case), and > 0 if the export is good to
 * go (with the rest of @info populated). */
static int nbd_opt_info_go(QIOChannel *ioc, uint32_t opt,
                           NBDExportInfo *info, Error **errp)
{
    NBDOptionReply reply;
    uint32_t len = strlen(info->name);
    uint16_t type;
    int error;
    char *buf;

    /* The protocol requires that the server send NBD_INFO_EXPORT with
     * a non-zero flags (at least NBD_FLAG_HAS_FLAGS must be set); so
     * flags still 0 is a witness of a broken server. */
    info->flags = 0;

    assert(opt == NBD_OPT_GO || opt == NBD_OPT_INFO);
    trace_nbd_opt_go_start(nbd_opt_lookup(opt), info->name);
    buf = g_malloc(4 + len + 2 + 2 * info->request_sizes + 1);
    stl_be_p(buf, len);
    memcpy(buf + 4, info->name, len);
    /* At most one request, everything else up to server */
    stw_be_p(buf + 4 + len, info->request_sizes);
    if (info->request_sizes) {
        stw_be_p(buf + 4 + len + 2, NBD_INFO_BLOCK_SIZE);
    }
    error = nbd_send_option_request(ioc, opt,
                                    4 + len + 2 + 2 * info->request_sizes,
                                    buf, errp);
    g_free(buf);
    if (error < 0) {
        return -1;
    }

    while (1) {
        if (nbd_receive_option_reply(ioc, opt, &reply, errp) < 0) {
            return -1;
        }
        error = nbd_handle_reply_err(ioc, &reply, errp);
        if (error <= 0) {
            return error;
        }
        len = reply.length;

        if (reply.type == NBD_REP_ACK) {
            /* Server is done sending info and moved into transmission
               phase, but make sure it sent flags */
            if (len) {
                error_setg(errp, "server sent invalid NBD_REP_ACK");
                return -1;
            }
            if (!info->flags) {
                error_setg(errp, "broken server omitted NBD_INFO_EXPORT");
                return -1;
            }
            trace_nbd_opt_go_success();
            return 1;
        }
        if (reply.type != NBD_REP_INFO) {
            error_setg(errp, "unexpected reply type %u (%s), expected %u (%s)",
                       reply.type, nbd_rep_lookup(reply.type),
                       NBD_REP_INFO, nbd_rep_lookup(NBD_REP_INFO));
            nbd_send_opt_abort(ioc);
            return -1;
        }
        if (len < sizeof(type)) {
            error_setg(errp, "NBD_REP_INFO length %" PRIu32 " is too short",
                       len);
            nbd_send_opt_abort(ioc);
            return -1;
        }
        if (nbd_read(ioc, &type, sizeof(type), errp) < 0) {
            error_prepend(errp, "failed to read info type: ");
            nbd_send_opt_abort(ioc);
            return -1;
        }
        len -= sizeof(type);
        type = be16_to_cpu(type);
        switch (type) {
        case NBD_INFO_EXPORT:
            if (len != sizeof(info->size) + sizeof(info->flags)) {
                error_setg(errp, "remaining export info len %" PRIu32
                           " is unexpected size", len);
                nbd_send_opt_abort(ioc);
                return -1;
            }
            if (nbd_read(ioc, &info->size, sizeof(info->size), errp) < 0) {
                error_prepend(errp, "failed to read info size: ");
                nbd_send_opt_abort(ioc);
                return -1;
            }
            info->size = be64_to_cpu(info->size);
            if (nbd_read(ioc, &info->flags, sizeof(info->flags), errp) < 0) {
                error_prepend(errp, "failed to read info flags: ");
                nbd_send_opt_abort(ioc);
                return -1;
            }
            info->flags = be16_to_cpu(info->flags);
            trace_nbd_receive_negotiate_size_flags(info->size, info->flags);
            break;

        case NBD_INFO_BLOCK_SIZE:
            if (len != sizeof(info->min_block) * 3) {
                error_setg(errp, "remaining export info len %" PRIu32
                           " is unexpected size", len);
                nbd_send_opt_abort(ioc);
                return -1;
            }
            if (nbd_read(ioc, &info->min_block, sizeof(info->min_block),
                         errp) < 0) {
                error_prepend(errp, "failed to read info minimum block size: ");
                nbd_send_opt_abort(ioc);
                return -1;
            }
            info->min_block = be32_to_cpu(info->min_block);
            if (!is_power_of_2(info->min_block)) {
                error_setg(errp, "server minimum block size %" PRIu32
                           " is not a power of two", info->min_block);
                nbd_send_opt_abort(ioc);
                return -1;
            }
            if (nbd_read(ioc, &info->opt_block, sizeof(info->opt_block),
                         errp) < 0) {
                error_prepend(errp,
                              "failed to read info preferred block size: ");
                nbd_send_opt_abort(ioc);
                return -1;
            }
            info->opt_block = be32_to_cpu(info->opt_block);
            if (!is_power_of_2(info->opt_block) ||
                info->opt_block < info->min_block) {
                error_setg(errp, "server preferred block size %" PRIu32
                           " is not valid", info->opt_block);
                nbd_send_opt_abort(ioc);
                return -1;
            }
            if (nbd_read(ioc, &info->max_block, sizeof(info->max_block),
                         errp) < 0) {
                error_prepend(errp, "failed to read info maximum block size: ");
                nbd_send_opt_abort(ioc);
                return -1;
            }
            info->max_block = be32_to_cpu(info->max_block);
            if (info->max_block < info->min_block) {
                error_setg(errp, "server maximum block size %" PRIu32
                           " is not valid", info->max_block);
                nbd_send_opt_abort(ioc);
                return -1;
            }
            trace_nbd_opt_go_info_block_size(info->min_block, info->opt_block,
                                             info->max_block);
            break;

        default:
            trace_nbd_opt_go_info_unknown(type, nbd_info_lookup(type));
            if (nbd_drop(ioc, len, errp) < 0) {
                error_prepend(errp, "Failed to read info payload: ");
                nbd_send_opt_abort(ioc);
                return -1;
            }
            break;
        }
    }
}

/* Return -1 on failure, 0 if wantname is an available export. */
static int nbd_receive_query_exports(QIOChannel *ioc,
                                     const char *wantname,
                                     Error **errp)
{
    bool foundExport = false;

    trace_nbd_receive_query_exports_start(wantname);
    if (nbd_send_option_request(ioc, NBD_OPT_LIST, 0, NULL, errp) < 0) {
        return -1;
    }

    while (1) {
        int ret = nbd_receive_list(ioc, wantname, &foundExport,
                                   NULL, NULL, errp);

        if (ret < 0) {
            /* Server gave unexpected reply */
            return -1;
        } else if (ret == 0) {
            /* Done iterating. */
            if (!foundExport) {
                error_setg(errp, "No export with name '%s' available",
                           wantname);
                nbd_send_opt_abort(ioc);
                return -1;
            }
            trace_nbd_receive_query_exports_success(wantname);
            return 0;
        }
    }
}

/* nbd_request_simple_option: Send an option request, and parse the reply
 * return 1 for successful negotiation,
 *        0 if operation is unsupported,
 *        -1 with errp set for any other error
 */
static int nbd_request_simple_option(QIOChannel *ioc, int opt, Error **errp)
{
    NBDOptionReply reply;
    int error;

    if (nbd_send_option_request(ioc, opt, 0, NULL, errp) < 0) {
        return -1;
    }

    if (nbd_receive_option_reply(ioc, opt, &reply, errp) < 0) {
        return -1;
    }
    error = nbd_handle_reply_err(ioc, &reply, errp);
    if (error <= 0) {
        return error;
    }

    if (reply.type != NBD_REP_ACK) {
        error_setg(errp, "Server answered option %d (%s) with unexpected "
                   "reply %" PRIu32 " (%s)", opt, nbd_opt_lookup(opt),
                   reply.type, nbd_rep_lookup(reply.type));
        nbd_send_opt_abort(ioc);
        return -1;
    }

    if (reply.length != 0) {
        error_setg(errp, "Option %d ('%s') response length is %" PRIu32
                   " (it should be zero)", opt, nbd_opt_lookup(opt),
                   reply.length);
        nbd_send_opt_abort(ioc);
        return -1;
    }

    return 1;
}

static QIOChannel *nbd_receive_starttls(QIOChannel *ioc,
                                        QCryptoTLSCreds *tlscreds,
                                        const char *hostname, Error **errp)
{
    int ret;
    QIOChannelTLS *tioc;
    struct NBDTLSHandshakeData data = { 0 };

    ret = nbd_request_simple_option(ioc, NBD_OPT_STARTTLS, errp);
    if (ret <= 0) {
        if (ret == 0) {
            error_setg(errp, "Server don't support STARTTLS option");
            nbd_send_opt_abort(ioc);
        }
        return NULL;
    }

    trace_nbd_receive_starttls_new_client();
    tioc = qio_channel_tls_new_client(ioc, tlscreds, hostname, errp);
    if (!tioc) {
        return NULL;
    }
    qio_channel_set_name(QIO_CHANNEL(tioc), "nbd-client-tls");
    data.loop = g_main_loop_new(g_main_context_default(), FALSE);
    trace_nbd_receive_starttls_tls_handshake();
    qio_channel_tls_handshake(tioc,
                              nbd_tls_handshake,
                              &data,
                              NULL,
                              NULL);

    if (!data.complete) {
        g_main_loop_run(data.loop);
    }
    g_main_loop_unref(data.loop);
    if (data.error) {
        error_propagate(errp, data.error);
        object_unref(OBJECT(tioc));
        return NULL;
    }

    return QIO_CHANNEL(tioc);
}

/* nbd_negotiate_simple_meta_context:
 * List or set meta context data for export @info->name, based on @opt.
 * For list, leave @context NULL for 0 queries, or supplied for a single
 * query; all replies are ignored, and the call merely traces server behavior.
 * For set, @context must result in at most one matching server reply, in
 * which case @info->meta_base_allocation_id is set to the resulting id.
 * return 1 for successful negotiation,
 *        0 if operation is unsupported or context unavailable,
 *        -1 with errp set for any other error
 */
static int nbd_negotiate_simple_meta_context(QIOChannel *ioc,
                                             int32_t opt,
                                             const char *context,
                                             NBDExportInfo *info,
                                             Error **errp)
{
    int ret;
    NBDOptionReply reply;
    uint32_t received_id = 0;
    bool received = false;
    uint32_t export_len = strlen(info->name);
    uint32_t context_len;
    uint32_t data_len = sizeof(export_len) + export_len + sizeof(uint32_t);
    char *data;
    char *p;

    if (!context) {
        assert(opt == NBD_OPT_LIST_META_CONTEXT);
    } else {
        context_len = strlen(context);
        data_len += sizeof(context_len) + context_len;
    }
    data = g_malloc(data_len);
    p = data;

    trace_nbd_opt_meta_request(nbd_opt_lookup(opt), context ?: "(all)",
                               info->name);
    stl_be_p(p, export_len);
    memcpy(p += sizeof(export_len), info->name, export_len);
    stl_be_p(p += export_len, !!context);
    if (context) {
        stl_be_p(p += sizeof(uint32_t), context_len);
        memcpy(p += sizeof(context_len), context, context_len);
    }

    ret = nbd_send_option_request(ioc, opt, data_len, data, errp);
    g_free(data);
    if (ret < 0) {
        return ret;
    }

    if (nbd_receive_option_reply(ioc, opt, &reply, errp) < 0)
    {
        return -1;
    }

    ret = nbd_handle_reply_err(ioc, &reply, errp);
    if (ret <= 0) {
        return ret;
    }

    while (reply.type == NBD_REP_META_CONTEXT) {
        char *name;

        if (reply.length <= sizeof(received_id)) {
            error_setg(errp, "Failed to negotiate meta context '%s', server "
                       "answered with unexpected length %" PRIu32, context,
                       reply.length);
            nbd_send_opt_abort(ioc);
            return -1;
        }

        if (nbd_read(ioc, &received_id, sizeof(received_id), errp) < 0) {
            return -1;
        }
        received_id = be32_to_cpu(received_id);

        reply.length -= sizeof(received_id);
        name = g_malloc(reply.length + 1);
        if (nbd_read(ioc, name, reply.length, errp) < 0) {
            g_free(name);
            return -1;
        }
        name[reply.length] = '\0';

        trace_nbd_opt_meta_reply(name, received_id);
        if (opt == NBD_OPT_SET_META_CONTEXT) {
            if (received) {
                error_setg(errp, "Server replied with more than one context");
                free(name);
                nbd_send_opt_abort(ioc);
                return -1;
            }

            if (strcmp(context, name)) {
                error_setg(errp,
                           "Failed to negotiate meta context '%s', server "
                           "answered with different context '%s'", context,
                           name);
                g_free(name);
                nbd_send_opt_abort(ioc);
                return -1;
            }
            g_free(name);
            received = true;
        } else {
            info->contexts = g_renew(char *, info->contexts,
                                     ++info->n_contexts);
            info->contexts[info->n_contexts - 1] = name;
            received |= strstart(name, "qemu:", NULL);
        }

        /* receive NBD_REP_ACK */
        if (nbd_receive_option_reply(ioc, opt, &reply, errp) < 0) {
            return -1;
        }

        ret = nbd_handle_reply_err(ioc, &reply, errp);
        if (ret <= 0) {
            return ret;
        }
    }

    if (reply.type != NBD_REP_ACK) {
        error_setg(errp, "Unexpected reply type %u (%s) expected %u (%s)",
                   reply.type, nbd_rep_lookup(reply.type),
                   NBD_REP_ACK, nbd_rep_lookup(NBD_REP_ACK));
        nbd_send_opt_abort(ioc);
        return -1;
    }
    if (reply.length) {
        error_setg(errp, "Unexpected length to ACK response");
        nbd_send_opt_abort(ioc);
        return -1;
    }

    if (received && opt == NBD_OPT_SET_META_CONTEXT) {
        info->meta_base_allocation_id = received_id;
    }

    /* Recurse to work around qemu 3.0 bug - the server forgot to send
     * "qemu:" replies to 0 queries. */
    if (!context && !received) {
        return nbd_negotiate_simple_meta_context(ioc, opt, "qemu:", info,
                                                 errp);
    }

    return received || opt == NBD_OPT_LIST_META_CONTEXT;
}

/* Start the handshake to the server.  After a positive return, the server
 * is ready to accept additional NBD_OPT requests.
 * Returns: negative errno: failure talking to server
 *          0: server is oldstyle, client must still parse export size
 *          1: server is newstyle, but can only accept EXPORT_NAME
 *          2: server is newstyle, but lacks structured replies
 *          3: server is newstyle and set up for structured replies
 */
static int nbd_start_negotiate(QIOChannel *ioc, QCryptoTLSCreds *tlscreds,
                               const char *hostname, QIOChannel **outioc,
                               bool structured_reply, bool *zeroes,
                               Error **errp)
{
    uint64_t magic;

    trace_nbd_start_negotiate(tlscreds, hostname ? hostname : "<null>");

    if (outioc) {
        *outioc = NULL;
    }
    if (tlscreds && !outioc) {
        error_setg(errp, "Output I/O channel required for TLS");
        return -EINVAL;
    }

    if (nbd_read(ioc, &magic, sizeof(magic), errp) < 0) {
        error_prepend(errp, "Failed to read data: ");
        return -EINVAL;
    }
    magic = be64_to_cpu(magic);
    trace_nbd_receive_negotiate_magic(magic);

    if (magic != NBD_INIT_MAGIC) {
        error_setg(errp, "Invalid magic received");
        return -EINVAL;
    }

    if (nbd_read(ioc, &magic, sizeof(magic), errp) < 0) {
        error_prepend(errp, "Failed to read magic: ");
        return -EINVAL;
    }
    magic = be64_to_cpu(magic);
    trace_nbd_receive_negotiate_magic(magic);

    if (magic == NBD_OPTS_MAGIC) {
        uint32_t clientflags = 0;
        uint16_t globalflags;
        bool fixedNewStyle = false;

        if (nbd_read(ioc, &globalflags, sizeof(globalflags), errp) < 0) {
            error_prepend(errp, "Failed to read server flags: ");
            return -EINVAL;
        }
        globalflags = be16_to_cpu(globalflags);
        trace_nbd_receive_negotiate_server_flags(globalflags);
        if (globalflags & NBD_FLAG_FIXED_NEWSTYLE) {
            fixedNewStyle = true;
            clientflags |= NBD_FLAG_C_FIXED_NEWSTYLE;
        }
        if (globalflags & NBD_FLAG_NO_ZEROES) {
            if (zeroes) {
                *zeroes = false;
            }
            clientflags |= NBD_FLAG_C_NO_ZEROES;
        }
        /* client requested flags */
        clientflags = cpu_to_be32(clientflags);
        if (nbd_write(ioc, &clientflags, sizeof(clientflags), errp) < 0) {
            error_prepend(errp, "Failed to send clientflags field: ");
            return -EINVAL;
        }
        if (tlscreds) {
            if (fixedNewStyle) {
                *outioc = nbd_receive_starttls(ioc, tlscreds, hostname, errp);
                if (!*outioc) {
                    return -EINVAL;
                }
                ioc = *outioc;
            } else {
                error_setg(errp, "Server does not support STARTTLS");
                return -EINVAL;
            }
        }
        if (fixedNewStyle) {
            int result = 0;

            if (structured_reply) {
                result = nbd_request_simple_option(ioc,
                                                   NBD_OPT_STRUCTURED_REPLY,
                                                   errp);
                if (result < 0) {
                    return -EINVAL;
                }
            }
            return 2 + result;
        } else {
            return 1;
        }
    } else if (magic == NBD_CLIENT_MAGIC) {
        if (tlscreds) {
            error_setg(errp, "Server does not support STARTTLS");
            return -EINVAL;
        }
        return 0;
    } else {
        error_setg(errp, "Bad magic received");
        return -EINVAL;
    }
}

/* Connect to server, complete negotiation, and move into transmission phase.
 * Returns: negative errno: failure talking to server
 *          0: server is connected
 */
int nbd_receive_negotiate(QIOChannel *ioc, QCryptoTLSCreds *tlscreds,
                          const char *hostname, QIOChannel **outioc,
                          NBDExportInfo *info, Error **errp)
{
    int result;
    bool zeroes = true;
    bool base_allocation = info->base_allocation;
    uint32_t oldflags;

    assert(info->name);
    trace_nbd_receive_negotiate_name(info->name);

    result = nbd_start_negotiate(ioc, tlscreds, hostname, outioc,
                                 info->structured_reply, &zeroes, errp);

    info->structured_reply = false;
    info->base_allocation = false;
    if (tlscreds && *outioc) {
        ioc = *outioc;
    }

    switch (result) {
    case 3: /* newstyle, with structured replies */
        info->structured_reply = true;
        if (base_allocation) {
            result = nbd_negotiate_simple_meta_context(
                        ioc, NBD_OPT_SET_META_CONTEXT,
                        info->x_dirty_bitmap ?: "base:allocation",
                        info, errp);
            if (result < 0) {
                return -EINVAL;
            }
            info->base_allocation = result == 1;
        }
        /* fall through */
    case 2: /* newstyle, try OPT_GO */
        /* Try NBD_OPT_GO first - if it works, we are done (it
         * also gives us a good message if the server requires
         * TLS).  If it is not available, fall back to
         * NBD_OPT_LIST for nicer error messages about a missing
         * export, then use NBD_OPT_EXPORT_NAME.  */
        result = nbd_opt_info_go(ioc, NBD_OPT_GO, info, errp);
        if (result < 0) {
            return -EINVAL;
        }
        if (result > 0) {
            return 0;
        }
        /* Check our desired export is present in the
         * server export list. Since NBD_OPT_EXPORT_NAME
         * cannot return an error message, running this
         * query gives us better error reporting if the
         * export name is not available.
         */
        if (nbd_receive_query_exports(ioc, info->name, errp) < 0) {
            return -EINVAL;
        }
        /* fall through */
    case 1: /* newstyle, but limited to EXPORT_NAME */
        /* write the export name request */
        if (nbd_send_option_request(ioc, NBD_OPT_EXPORT_NAME, -1, info->name,
                                    errp) < 0) {
            return -EINVAL;
        }

        /* Read the response */
        if (nbd_read(ioc, &info->size, sizeof(info->size), errp) < 0) {
            error_prepend(errp, "Failed to read export length: ");
            return -EINVAL;
        }
        info->size = be64_to_cpu(info->size);

        if (nbd_read(ioc, &info->flags, sizeof(info->flags), errp) < 0) {
            error_prepend(errp, "Failed to read export flags: ");
            return -EINVAL;
        }
        info->flags = be16_to_cpu(info->flags);
        break;
    case 0: /* oldstyle, parse length and flags */
        if (*info->name) {
            error_setg(errp, "Server does not support non-empty export names");
            return -EINVAL;
        }

        if (nbd_read(ioc, &info->size, sizeof(info->size), errp) < 0) {
            error_prepend(errp, "Failed to read export length: ");
            return -EINVAL;
        }
        info->size = be64_to_cpu(info->size);

        if (nbd_read(ioc, &oldflags, sizeof(oldflags), errp) < 0) {
            error_prepend(errp, "Failed to read export flags: ");
            return -EINVAL;
        }
        oldflags = be32_to_cpu(oldflags);
        if (oldflags & ~0xffff) {
            error_setg(errp, "Unexpected export flags %0x" PRIx32, oldflags);
            return -EINVAL;
        }
        info->flags = oldflags;
        break;
    default:
        return result;
    }

    trace_nbd_receive_negotiate_size_flags(info->size, info->flags);
    if (zeroes && nbd_drop(ioc, 124, errp) < 0) {
        error_prepend(errp, "Failed to read reserved block: ");
        return -EINVAL;
    }
    return 0;
}

/* Clean up result of nbd_receive_export_list */
void nbd_free_export_list(NBDExportInfo *info, int count)
{
    int i, j;

    for (i = 0; info && i < count; i++) {
        free(info[i].name);
        free(info[i].description);
        for (j = 0; j < info[i].n_contexts; j++) {
            free(info[i].contexts[j]);
        }
        free(info[i].contexts);
    }
    free(info);
}

/* Query details about a server's exports, then disconnect without
 * going into transmission phase. Return a count of the exports listed
 * in @info by the server, or -1 on error. Caller must free @info. */
int nbd_receive_export_list(QIOChannel *ioc, QCryptoTLSCreds *tlscreds,
                            const char *hostname, NBDExportInfo **info,
                            Error **errp)
{
    int result;
    int count = 0;
    int i;
    int rc;
    int ret = -1;
    NBDExportInfo *array = NULL;
    uint32_t oldflags;
    QIOChannel *sioc = NULL;
    bool try_context = true;

    *info = NULL;
    result = nbd_start_negotiate(ioc, tlscreds, hostname, &sioc, true, NULL,
                                 errp);
    if (tlscreds && sioc) {
        ioc = sioc;
    }

    switch (result) {
    case 2:
        /* meta contexts are only useful with structured reply */
        try_context = false;
        /* fall through */
    case 3:
        /* newstyle - use NBD_OPT_LIST to populate array, then try
         * NBD_OPT_INFO on each array member. If structured replies
         * are enabled, also try NBD_OPT_LIST_META_CONTEXT. */
        if (nbd_send_option_request(ioc, NBD_OPT_LIST, 0, NULL, errp) < 0) {
            goto out;
        }
        while (1) {
            char *name;
            char *desc;

            rc = nbd_receive_list(ioc, NULL, NULL, &name, &desc, errp);
            if (rc < 0) {
                goto out;
            } else if (rc == 0) {
                break;
            }
            array = g_renew(NBDExportInfo, array, ++count);
            memset(&array[count - 1], 0, sizeof(*array));
            array[count - 1].name = name;
            array[count - 1].description = desc;
            array[count - 1].structured_reply = result == 3;
        }

        for (i = 0; i < count; i++) {
            array[i].request_sizes = true;
            rc = nbd_opt_info_go(ioc, NBD_OPT_INFO, &array[i], errp);
            if (rc < 0) {
                goto out;
            } else if (rc == 0) {
                /* Pointless to try rest of loop. If OPT_INFO doesn't work,
                 * it's unlikely that meta contexts work either */
                break;
            }

            if (try_context) {
                rc = nbd_negotiate_simple_meta_context(
                    ioc, NBD_OPT_LIST_META_CONTEXT, NULL, &array[i], errp);
                if (rc < 0) {
                    goto out;
                } else if (rc == 0) {
                    try_context = false;
                }
            }
        }

        /* Send NBD_OPT_ABORT as a courtesy before hanging up */
        nbd_send_opt_abort(ioc);
        break;
    case 1: /* newstyle, but limited to EXPORT_NAME */
        error_setg(errp, "Server does not support export lists");
        /* We can't even send NBD_OPT_ABORT, so merely hang up */
        goto out;
    case 0: /* oldstyle, parse length and flags */
        array = g_new0(NBDExportInfo, 1);
        array->name = g_strdup("");
        count = 1;

        if (nbd_read(ioc, &array->size, sizeof(array->size), errp) < 0) {
            error_prepend(errp, "Failed to read export length: ");
            goto out;
        }
        array->size = be64_to_cpu(array->size);

        if (nbd_read(ioc, &oldflags, sizeof(oldflags), errp) < 0) {
            error_prepend(errp, "Failed to read export flags: ");
            goto out;
        }
        oldflags = be32_to_cpu(oldflags);
        if (oldflags & ~0xffff) {
            error_setg(errp, "Unexpected export flags %0x" PRIx32, oldflags);
            goto out;
        }
        array->flags = oldflags;

        /* Send NBD_CMD_DISC as a courtesy to the server, but ignore all
         * errors now that we have the information we wanted. */
        if (nbd_drop(ioc, 124, NULL) == 0) {
            NBDRequest request = { .type = NBD_CMD_DISC };

            nbd_send_request(ioc, &request);
        }
        break;
    default:
        goto out;
    }

    *info = array;
    array = NULL;
    ret = count;

 out:
    qio_channel_shutdown(ioc, QIO_CHANNEL_SHUTDOWN_BOTH, NULL);
    qio_channel_close(ioc, NULL);
    object_unref(OBJECT(sioc));
    nbd_free_export_list(array, count);
    return ret;
}

#ifdef __linux__
int nbd_init(int fd, QIOChannelSocket *sioc, NBDExportInfo *info,
             Error **errp)
{
    unsigned long sector_size = MAX(BDRV_SECTOR_SIZE, info->min_block);
    unsigned long sectors = info->size / sector_size;

    /* FIXME: Once the kernel module is patched to honor block sizes,
     * and to advertise that fact to user space, we should update the
     * hand-off to the kernel to use any block sizes we learned. */
    assert(!info->request_sizes);
    if (info->size / sector_size != sectors) {
        error_setg(errp, "Export size %" PRIu64 " too large for 32-bit kernel",
                   info->size);
        return -E2BIG;
    }

    trace_nbd_init_set_socket();

    if (ioctl(fd, NBD_SET_SOCK, (unsigned long) sioc->fd) < 0) {
        int serrno = errno;
        error_setg(errp, "Failed to set NBD socket");
        return -serrno;
    }

    trace_nbd_init_set_block_size(sector_size);

    if (ioctl(fd, NBD_SET_BLKSIZE, sector_size) < 0) {
        int serrno = errno;
        error_setg(errp, "Failed setting NBD block size");
        return -serrno;
    }

    trace_nbd_init_set_size(sectors);
    if (info->size % sector_size) {
        trace_nbd_init_trailing_bytes(info->size % sector_size);
    }

    if (ioctl(fd, NBD_SET_SIZE_BLOCKS, sectors) < 0) {
        int serrno = errno;
        error_setg(errp, "Failed setting size (in blocks)");
        return -serrno;
    }

    if (ioctl(fd, NBD_SET_FLAGS, (unsigned long) info->flags) < 0) {
        if (errno == ENOTTY) {
            int read_only = (info->flags & NBD_FLAG_READ_ONLY) != 0;
            trace_nbd_init_set_readonly();

            if (ioctl(fd, BLKROSET, (unsigned long) &read_only) < 0) {
                int serrno = errno;
                error_setg(errp, "Failed setting read-only attribute");
                return -serrno;
            }
        } else {
            int serrno = errno;
            error_setg(errp, "Failed setting flags");
            return -serrno;
        }
    }

    trace_nbd_init_finish();

    return 0;
}

int nbd_client(int fd)
{
    int ret;
    int serrno;

    trace_nbd_client_loop();

    ret = ioctl(fd, NBD_DO_IT);
    if (ret < 0 && errno == EPIPE) {
        /* NBD_DO_IT normally returns EPIPE when someone has disconnected
         * the socket via NBD_DISCONNECT.  We do not want to return 1 in
         * that case.
         */
        ret = 0;
    }
    serrno = errno;

    trace_nbd_client_loop_ret(ret, strerror(serrno));

    trace_nbd_client_clear_queue();
    ioctl(fd, NBD_CLEAR_QUE);

    trace_nbd_client_clear_socket();
    ioctl(fd, NBD_CLEAR_SOCK);

    errno = serrno;
    return ret;
}

int nbd_disconnect(int fd)
{
    ioctl(fd, NBD_CLEAR_QUE);
    ioctl(fd, NBD_DISCONNECT);
    ioctl(fd, NBD_CLEAR_SOCK);
    return 0;
}

#endif /* __linux__ */

int nbd_send_request(QIOChannel *ioc, NBDRequest *request)
{
    uint8_t buf[NBD_REQUEST_SIZE];

    trace_nbd_send_request(request->from, request->len, request->handle,
                           request->flags, request->type,
                           nbd_cmd_lookup(request->type));

    stl_be_p(buf, NBD_REQUEST_MAGIC);
    stw_be_p(buf + 4, request->flags);
    stw_be_p(buf + 6, request->type);
    stq_be_p(buf + 8, request->handle);
    stq_be_p(buf + 16, request->from);
    stl_be_p(buf + 24, request->len);

    return nbd_write(ioc, buf, sizeof(buf), NULL);
}

/* nbd_receive_simple_reply
 * Read simple reply except magic field (which should be already read).
 * Payload is not read (payload is possible for CMD_READ, but here we even
 * don't know whether it take place or not).
 */
static int nbd_receive_simple_reply(QIOChannel *ioc, NBDSimpleReply *reply,
                                    Error **errp)
{
    int ret;

    assert(reply->magic == NBD_SIMPLE_REPLY_MAGIC);

    ret = nbd_read(ioc, (uint8_t *)reply + sizeof(reply->magic),
                   sizeof(*reply) - sizeof(reply->magic), errp);
    if (ret < 0) {
        return ret;
    }

    reply->error = be32_to_cpu(reply->error);
    reply->handle = be64_to_cpu(reply->handle);

    return 0;
}

/* nbd_receive_structured_reply_chunk
 * Read structured reply chunk except magic field (which should be already
 * read).
 * Payload is not read.
 */
static int nbd_receive_structured_reply_chunk(QIOChannel *ioc,
                                              NBDStructuredReplyChunk *chunk,
                                              Error **errp)
{
    int ret;

    assert(chunk->magic == NBD_STRUCTURED_REPLY_MAGIC);

    ret = nbd_read(ioc, (uint8_t *)chunk + sizeof(chunk->magic),
                   sizeof(*chunk) - sizeof(chunk->magic), errp);
    if (ret < 0) {
        return ret;
    }

    chunk->flags = be16_to_cpu(chunk->flags);
    chunk->type = be16_to_cpu(chunk->type);
    chunk->handle = be64_to_cpu(chunk->handle);
    chunk->length = be32_to_cpu(chunk->length);

    return 0;
}

/* nbd_receive_reply
 * Returns 1 on success
 *         0 on eof, when no data was read (errp is not set)
 *         negative errno on failure (errp is set)
 */
int nbd_receive_reply(QIOChannel *ioc, NBDReply *reply, Error **errp)
{
    int ret;
    const char *type;

    ret = nbd_read_eof(ioc, &reply->magic, sizeof(reply->magic), errp);
    if (ret <= 0) {
        return ret;
    }

    reply->magic = be32_to_cpu(reply->magic);

    switch (reply->magic) {
    case NBD_SIMPLE_REPLY_MAGIC:
        ret = nbd_receive_simple_reply(ioc, &reply->simple, errp);
        if (ret < 0) {
            break;
        }
        trace_nbd_receive_simple_reply(reply->simple.error,
                                       nbd_err_lookup(reply->simple.error),
                                       reply->handle);
        break;
    case NBD_STRUCTURED_REPLY_MAGIC:
        ret = nbd_receive_structured_reply_chunk(ioc, &reply->structured, errp);
        if (ret < 0) {
            break;
        }
        type = nbd_reply_type_lookup(reply->structured.type);
        trace_nbd_receive_structured_reply_chunk(reply->structured.flags,
                                                 reply->structured.type, type,
                                                 reply->structured.handle,
                                                 reply->structured.length);
        break;
    default:
        error_setg(errp, "invalid magic (got 0x%" PRIx32 ")", reply->magic);
        return -EINVAL;
    }
    if (ret < 0) {
        return ret;
    }

    return 1;
}

