/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stddef.h>

#include <apr_atomic.h>
#include <apr_strings.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>
#include <http_connection.h>

#include "h2_private.h"
#include "h2_bucket.h"
#include "h2_mplx.h"
#include "h2_session.h"
#include "h2_stream.h"
#include "h2_task_input.h"
#include "h2_task_output.h"
#include "h2_task.h"
#include "h2_ctx.h"

struct h2_task {
    long session_id;
    int stream_id;
    int aborted;
    apr_uint32_t running;
    
    h2_mplx *mplx;
    apr_pool_t *pool;
    conn_rec *c;
    apr_socket_t *socket;
    
    struct h2_task_input *input;    /* http/1.1 input data */
    struct h2_task_output *output;  /* response body data */
};

static ap_filter_rec_t *h2_input_filter_handle;
static ap_filter_rec_t *h2_output_filter_handle;

static apr_status_t h2_filter_stream_input(ap_filter_t* filter,
                                           apr_bucket_brigade* brigade,
                                           ap_input_mode_t mode,
                                           apr_read_type_e block,
                                           apr_off_t readbytes) {
    h2_task *task = (h2_task *)filter->ctx;
    assert(task);
    if (!task->input) {
        return APR_ECONNABORTED;
    }
    return h2_task_input_read(task->input, filter, brigade,
                              mode, block, readbytes);
}

static apr_status_t h2_filter_stream_output(ap_filter_t* filter,
                                            apr_bucket_brigade* brigade) {
    h2_task *task = (h2_task *)filter->ctx;
    assert(task);
    if (!task->output) {
        return APR_ECONNABORTED;
    }
    return h2_task_output_write(task->output, filter, brigade);
}


void h2_task_register_hooks(void)
{
    h2_input_filter_handle =
    ap_register_input_filter("H2_TO_HTTP", h2_filter_stream_input,
                             NULL, AP_FTYPE_NETWORK);
    
    h2_output_filter_handle =
    ap_register_output_filter("HTTP_TO_H2", h2_filter_stream_output,
                              NULL, AP_FTYPE_NETWORK);
}

int h2_task_pre_conn(h2_task *task, conn_rec *c)
{
    /* Add our own, network level in- and output filters.
     * These will take input from the h2_session->request_data
     * bucket queue and place the output into the
     * h2_session->response_data bucket queue.
     */
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                  "h2_stream(%ld-%d): task_pre_conn, installing filters",
                  task->session_id, task->stream_id);
    ap_add_input_filter_handle(h2_input_filter_handle,
                               task, NULL, c);
    ap_add_output_filter_handle(h2_output_filter_handle,
                                task, NULL, c);
    
    /* prevent processing by anyone else, including httpd core */
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                  "h2_stream(%ld-%d): task_pre_conn, taking over",
                  task->session_id, task->stream_id);
    return DONE;
}


static apr_status_t h2_conn_create(h2_task *task, conn_rec *master)
{
    /* Setup a apache connection record for this stream.
     * General idea is borrowed from mod_spdy::slave_connection.cc,
     * partly replaced with some more modern calls to ap infrastructure.
     *
     * Here, we are tasting some sweet, internal knowledge, e.g. that
     * the core module is storing the connection socket as its config.
     * "ap_run_create_connection() needs a real socket as it tries to
     * detect local and client address information and fails if it is
     * unable to get it.
     * In case someone ever replaces these core hooks, this will probably
     * break miserably.
     */
    task->socket = ap_get_module_config(master->conn_config,
                                        &core_module);
    
    task->c = ap_run_create_connection(task->pool, master->base_server,
                                       task->socket,
                                       master->id, master->sbh,
                                       apr_bucket_alloc_create(task->pool));
    if (task->c == NULL) {
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, task->pool,
                      "h2_task: creating conn failed");
        return APR_EGENERAL;
    }
    
    ap_log_perror(APLOG_MARK, APLOG_TRACE3, 0, task->pool,
                  "h2_task: created con %ld from master %ld",
                  task->c->id, master->id);
    return APR_SUCCESS;
}

