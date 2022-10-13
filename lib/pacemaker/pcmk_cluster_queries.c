/*
 * Copyright 2020-2022 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <glib.h>               // gboolean, GMainLoop, etc.
#include <libxml/tree.h>        // xmlNode

#include <pacemaker.h>
#include <pacemaker-internal.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/cib/internal.h>
#include <crm/msg_xml.h>
#include <crm/common/output_internal.h>
#include <crm/common/xml.h>
#include <crm/common/xml_internal.h>
#include <crm/common/iso8601.h>
#include <crm/common/ipc_controld.h>
#include <crm/common/ipc_pacemakerd.h>
#include <crm/common/mainloop.h>

#define DEFAULT_MESSAGE_TIMEOUT_MS 30000


typedef struct {
    pcmk__output_t *out;
    GMainLoop *mainloop;
    int rc;
    guint message_timer_id;
    guint message_timeout_ms;
    enum pcmk_pacemakerd_state pcmkd_state;
} data_t;

static void
quit_main_loop(data_t *data)
{
    if (data->mainloop != NULL) {
        GMainLoop *mloop = data->mainloop;

        data->mainloop = NULL; // Don't re-enter this block
        pcmk_quit_main_loop(mloop, 10);
        g_main_loop_unref(mloop);
    }
}

static gboolean
admin_message_timeout(gpointer user_data)
{
    data_t *data = user_data;
    pcmk__output_t *out = data->out;

    out->err(out, "error: No reply received from controller before timeout (%dms)",
            data->message_timeout_ms);
    data->message_timer_id = 0;
    data->rc = ETIMEDOUT;
    quit_main_loop(data);
    return FALSE; // Tells glib to remove source
}

static void
start_main_loop(data_t *data)
{
    if (data->message_timeout_ms < 1) {
        data->message_timeout_ms = DEFAULT_MESSAGE_TIMEOUT_MS;
    }

    data->rc = ECONNRESET; // For unexpected disconnects
    data->mainloop = g_main_loop_new(NULL, FALSE);
    data->message_timer_id = g_timeout_add(data->message_timeout_ms,
                                     admin_message_timeout,
                                     data);
    g_main_loop_run(data->mainloop);
}

static void
event_done(data_t *data, pcmk_ipc_api_t *api)
{
    pcmk_disconnect_ipc(api);
    quit_main_loop(data);
}

/*!
 * \internal
 * \brief Process a reply from the controller IPC API
 *
 * \param[in,out] data          API results and options
 * \param[in,out] controld_api  Controller connection
 * \param[in]     event_type    Type of event that occurred
 * \param[in]     status        Event status
 * \param[in]     event_data    \p pcmk_controld_api_reply_t object containing
 *                              event-specific data
 */
static pcmk_controld_api_reply_t *
controld_event_reply(data_t *data, pcmk_ipc_api_t *controld_api,
                     enum pcmk_ipc_event event_type, crm_exit_t status,
                     const void *event_data)
{
    pcmk__output_t *out = data->out;
    pcmk_controld_api_reply_t *reply = event_data;

    switch (event_type) {
        case pcmk_ipc_event_disconnect:
            if (data->rc == ECONNRESET) { // Unexpected
                out->err(out, "error: Lost connection to controller");
            }
            event_done(data, controld_api);
            return NULL;

        case pcmk_ipc_event_reply:
            break;

        default:
            return NULL;
    }

    if (data->message_timer_id != 0) {
        g_source_remove(data->message_timer_id);
        data->message_timer_id = 0;
    }

    if (status != CRM_EX_OK) {
        out->err(out, "error: Bad reply from controller: %s",
                 crm_exit_str(status));
        data->rc = EBADMSG;
        event_done(data, controld_api);
        return NULL;
    }

    if (reply->reply_type != pcmk_controld_reply_ping) {
        out->err(out, "error: Unknown reply type %d from controller",
                 reply->reply_type);
        data->rc = EBADMSG;
        event_done(data, controld_api);
        return NULL;
    }

    return reply;
}

