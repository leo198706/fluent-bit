#include <fcntl.h>

#include <msgpack.h>
#include <chunkio/chunkio.h>

#include <fluent-bit/flb_input_chunk.h>
#include <fluent-bit/flb_input.h>
#include <fluent-bit/flb_output.h>
#include <fluent-bit/flb_trace_chunk.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_base64.h>
#include <fluent-bit/flb_storage.h>
#include <fluent-bit/flb_router.h>
#include <fluent-bit/flb_kv.h>


/* Register external function to emit records, check 'plugins/in_emitter' */
int in_emitter_add_record(const char *tag, int tag_len,
                          const char *buf_data, size_t buf_size,
                          struct flb_input_instance *in);

// To avoid double frees when enabling and disabling tracing as well
// as avoiding race conditions when stopping fluent-bit while someone is
// toggling tracing via the HTTP API this set of APIS with a mutex lock 
// is used:
//   * flb_trace_chunk_to_be_destroyed - query to see if the trace context
//     is slated to be freed
//   * flb_trace_chunk_set_destroy - set the trace context to be destroyed
//     once all chunks are freed (executed in flb_trace_chunk_destroy).
//   * flb_trace_chunk_has_chunks - see if there are still chunks using
//     using the tracing context
//   * flb_trace_chunk_add - increment the traces chunk count
//   * flb_trace_chunk_sub - decrement the traces chunk count
static inline int flb_trace_chunk_to_be_destroyed(struct flb_trace_chunk_context *ctxt)
{
    int ret = FLB_FALSE;

    ret = (ctxt->to_destroy == 1 ? FLB_TRUE : FLB_FALSE);
    return ret;
}

static inline int flb_trace_chunk_has_chunks(struct flb_trace_chunk_context *ctxt)
{
    int ret = FLB_FALSE;

    ret = ((ctxt->chunks > 0) ? FLB_TRUE : FLB_FALSE);
    return ret;
}

static inline void flb_trace_chunk_add(struct flb_trace_chunk_context *ctxt)
{
    ctxt->chunks++;
}

static inline void flb_trace_chunk_sub(struct flb_trace_chunk_context *ctxt)
{
    ctxt->chunks--;
}

static inline void flb_trace_chunk_set_destroy(struct flb_trace_chunk_context *ctxt)
{
    ctxt->to_destroy = 1;
}

static struct flb_output_instance *find_calyptia_output_instance(struct flb_config *config)
{
    struct mk_list *head;
    struct flb_output_instance *output;

    mk_list_foreach(head, &config->outputs) {
        output = mk_list_entry(head, struct flb_output_instance, _head);
        if (strcmp(output->p->name, "calyptia") == 0) {
            return output;
        }
    }
    return NULL;
}

static void log_cb(void *ctx, int level, const char *file, int line,
                   const char *str)
{
    if (level == CIO_LOG_ERROR) {
        flb_error("[trace] %s", str);
    }
    else if (level == CIO_LOG_WARN) {
        flb_warn("[trace] %s", str);
    }
    else if (level == CIO_LOG_INFO) {
        flb_info("[trace] %s", str);
    }
    else if (level == CIO_LOG_DEBUG) {
        flb_debug("[trace] %s", str);
    }
}

static void trace_chunk_context_destroy(void *input)
{
    struct flb_input_instance *in = (struct flb_input_instance *)input;
    struct flb_trace_chunk_context *ctxt = in->trace_ctxt;

    if (ctxt == NULL) {
        return;
    }

    in->trace_ctxt = NULL;

    if (flb_trace_chunk_has_chunks(ctxt) == FLB_TRUE) {
        flb_trace_chunk_set_destroy(ctxt);
        // maybe take out?
        flb_input_pause(ctxt->input);
        return;
    }
    
    flb_sds_destroy(ctxt->trace_prefix);
    flb_stop(ctxt->flb);
    flb_destroy(ctxt->flb);
    cio_destroy(ctxt->cio);
    flb_free(ctxt);

    in->trace_ctxt = NULL;
}

void flb_trace_chunk_context_destroy(void *input)
{
    struct flb_input_instance *in = (struct flb_input_instance *)input;
    pthread_mutex_lock(&in->trace_lock);
    trace_chunk_context_destroy(input);
    pthread_mutex_unlock(&in->trace_lock);
}