h2_task *h2_task_create(long session_id,
                        int stream_id,
                        conn_rec *master,
                        apr_pool_t *pool,
                        h2_bucket *input,
                        int input_eos,
                        h2_mplx *mplx)
{
    apr_status_t status = APR_SUCCESS;
    // TODO: share pool with h2_stream, join task before destroying stream
    if (1 || pool == NULL) {
        apr_status_t status = apr_pool_create_ex(&pool, NULL, NULL, NULL);
        if (status != APR_SUCCESS) {
            h2_mplx_out_reset(mplx, stream_id, status);
            return NULL;
        }
    }
    
    h2_task *task = apr_pcalloc(pool, sizeof(h2_task));
    if (task == NULL) {
        ap_log_perror(APLOG_MARK, APLOG_ERR, APR_ENOMEM, pool,
                      "h2_task(%ld-%d): unable to create stream task",
                      session_id, stream_id);
        h2_mplx_out_reset(mplx, task->stream_id, APR_ENOMEM);
        return NULL;
    }
    
    task->stream_id = stream_id;
    task->session_id = session_id;
    task->pool = pool;
    h2_mplx_reference(mplx);
    task->mplx = mplx;
    
    status = h2_conn_create(task, master);
    if (status != APR_SUCCESS) {
        ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
                      "h2_task(%ld-%d): unable to create stream task",
                      session_id, stream_id);
        h2_mplx_out_reset(mplx, stream_id, status);
        return NULL;
    }
    
    task->input = h2_task_input_create(task->c->pool,
                                       session_id, stream_id,
                                       input, input_eos, mplx);
    task->output = h2_task_output_create(task->c->pool,
                                         session_id, stream_id, mplx);
    
    h2_ctx_create_for(task->c, task);
    
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, task->c,
                  "h2_task(%ld-%d): created", task->session_id, task->stream_id);
    return task;
}

apr_status_t h2_task_destroy(h2_task *task)
{
    assert(task);
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, task->c,
                  "h2_task(%ld-%d): destroy started",
                  task->session_id, task->stream_id);
    if (task->input) {
        h2_task_input_destroy(task->input);
        task->input = NULL;
    }
    if (task->output) {
        h2_task_output_destroy(task->output);
        task->output = NULL;
    }
    if (task->mplx) {
        h2_mplx_release(task->mplx);
        task->mplx = NULL;
    }
    if (task->pool) {
        apr_pool_destroy(task->pool);
    }
    return APR_SUCCESS;
}

apr_status_t h2_task_do(h2_task *task)
{
    assert(task);
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, task->c,
                  "h2_task(%ld-%d): do", task->session_id, task->stream_id);
    apr_status_t status;
    
    /* Furthermore, other code might want to see the socket for
     * this connection. Allocate one without further function...
     */
    
    status = apr_socket_create(&task->socket,
                               APR_INET, SOCK_STREAM,
                               APR_PROTO_TCP, task->pool);
    if (status != APR_SUCCESS) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, task->c,
                      "h2_stream_process, unable to alloc socket");
        h2_mplx_out_reset(task->mplx, task->stream_id, status);
        return status;
    }
    
    ap_set_module_config(task->c->conn_config, &core_module, task->socket);
    
    ap_process_connection(task->c, task->socket);

    if (!h2_task_output_has_started(task->output)) {
        h2_mplx_out_reset(task->mplx, task->stream_id, status);
    }
    
    apr_socket_close(task->socket);
    
    return APR_SUCCESS;
}

void h2_task_abort(h2_task *task)
{
    assert(task);
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, task->c,
                  "h2_task(%ld-%d): aborting task",
                  task->session_id, task->stream_id);
    task->aborted =  1;
    if (task->input) {
        h2_task_input_destroy(task->input);
        task->input = NULL;
    }
    if (task->output) {
        h2_task_output_destroy(task->output);
        task->output = NULL;
    }
}

long h2_task_get_session_id(h2_task *task)
{
    return task->session_id;
}

int h2_task_get_stream_id(h2_task *task)
{
    return task->stream_id;
}

int h2_task_is_running(h2_task *task)
{
    return apr_atomic_read32(&task->running);
}

void h2_task_set_running(h2_task *task, int running)
{
    apr_atomic_set32(&task->running, running);
}