/*!
 * \internal
 * \brief Process a controller status IPC event
 *
 * \param[in,out] controld_api  Controller connection
 * \param[in]     event_type    Type of event that occurred
 * \param[in]     status        Event status
 * \param[in,out] event_data    \p pcmk_controld_api_reply_t object containing
 *                              event-specific data
 * \param[in,out] user_data     \p data_t object for API results and options
 */
static void
controller_status_event_cb(pcmk_ipc_api_t *controld_api,
                           enum pcmk_ipc_event event_type, crm_exit_t status,
                           void *event_data, void *user_data)
{
    data_t *data = (data_t *) user_data;
    pcmk__output_t *out = data->out;
    pcmk_controld_api_reply_t *reply = controld_event_reply(data, controld_api,
                                                            event_type, status,
                                                            event_data);

    if (reply != NULL) {
        out->message(out, "health",
                     reply->data.ping.sys_from, reply->host_from,
                     reply->data.ping.fsa_state, reply->data.ping.result);
        data->rc = pcmk_rc_ok;
    }

    event_done(data, controld_api);
}

/*!
 * \internal
 * \brief Process a designated controller IPC event
 *
 * \param[in,out] controld_api  Controller connection
 * \param[in]     event_type    Type of event that occurred
 * \param[in]     status        Event status
 * \param[in,out] event_data    \p pcmk_controld_api_reply_t object containing
 *                              event-specific data
 * \param[in,out] user_data     \p data_t object for API results and options
 */
static void
designated_controller_event_cb(pcmk_ipc_api_t *controld_api,
                               enum pcmk_ipc_event event_type,
                               crm_exit_t status, void *event_data,
                               void *user_data)
{
    data_t *data = (data_t *) user_data;
    pcmk__output_t *out = data->out;
    pcmk_controld_api_reply_t *reply = controld_event_reply(data, controld_api,
                                                            event_type, status,
                                                            event_data);

    if (reply != NULL) {
        out->message(out, "dc", reply->host_from);
        data->rc = pcmk_rc_ok;
    }

    event_done(data, controld_api);
}

static void
pacemakerd_event_cb(pcmk_ipc_api_t *pacemakerd_api,
                    enum pcmk_ipc_event event_type, crm_exit_t status,
                    void *event_data, void *user_data)
{
    data_t *data = user_data;
    pcmk__output_t *out = data->out;
    pcmk_pacemakerd_api_reply_t *reply = event_data;

    switch (event_type) {
        case pcmk_ipc_event_disconnect:
            if (data->rc == ECONNRESET) { // Unexpected
                out->err(out, "error: Lost connection to pacemakerd");
            }
            event_done(data, pacemakerd_api);
            return;

        case pcmk_ipc_event_reply:
            break;

        default:
            return;
    }

    if (data->message_timer_id != 0) {
        g_source_remove(data->message_timer_id);
        data->message_timer_id = 0;
    }

    if (status != CRM_EX_OK) {
        out->err(out, "error: Bad reply from pacemakerd: %s",
                crm_exit_str(status));
        event_done(data, pacemakerd_api);
        data->rc = EBADMSG;
        return;
    }

    if (reply->reply_type != pcmk_pacemakerd_reply_ping) {
        out->err(out, "error: Unknown reply type %d from pacemakerd",
                reply->reply_type);
        event_done(data, pacemakerd_api);
        data->rc = EBADMSG;
        return;
    }

    // Parse desired information from reply
    data->pcmkd_state = reply->data.ping.state;
    if (reply->data.ping.status == pcmk_rc_ok) {
        crm_time_t *when = crm_time_new(NULL);
        char *when_s = NULL;

        crm_time_set_timet(when, &reply->data.ping.last_good);
        when_s = crm_time_as_string(when,
                                    crm_time_log_date
                                    |crm_time_log_timeofday
                                    |crm_time_log_with_timezone);

        out->message(out, "pacemakerd-health",
                     reply->data.ping.sys_from, reply->data.ping.state, NULL,
                     when_s);

        crm_time_free(when);
        free(when_s);

    } else {
        out->message(out, "pacemakerd-health",
                     reply->data.ping.sys_from, reply->data.ping.state,
                     "query failed", NULL);
    }
    data->rc = pcmk_rc_ok;
    event_done(data, pacemakerd_api);
}