struct flb_trace_chunk_context *flb_trace_chunk_context_new(void *trace_input, const char *output_name, const char *trace_prefix, void *data, struct mk_list *props)
{
    struct flb_input_instance *in = (struct flb_input_instance *)trace_input;
    struct flb_config *config = in->config;
    pthread_mutexattr_t attr = {0};
    struct flb_input_instance *input;
    struct flb_output_instance *output;
    struct flb_output_instance *calyptia;
    struct flb_trace_chunk_context *ctx;
    struct mk_list *head;
    struct flb_kv *prop;
    struct cio_options opts = {0};
    int ret;

    if (config->enable_trace == FLB_FALSE) {
        return NULL;
    }

    pthread_mutex_lock(&in->trace_lock);

    ctx = flb_calloc(1, sizeof(struct flb_trace_chunk_context));
    if (ctx == NULL) {
        pthread_mutex_unlock(&in->trace_lock);
        return NULL;
    }

    ctx->flb = flb_create();
    if (ctx->flb == NULL) {
        goto error_ctxt;
    }

    flb_service_set(ctx->flb, "flush", "1", "grace", "1", NULL);

    input = (void *)flb_input_new(ctx->flb->config, "emitter", NULL, FLB_FALSE);
    if (input == NULL) {
        flb_error("could not load trace emitter");
        goto error_flb;
    }
    input->event_type = FLB_EVENT_TYPE_LOG | FLB_EVENT_TYPE_HAS_TRACE;

    // create our own chunk context so we do not interfere with the
    // global chunk context.
    ctx->cio = cio_create(NULL);
    if (ctx->cio == NULL) {
    	flb_error("unable to create cio context");
    	goto error_flb;
    }
    flb_storage_input_create(ctx->cio, input);

    ret = flb_input_set_property(input, "alias", "trace-emitter");
    if (ret != 0) {
        flb_error("unable to set alias for trace emitter");
        goto error_input;
    }

    output = flb_output_new(ctx->flb->config, output_name, data, 1);
    if (output == NULL) {
        flb_error("could not create trace output");
        goto error_input;
    }
    
    // special handling for the calyptia plugin so we can copy the API
    // key and other configuration properties.
    if (strcmp(output_name, "calyptia") == 0) {
        calyptia = find_calyptia_output_instance(config);
        if (calyptia == NULL) {
            flb_error("unable to find calyptia output instance");
            goto error_output;
        }
        mk_list_foreach(head, &calyptia->properties) {
            prop = mk_list_entry(head, struct flb_kv, _head);
            flb_output_set_property(output, prop->key, prop->val);
        }        
    } else if (props != NULL) {
        mk_list_foreach(head, props) {
            prop = mk_list_entry(head, struct flb_kv, _head);
            flb_output_set_property(output, prop->key, prop->val);
        }
    }

    ret = flb_router_connect_direct(input, output);
    if (ret != 0) {
        flb_error("unable to route traces");
        goto error_output;
    }

    ctx->output = (void *)output;
    ctx->input = (void *)input;
    ctx->trace_prefix = flb_sds_create(trace_prefix);

    flb_start(ctx->flb);
    
    in->trace_ctxt = ctx;
    pthread_mutex_unlock(&in->trace_lock);
    return ctx;

error_output:
    flb_output_instance_destroy(output);
error_input:
    if (ctx->cio) {
        cio_destroy(ctx->cio);
    }
    flb_input_instance_destroy(input);
error_flb:
    flb_destroy(ctx->flb);
error_ctxt:
    flb_free(ctx);
    pthread_mutex_unlock(&in->trace_lock);
    return NULL;
}

struct flb_trace_chunk *flb_trace_chunk_new(struct flb_input_chunk *chunk)
{
    struct flb_trace_chunk *trace;
    struct flb_input_instance *f_ins = (struct flb_input_instance *)chunk->in;

    pthread_mutex_lock(&f_ins->trace_lock);

    if (flb_trace_chunk_to_be_destroyed(f_ins->trace_ctxt) == FLB_TRUE) {
        pthread_mutex_unlock(&f_ins->trace_lock);
        return NULL;
    }

