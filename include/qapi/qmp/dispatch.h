/*
 * Core Definitions for QAPI/QMP Dispatch
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#ifndef QAPI_QMP_DISPATCH_H
#define QAPI_QMP_DISPATCH_H

#include "qapi/qmp/qobject.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/json-streamer.h"

typedef struct QmpClient QmpClient;

typedef bool (QmpPreDispatch) (QmpClient *client, QObject *rsp, Error **err);
typedef bool (QmpPostDispatch) (QmpClient *client, QObject *rsp, Error **err);
typedef void (QmpDispatchReturn) (QmpClient *client, QObject *rsp);

typedef struct QmpReturn {
    QDict *rsp;
    QmpClient *client;

    QLIST_ENTRY(QmpReturn) link;
} QmpReturn;

struct QmpClient {
    JSONMessageParser parser;
    QmpPreDispatch *pre_dispatch_cb;
    QmpPostDispatch *post_dispatch_cb;
    QmpDispatchReturn *return_cb;

    QLIST_HEAD(, QmpReturn) pending;
};

typedef void (QmpCommandFunc)(QDict *, QObject **, Error **);
typedef void (QmpCommandFuncAsync)(QDict *, QmpReturn *);

typedef enum QmpCommandType {
    QCT_NORMAL,
    QCT_ASYNC,
} QmpCommandType;

typedef enum QmpCommandOptions
{
    QCO_NO_OPTIONS = 0x0,
    QCO_NO_SUCCESS_RESP = 0x1,
} QmpCommandOptions;

typedef struct QmpCommand
{
    QmpCommandType type;
    const char *name;
    union {
        QmpCommandFunc *fn;
        QmpCommandFuncAsync *fn_async;
    };
    QmpCommandOptions options;
    QTAILQ_ENTRY(QmpCommand) node;
    bool enabled;
} QmpCommand;

void qmp_register_command(const char *name, QmpCommandFunc *fn,
                          QmpCommandOptions options);
void qmp_register_async_command(const char *name, QmpCommandFuncAsync *fn,
                                QmpCommandOptions options);
void qmp_unregister_command(const char *name);
QmpCommand *qmp_find_command(const char *name);
void qmp_client_init(QmpClient *client,
                     QmpPreDispatch *pre_dispatch_cb,
                     QmpPostDispatch *post_dispatch_cb,
                     QmpDispatchReturn *return_cb);
void qmp_client_destroy(QmpClient *client);
void qmp_client_feed(QmpClient *client, const char *buffer, size_t size);

void qmp_dispatch(QmpClient *client, QObject *request, QDict *rsp);
void qmp_disable_command(const char *name);
void qmp_enable_command(const char *name);
bool qmp_command_is_enabled(const QmpCommand *cmd);
const char *qmp_command_name(const QmpCommand *cmd);
bool qmp_has_success_response(const QmpCommand *cmd);
QObject *qmp_build_error_object(Error *err);
typedef void (*qmp_cmd_callback_fn)(QmpCommand *cmd, void *opaque);
void qmp_for_each_command(qmp_cmd_callback_fn fn, void *opaque);

/*
 * qmp_return{_error}:
 *
 * Construct the command reply, and call the
 * return_cb() associated with the QmpClient.
 *
 * Finally, free the QmpReturn.
 */
void qmp_return(QmpReturn *qret, QObject *cmd_rsp);
void qmp_return_error(QmpReturn *qret, Error *err);

/*
 * qmp_return_is_cancelled:
 *
 * Return true if the QmpReturn is cancelled, and free the QmpReturn
 * in this case.
 */
bool qmp_return_is_cancelled(QmpReturn *qret);

#endif