static pcmk_ipc_api_t *
ipc_connect(data_t *data, enum pcmk_ipc_server server, pcmk_ipc_callback_t cb,
            enum pcmk_ipc_dispatch dispatch_type, bool eremoteio_ok)
{
    int rc;
    pcmk__output_t *out = data->out;
    pcmk_ipc_api_t *api = NULL;

    rc = pcmk_new_ipc_api(&api, server);
    if (api == NULL) {
        out->err(out, "error: Could not connect to %s: %s",
                pcmk_ipc_name(api, true),
                pcmk_rc_str(rc));
        data->rc = rc;
        return NULL;
    }
    if (cb != NULL) {
        pcmk_register_ipc_callback(api, cb, data);
    }

    rc = pcmk_connect_ipc(api, dispatch_type);
    if (rc != pcmk_rc_ok) {
        if ((rc == EREMOTEIO) && eremoteio_ok) {
            /* EREMOTEIO may be expected and acceptable for some callers.
             * Preserve the return code in case callers need to handle it
             * specially.
             */
        } else {
            out->err(out, "error: Could not connect to %s: %s",
                     pcmk_ipc_name(api, true), pcmk_rc_str(rc));
        }
        data->rc = rc;
        pcmk_free_ipc_api(api);
        return NULL;
    }

    return api;
}

/*!
 * \internal
 * \brief Get and output controller status
 *
 * \param[in,out] out                 Output object
 * \param[in]     node_name           Name of node whose status is desired
 *                                    (\p NULL for DC)
 * \param[in]     message_timeout_ms  How long to wait for a reply from the
 *                                    \p pacemaker-controld API. If 0,
 *                                    \p pcmk_ipc_dispatch_sync will be used.
 *                                    Otherwise, \p pcmk_ipc_dispatch_main will
 *                                    be used, and a new mainloop will be
 *                                    created for this purpose (freed before
 *                                    return).
 *
 * \return Standard Pacemaker return code
 */
int
pcmk__controller_status(pcmk__output_t *out, const char *node_name,
                        guint message_timeout_ms)
{
    data_t data = {
        .out = out,
        .mainloop = NULL,
        .rc = pcmk_rc_ok,
        .message_timer_id = 0,
        .message_timeout_ms = message_timeout_ms,
        .pcmkd_state = pcmk_pacemakerd_state_invalid,
    };
    enum pcmk_ipc_dispatch dispatch_type = pcmk_ipc_dispatch_main;
    pcmk_ipc_api_t *controld_api = NULL;

    if (message_timeout_ms == 0) {
        dispatch_type = pcmk_ipc_dispatch_sync;
    }
    controld_api = ipc_connect(&data, pcmk_ipc_controld,
                               controller_status_event_cb, dispatch_type,
                               false);

    if (controld_api != NULL) {
        int rc = pcmk_controld_api_ping(controld_api, node_name);
        if (rc != pcmk_rc_ok) {
            out->err(out, "error: Could not ping controller API on %s: %s",
                     pcmk__s(node_name, "DC"), pcmk_rc_str(rc));
            data.rc = rc;
        }

        if (dispatch_type == pcmk_ipc_dispatch_main) {
            start_main_loop(&data);
        }

        pcmk_free_ipc_api(controld_api);
    }

    return data.rc;
}

// Documented in header
int
pcmk_controller_status(xmlNodePtr *xml, const char *node_name,
                       unsigned int message_timeout_ms)
{
    pcmk__output_t *out = NULL;
    int rc = pcmk_rc_ok;

    rc = pcmk__xml_output_new(&out, xml);
    if (rc != pcmk_rc_ok) {
        return rc;
    }

    pcmk__register_lib_messages(out);

    rc = pcmk__controller_status(out, node_name, (guint) message_timeout_ms);
    pcmk__xml_output_finish(out, xml);
    return rc;
}