    trace = flb_calloc(1, sizeof(struct flb_trace_chunk));
    if (trace == NULL) {
        pthread_mutex_unlock(&f_ins->trace_lock);
        return NULL;
    }

    trace->ctxt = f_ins->trace_ctxt;
    flb_trace_chunk_add(trace->ctxt);

    trace->ic = chunk;
    trace->trace_id = flb_sds_create("");
    flb_sds_printf(&trace->trace_id, "%s%d", trace->ctxt->trace_prefix,
                  trace->ctxt->trace_count++);

    pthread_mutex_unlock(&f_ins->trace_lock);
    return trace;
}

void flb_trace_chunk_destroy(struct flb_trace_chunk *trace)
{    
    pthread_mutex_lock(&trace->ic->in->trace_lock);
    flb_trace_chunk_sub(trace->ctxt);

    // check to see if we need to free the trace context.
    if (flb_trace_chunk_has_chunks(trace->ctxt) == FLB_FALSE &&
        flb_trace_chunk_to_be_destroyed(trace->ctxt) == FLB_TRUE) {
        trace_chunk_context_destroy(trace->ic->in);
    }
    pthread_mutex_unlock(&trace->ic->in->trace_lock);

    flb_sds_destroy(trace->trace_id);
    flb_free(trace);
}

int flb_trace_chunk_context_set_limit(void *input, int limit_type, int limit_arg)
{
    struct flb_input_instance *in = (struct flb_input_instance *)input;
    struct flb_trace_chunk_context *ctxt;
    struct flb_time tm;

    pthread_mutex_lock(&in->trace_lock);

    ctxt = in->trace_ctxt;
    if (ctxt == NULL) {
        pthread_mutex_unlock(&in->trace_lock);
        return -1;
    }

    switch(limit_type) {
    case FLB_TRACE_CHUNK_LIMIT_TIME:
        flb_time_get(&tm);
        ctxt->limit.type = FLB_TRACE_CHUNK_LIMIT_TIME;
        ctxt->limit.seconds_started = tm.tm.tv_sec;
        ctxt->limit.seconds = limit_arg;
        
        pthread_mutex_unlock(&in->trace_lock);
        return 0;
    case FLB_TRACE_CHUNK_LIMIT_COUNT:
        ctxt->limit.type = FLB_TRACE_CHUNK_LIMIT_COUNT;
        ctxt->limit.count = limit_arg;

        pthread_mutex_unlock(&in->trace_lock);
        return 0;
    }

    pthread_mutex_unlock(&in->trace_lock);
    return -1;
}

int flb_trace_chunk_context_hit_limit(void *input)
{
    struct flb_input_instance *in = (struct flb_input_instance *)input;
    struct flb_time tm;
    struct flb_trace_chunk_context *ctxt;

    pthread_mutex_lock(&in->trace_lock);

    ctxt = in->trace_ctxt;
    if (ctxt == NULL) {
        pthread_mutex_unlock(&in->trace_lock);
        return FLB_FALSE;
    }

    switch(ctxt->limit.type) {
    case FLB_TRACE_CHUNK_LIMIT_TIME:
        flb_time_get(&tm);
        if ((tm.tm.tv_sec - ctxt->limit.seconds_started) > ctxt->limit.seconds) {
            pthread_mutex_unlock(&in->trace_lock);
            return FLB_TRUE;
        }
        return FLB_FALSE;
    case FLB_TRACE_CHUNK_LIMIT_COUNT:
        if (ctxt->limit.count <= ctxt->trace_count) {
            pthread_mutex_unlock(&in->trace_lock);
            return FLB_TRUE;
        }
        pthread_mutex_unlock(&in->trace_lock);
        return FLB_FALSE;
    }
    pthread_mutex_unlock(&in->trace_lock);
    return FLB_FALSE;
}

void flb_trace_chunk_do_input(struct flb_input_chunk *ic)
{
    pthread_mutex_lock(&ic->in->trace_lock);
    if (ic->in->trace_ctxt == NULL) {
        pthread_mutex_unlock(&ic->in->trace_lock);
    	return;
    }
    pthread_mutex_unlock(&ic->in->trace_lock);
    
    if (ic->trace == NULL) {
        ic->trace = flb_trace_chunk_new(ic);
    }

    if (ic->trace) {
        flb_trace_chunk_input(ic->trace);
        if (flb_trace_chunk_context_hit_limit(ic->in) == FLB_TRUE) {
            flb_trace_chunk_context_destroy(ic->in);
        }
    }
}

int flb_trace_chunk_input(struct flb_trace_chunk *trace)
{
    msgpack_packer mp_pck;
    msgpack_sbuffer mp_sbuf;
    msgpack_unpacked result;
    msgpack_object *record;
    char *buf;
    size_t buf_size;
    struct flb_time tm;
    struct flb_time tm_end;
    struct flb_input_instance *input = (struct flb_input_instance *)trace->ic->in;
    int rc = -1;
    size_t off = 0;
    flb_sds_t tag = flb_sds_create("trace");
    int records = 0;


    // initiailize start time
    flb_time_get(&tm);
    flb_time_get(&tm_end);

    msgpack_sbuffer_init(&mp_sbuf);
    msgpack_packer_init(&mp_pck, &mp_sbuf, msgpack_sbuffer_write);
    msgpack_unpacked_init(&result);

    cio_chunk_get_content(trace->ic->chunk, &buf, &buf_size);
    
    msgpack_pack_array(&mp_pck, 2);
    flb_pack_time_now(&mp_pck);
    if (input->alias != NULL) {
        msgpack_pack_map(&mp_pck, 7);
    } 
    else {
        msgpack_pack_map(&mp_pck, 6);
    }

    msgpack_pack_str_with_body(&mp_pck, "type", 4);
    msgpack_pack_int(&mp_pck, FLB_TRACE_CHUNK_TYPE_INPUT);

    msgpack_pack_str_with_body(&mp_pck, "trace_id", strlen("trace_id"));
    msgpack_pack_str_with_body(&mp_pck, trace->trace_id, strlen(trace->trace_id));

    msgpack_pack_str_with_body(&mp_pck, "plugin_instance", strlen("plugin_instance"));
    msgpack_pack_str_with_body(&mp_pck, input->name, strlen(input->name));

    if (input->alias != NULL) {
        msgpack_pack_str_with_body(&mp_pck, "plugin_alias", strlen("plugin_alias"));
        msgpack_pack_str_with_body(&mp_pck, input->alias, strlen(input->alias));
    }

    msgpack_pack_str_with_body(&mp_pck, "records", strlen("records"));

    do {
        rc = msgpack_unpack_next(&result, buf, buf_size, &off);
        if (rc != MSGPACK_UNPACK_SUCCESS) {
            flb_error("unable to unpack record");
            goto sbuffer_error;
        }
        records++;
    } while (rc == MSGPACK_UNPACK_SUCCESS && off < buf_size);

    msgpack_pack_array(&mp_pck, records);

    off = 0;
    do {
        rc = msgpack_unpack_next(&result, buf, buf_size, &off);
        if (rc != MSGPACK_UNPACK_SUCCESS) {
            flb_error("unable to unpack record");
            goto sbuffer_error;
        }
        flb_time_pop_from_msgpack(&tm, &result, &record);

        msgpack_pack_map(&mp_pck, 2);
        msgpack_pack_str_with_body(&mp_pck, "timestamp", strlen("timestamp"));
        flb_time_append_to_msgpack(&tm, &mp_pck, FLB_TIME_ETFMT_INT);
        msgpack_pack_str_with_body(&mp_pck, "record", strlen("record"));
        msgpack_pack_object(&mp_pck, *record);

    } while (rc == MSGPACK_UNPACK_SUCCESS && off < buf_size);

    msgpack_pack_str_with_body(&mp_pck, "start_time", strlen("start_time"));
    flb_time_append_to_msgpack(&tm, &mp_pck, FLB_TIME_ETFMT_INT);
    msgpack_pack_str_with_body(&mp_pck, "end_time", strlen("end_time"));
    flb_time_append_to_msgpack(&tm_end, &mp_pck, FLB_TIME_ETFMT_INT);
    in_emitter_add_record(tag, flb_sds_len(tag), mp_sbuf.data, mp_sbuf.size,
                          trace->ctxt->input);
sbuffer_error:
    flb_sds_destroy(tag);
    msgpack_unpacked_destroy(&result);
    msgpack_sbuffer_destroy(&mp_sbuf);
    return rc;
}