/*!
 * \internal
 * \brief Get and output designated controller node name
 *
 * \param[in,out] out                 Output object
 * \param[in]     message_timeout_ms  How long to wait for a reply from the
 *                                    \p pacemaker-controld API. If 0,
 *                                    \p pcmk_ipc_dispatch_sync will be used.
 *                                    Otherwise, \p pcmk_ipc_dispatch_main will
 *                                    be used, and a new mainloop will be
 *                                    created for this purpose (freed before
 *                                    return).
 *
 * \return Standard Pacemaker return code
 */
int
pcmk__designated_controller(pcmk__output_t *out, guint message_timeout_ms)
{
    data_t data = {
        .out = out,
        .mainloop = NULL,
        .rc = pcmk_rc_ok,
        .message_timer_id = 0,
        .message_timeout_ms = message_timeout_ms,
        .pcmkd_state = pcmk_pacemakerd_state_invalid,
    };
    enum pcmk_ipc_dispatch dispatch_type = pcmk_ipc_dispatch_main;
    pcmk_ipc_api_t *controld_api = NULL;

    if (message_timeout_ms == 0) {
        dispatch_type = pcmk_ipc_dispatch_sync;
    }
    controld_api = ipc_connect(&data, pcmk_ipc_controld,
                               designated_controller_event_cb, dispatch_type,
                               false);

    if (controld_api != NULL) {
        int rc = pcmk_controld_api_ping(controld_api, NULL);
        if (rc != pcmk_rc_ok) {
            out->err(out, "error: Could not ping controller API on DC: %s",
                     pcmk_rc_str(rc));
            data.rc = rc;
        }

        if (dispatch_type == pcmk_ipc_dispatch_main) {
            start_main_loop(&data);
        }

        pcmk_free_ipc_api(controld_api);
    }

    return data.rc;
}

// Documented in header
int
pcmk_designated_controller(xmlNodePtr *xml, unsigned int message_timeout_ms)
{
    pcmk__output_t *out = NULL;
    int rc = pcmk_rc_ok;

    rc = pcmk__xml_output_new(&out, xml);
    if (rc != pcmk_rc_ok) {
        return rc;
    }

    pcmk__register_lib_messages(out);

    rc = pcmk__designated_controller(out, (guint) message_timeout_ms);
    pcmk__xml_output_finish(out, xml);
    return rc;
}

/*!
 * \internal
 * \brief Get and output \p pacemakerd status
 *
 * \param[in,out] out                 Output object
 * \param[in]     ipc_name            IPC name for request
 * \param[in]     message_timeout_ms  How long to wait for a reply from the
 *                                    \p pacemakerd API. If 0,
 *                                    \p pcmk_ipc_dispatch_sync will be used.
 *                                    If positive, \p pcmk_ipc_dispatch_main
 *                                    will be used, and a new mainloop will be
 *                                    created for this purpose (freed before
 *                                    return).
 * \param[out]    state               Where to store the \p pacemakerd state, if
 *                                    not \p NULL
 *
 * \return Standard Pacemaker return code
 *
 * \note This function returns \p EREMOTEIO if run on a Pacemaker Remote node
 *       with \p pacemaker-remoted running, since \p pacemakerd is not proxied
 *       to remote nodes. The fencer and CIB may still be accessible, but
 *       \p state will be \p pcmk_pacemakerd_state_invalid.
 */
int
pcmk__pacemakerd_status(pcmk__output_t *out, const char *ipc_name,
                        guint message_timeout_ms,
                        enum pcmk_pacemakerd_state *state)
{
    data_t data = {
        .out = out,
        .mainloop = NULL,
        .rc = pcmk_rc_ipc_unresponsive,
        .message_timer_id = 0,
        .message_timeout_ms = message_timeout_ms,
        .pcmkd_state = pcmk_pacemakerd_state_invalid,
    };
    enum pcmk_ipc_dispatch dispatch_type = pcmk_ipc_dispatch_main;
    pcmk_ipc_api_t *pacemakerd_api = NULL;

    if (message_timeout_ms == 0) {
        dispatch_type = pcmk_ipc_dispatch_sync;
    }
    pacemakerd_api = ipc_connect(&data, pcmk_ipc_pacemakerd,
                                 pacemakerd_event_cb, dispatch_type, true);