int flb_trace_chunk_pre_output(struct flb_trace_chunk *trace)
{
    msgpack_packer mp_pck;
    msgpack_sbuffer mp_sbuf;
    msgpack_unpacked result;
    msgpack_object *record;
    char *buf;
    size_t buf_size;
    struct flb_time tm;
    struct flb_time tm_end;
    struct flb_input_instance *input = (struct flb_input_instance *)trace->ic->in;
    int rc = -1;
    size_t off = 0;
    flb_sds_t tag = flb_sds_create("trace");
    int records = 0;


    // initiailize start time
    flb_time_get(&tm);
    flb_time_get(&tm_end);

    msgpack_sbuffer_init(&mp_sbuf);
    msgpack_packer_init(&mp_pck, &mp_sbuf, msgpack_sbuffer_write);
    msgpack_unpacked_init(&result);

    cio_chunk_get_content(trace->ic->chunk, &buf, &buf_size);

    msgpack_pack_array(&mp_pck, 2);
    flb_pack_time_now(&mp_pck);
    if (input->alias != NULL) {
        msgpack_pack_map(&mp_pck, 7);
    } 
    else {
        msgpack_pack_map(&mp_pck, 6);
    }

    msgpack_pack_str_with_body(&mp_pck, "type", 4);
    msgpack_pack_int(&mp_pck, FLB_TRACE_CHUNK_TYPE_PRE_OUTPUT);

    msgpack_pack_str_with_body(&mp_pck, "trace_id", strlen("trace_id"));
    msgpack_pack_str_with_body(&mp_pck, trace->trace_id, strlen(trace->trace_id));

    msgpack_pack_str_with_body(&mp_pck, "plugin_instance", strlen("plugin_instance"));
    msgpack_pack_str_with_body(&mp_pck, input->name, strlen(input->name));

    if (input->alias != NULL) {
        msgpack_pack_str_with_body(&mp_pck, "plugin_alias", strlen("plugin_alias"));
        msgpack_pack_str_with_body(&mp_pck, input->alias, strlen(input->alias));
    }

    msgpack_pack_str_with_body(&mp_pck, "records", strlen("records"));

    do {
        rc = msgpack_unpack_next(&result, buf, buf_size, &off);
        if (rc != MSGPACK_UNPACK_SUCCESS) {
            flb_error("unable to unpack record");
            goto sbuffer_error;
        }
        records++;
    } while (rc == MSGPACK_UNPACK_SUCCESS && off < buf_size);

    msgpack_pack_array(&mp_pck, records);
    off = 0;
    do {
        rc = msgpack_unpack_next(&result, buf, buf_size, &off);
        if (rc != MSGPACK_UNPACK_SUCCESS) {
            flb_error("unable to unpack record");
            goto sbuffer_error;
        }
        flb_time_pop_from_msgpack(&tm, &result, &record);

        msgpack_pack_map(&mp_pck, 2);
        msgpack_pack_str_with_body(&mp_pck, "timestamp", strlen("timestamp"));
        flb_time_append_to_msgpack(&tm, &mp_pck, FLB_TIME_ETFMT_INT);
        msgpack_pack_str_with_body(&mp_pck, "record", strlen("record"));
        msgpack_pack_object(&mp_pck, *record);

    } while (rc == MSGPACK_UNPACK_SUCCESS && off < buf_size);

    msgpack_pack_str_with_body(&mp_pck, "start_time", strlen("start_time"));
    flb_time_append_to_msgpack(&tm, &mp_pck, FLB_TIME_ETFMT_INT);
    msgpack_pack_str_with_body(&mp_pck, "end_time", strlen("end_time"));
    flb_time_append_to_msgpack(&tm_end, &mp_pck, FLB_TIME_ETFMT_INT);
    in_emitter_add_record(tag, flb_sds_len(tag), mp_sbuf.data, mp_sbuf.size,
                          trace->ctxt->input);
sbuffer_error:
    flb_sds_destroy(tag);
    msgpack_unpacked_destroy(&result);
    msgpack_sbuffer_destroy(&mp_sbuf);
    return rc;
}

int flb_trace_chunk_filter(struct flb_trace_chunk *tracer, void *pfilter, struct flb_time *tm_start, struct flb_time *tm_end, char *buf, size_t buf_size)
{
    msgpack_packer mp_pck;
    msgpack_sbuffer mp_sbuf;
    msgpack_unpacked result;
    msgpack_object *record;
    int rc = -1;
    struct flb_filter_instance *filter = (struct flb_filter_instance *)pfilter;
    flb_sds_t tag = flb_sds_create("trace");
    struct flb_time tm;
    size_t off = 0;
    int records = 0;


    if (tracer == NULL) {
        goto tracer_error;
    }

    msgpack_sbuffer_init(&mp_sbuf);
    msgpack_packer_init(&mp_pck, &mp_sbuf, msgpack_sbuffer_write);

    msgpack_pack_array(&mp_pck, 2);
    flb_pack_time_now(&mp_pck);
    if (filter->alias == NULL) {
        msgpack_pack_map(&mp_pck, 6);
    }
    else {
        msgpack_pack_map(&mp_pck, 7);	
    }

    msgpack_pack_str_with_body(&mp_pck, "type", strlen("type"));
    rc = msgpack_pack_int(&mp_pck, FLB_TRACE_CHUNK_TYPE_FILTER);
    if (rc == -1) {
        goto sbuffer_error;
    }

    msgpack_pack_str_with_body(&mp_pck, "start_time", strlen("start_time"));
    //msgpack_pack_double(&mp_pck, flb_time_to_double(tm_start));
    flb_time_append_to_msgpack(tm_start, &mp_pck, FLB_TIME_ETFMT_INT);
    msgpack_pack_str_with_body(&mp_pck, "end_time", strlen("end_time"));
    //msgpack_pack_double(&mp_pck, flb_time_to_double(tm_end));
    flb_time_append_to_msgpack(tm_end, &mp_pck, FLB_TIME_ETFMT_INT);

    msgpack_pack_str_with_body(&mp_pck, "trace_id", strlen("trace_id"));
    msgpack_pack_str_with_body(&mp_pck, tracer->trace_id, strlen(tracer->trace_id));

    
    msgpack_pack_str_with_body(&mp_pck, "plugin_instance", strlen("plugin_instance"));
    rc = msgpack_pack_str_with_body(&mp_pck, filter->name, strlen(filter->name));
    if (rc == -1) {
        goto sbuffer_error;
    }
    
    if (filter->alias != NULL) {
        msgpack_pack_str_with_body(&mp_pck, "plugin_alias", strlen("plugin_alias"));
        msgpack_pack_str_with_body(&mp_pck, filter->alias, strlen(filter->alias));
    }

    msgpack_pack_str_with_body(&mp_pck, "records", strlen("records"));

    msgpack_unpacked_init(&result);
    do {
        rc = msgpack_unpack_next(&result, buf, buf_size, &off);
        if (rc != MSGPACK_UNPACK_SUCCESS) {
            flb_error("unable to unpack record");
            goto unpack_error;
        }
        records++;
    } while (rc == MSGPACK_UNPACK_SUCCESS && off < buf_size);

    msgpack_pack_array(&mp_pck, records);
    off = 0;
    do {
        rc = msgpack_unpack_next(&result, buf, buf_size, &off);
        if (rc != MSGPACK_UNPACK_SUCCESS) {
            flb_error("unable to unpack record");
            goto unpack_error;
        }
        flb_time_pop_from_msgpack(&tm, &result, &record);

        msgpack_pack_map(&mp_pck, 2);
        msgpack_pack_str_with_body(&mp_pck, "timestamp", strlen("timestamp"));
        flb_time_append_to_msgpack(&tm, &mp_pck, FLB_TIME_ETFMT_INT);
        msgpack_pack_str_with_body(&mp_pck, "record", strlen("record"));
        msgpack_pack_object(&mp_pck, *record);

    } while (rc == MSGPACK_UNPACK_SUCCESS && off < buf_size);

    in_emitter_add_record(tag, flb_sds_len(tag), mp_sbuf.data, mp_sbuf.size,
                          tracer->ctxt->input);
    
    rc = 0;

unpack_error:
    msgpack_unpacked_destroy(&result);
sbuffer_error:
    msgpack_sbuffer_destroy(&mp_sbuf);
tracer_error:
    flb_sds_destroy(tag);
    return rc;
}