    if (pacemakerd_api != NULL) {
        int rc = pcmk_pacemakerd_api_ping(pacemakerd_api, ipc_name);
        if (rc != pcmk_rc_ok) {
            out->err(out, "error: Could not ping launcher API: %s",
                     pcmk_rc_str(rc));
            data.rc = rc;
        }

        if (dispatch_type == pcmk_ipc_dispatch_main) {
            start_main_loop(&data);
        }
        pcmk_free_ipc_api(pacemakerd_api);
    }

    if (state != NULL) {
        *state = data.pcmkd_state;
    }
    return data.rc;
}

// Documented in header
int
pcmk_pacemakerd_status(xmlNodePtr *xml, const char *ipc_name,
                       unsigned int message_timeout_ms)
{
    pcmk__output_t *out = NULL;
    int rc = pcmk_rc_ok;

    rc = pcmk__xml_output_new(&out, xml);
    if (rc != pcmk_rc_ok) {
        return rc;
    }

    pcmk__register_lib_messages(out);

    rc = pcmk__pacemakerd_status(out, ipc_name, (guint) message_timeout_ms,
                                 NULL);
    pcmk__xml_output_finish(out, xml);
    return rc;
}

/* user data for looping through remote node xpath searches */
struct node_data {
    pcmk__output_t *out;
    int found;
    const char *field;  /* XML attribute to check for node name */
    const char *type;
    gboolean bash_export;
};

static void
remote_node_print_helper(xmlNode *result, void *user_data)
{
    struct node_data *data = user_data;
    pcmk__output_t *out = data->out;
    const char *name = crm_element_value(result, XML_ATTR_UNAME);
    const char *id = crm_element_value(result, data->field);

    // node name and node id are the same for remote/guest nodes
    out->message(out, "crmadmin-node", data->type,
                 name ? name : id,
                 id,
                 data->bash_export);
    data->found++;
}

// \return Standard Pacemaker return code
int
pcmk__list_nodes(pcmk__output_t *out, char *node_types, gboolean bash_export)
{
    xmlNode *xml_node = NULL;
    int rc;

    rc = cib__signon_query(NULL, &xml_node);

    if (rc == pcmk_rc_ok) {
        struct node_data data = {
            .out = out,
            .found = 0,
            .bash_export = bash_export
        };

        out->begin_list(out, NULL, NULL, "nodes");

        if (!pcmk__str_empty(node_types) && strstr(node_types, "all")) {
            node_types = NULL;
        }

        if (pcmk__str_empty(node_types) || strstr(node_types, "cluster")) {
            data.field = "id";
            data.type = "cluster";
            crm_foreach_xpath_result(xml_node, PCMK__XP_MEMBER_NODE_CONFIG,
                                     remote_node_print_helper, &data);
        }

        if (pcmk__str_empty(node_types) || strstr(node_types, "guest")) {
            data.field = "value";
            data.type = "guest";
            crm_foreach_xpath_result(xml_node, PCMK__XP_GUEST_NODE_CONFIG,
                                     remote_node_print_helper, &data);
        }

        if (pcmk__str_empty(node_types) || !pcmk__strcmp(node_types, ",|^remote", pcmk__str_regex)) {
            data.field = "id";
            data.type = "remote";
            crm_foreach_xpath_result(xml_node, PCMK__XP_REMOTE_NODE_CONFIG,
                                     remote_node_print_helper, &data);
        }

        out->end_list(out);

        if (data.found == 0) {
            out->info(out, "No nodes configured");
        }

        free_xml(xml_node);
    }

    return rc;
}

int
pcmk_list_nodes(xmlNodePtr *xml, char *node_types)
{
    pcmk__output_t *out = NULL;
    int rc = pcmk_rc_ok;

    rc = pcmk__xml_output_new(&out, xml);
    if (rc != pcmk_rc_ok) {
        return rc;
    }

    pcmk__register_lib_messages(out);

    rc = pcmk__list_nodes(out, node_types, FALSE);
    pcmk__xml_output_finish(out, xml);
    return rc;
}
