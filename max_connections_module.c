/* max connections module for nginx
 * Copyright 2008 Engine Yard, Inc. All rights reserved. 
 * Author: Ryan Dahl (ry@ndahl.us)
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_http.h>
#include <ngx_http_upstream.h>
#include <assert.h>

typedef struct {
  ngx_uint_t max_connections;
  ngx_uint_t connections;
  ngx_uint_t max_queue_length;
  ngx_uint_t queue_length;
  ngx_queue_t waiting_requests;
  ngx_event_t queue_check_event;
  ngx_msec_t queue_timeout;

  ngx_http_upstream_init_pt          original_init_upstream;
  ngx_http_upstream_init_peer_pt     original_init_peer;

} max_connections_srv_conf_t;

typedef struct {
  ngx_queue_t queue; /* queue information */

  max_connections_srv_conf_t *maxconn_cf;
  ngx_http_request_t *r; /* the request associated with the peer */

  ngx_msec_t accessed;
  ngx_uint_t processing:1;

  void *data;
  ngx_event_get_peer_pt     original_get_peer;
  ngx_event_free_peer_pt    original_free_peer;
  
  void  (*original_finalize_request)(ngx_http_request_t *r, ngx_int_t rc);
} max_connections_peer_data_t;

/* forward declarations */
static char * max_connections_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char * max_connections_queue_timeout_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char * max_connections_max_queue_length_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void * max_connections_create_conf(ngx_conf_t *cf);
static void finalize_request(ngx_http_request_t *r, ngx_int_t rc);

extern void ngx_http_upstream_connect(ngx_http_request_t *r, ngx_http_upstream_t *u);

#define RAMP(x) (x > 0 ? x : 0)

static ngx_command_t  max_connections_commands[] =
{ { ngx_string("max_connections")
  , NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1
  , max_connections_command
  , 0
  , 0
  , NULL
  }
, { ngx_string("max_connections_queue_timeout")
  , NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1
  , max_connections_queue_timeout_command
  , 0
  , 0
  , NULL
  }
, { ngx_string("max_connections_max_queue_length")
  , NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1
  , max_connections_max_queue_length_command
  , 0
  , 0
  , NULL
  }
, ngx_null_command
};

static ngx_http_module_t max_connections_module_ctx =
/* preconfiguration              */ { NULL 
/* postconfiguration             */ , NULL 
/* create main configuration     */ , NULL 
/* init main configuration       */ , NULL 
/* create server configuration   */ , max_connections_create_conf 
/* merge server configuration    */ , NULL
/* create location configuration */ , NULL 
/* merge location configuration  */ , NULL 
                                    };

ngx_module_t max_connections_module =
                        { NGX_MODULE_V1
/* module context    */ , &max_connections_module_ctx
/* module directives */ , max_connections_commands
/* module type       */ , NGX_HTTP_MODULE
/* init master       */ , NULL
/* init module       */ , NULL
/* init process      */ , NULL
/* init thread       */ , NULL
/* exit thread       */ , NULL
/* exit process      */ , NULL
/* exit master       */ , NULL
                        , NGX_MODULE_V1_PADDING
                        };

static ngx_uint_t
queue_size (max_connections_srv_conf_t *maxconn_cf)
{
  ngx_queue_t *node;
  ngx_uint_t queue_size = 0;
  /* TODO O(n) could be O(1) */
  for( node = maxconn_cf->waiting_requests.next
     ; node && node != &maxconn_cf->waiting_requests 
     ; node = node->next
     ) queue_size += 1;
  return queue_size;
}

static max_connections_peer_data_t *
queue_oldest (max_connections_srv_conf_t *maxconn_cf)
{
  if(ngx_queue_empty(&maxconn_cf->waiting_requests)) 
    return NULL;

  ngx_queue_t *last = ngx_queue_last(&maxconn_cf->waiting_requests);

  max_connections_peer_data_t *peer_data = 
    ngx_queue_data(last, max_connections_peer_data_t, queue);
  return peer_data;
}

static ngx_int_t
queue_remove (max_connections_peer_data_t *peer_data)
{
  max_connections_srv_conf_t *maxconn_cf = peer_data->maxconn_cf;

  /* return 0 if it wasn't in the queue */
  if(peer_data->queue.next == NULL)
    return 0;

  max_connections_peer_data_t *oldest = queue_oldest (maxconn_cf);

  ngx_queue_remove(&peer_data->queue);
  peer_data->queue.prev = peer_data->queue.next = NULL; 

  maxconn_cf->queue_length -= 1;
  assert(maxconn_cf->queue_length == queue_size(peer_data->maxconn_cf));

  ngx_log_error( NGX_LOG_INFO
                , peer_data->r->connection->log
                , 0
                , "max_connections del queue (new size %ui)"
                , maxconn_cf->queue_length
                );
  if(ngx_queue_empty(&maxconn_cf->waiting_requests)) {
    /* delete the timer if the queue is empty now */
    if(maxconn_cf->queue_check_event.timer_set) {
      ngx_del_timer( (&maxconn_cf->queue_check_event) );
    }
  } else if(oldest == peer_data) {  
    /* if the removed peer_data was the first */
    /* make sure that the check queue timer is set when we have things in
     * the queue */
    oldest = queue_oldest (maxconn_cf);

    /*  ------|-----------|-------------|------------------------ */
    /*       accessed    now           accessed + TIMEOUT         */
    ngx_add_timer( (&maxconn_cf->queue_check_event)
                 , RAMP(oldest->accessed + maxconn_cf->queue_timeout - ngx_current_msec)
                 ); 
  }

  return 1;
}

/* removes the first item from the queue - returns request */
static max_connections_peer_data_t *
queue_shift (max_connections_srv_conf_t *maxconn_cf)
{
  if(ngx_queue_empty(&maxconn_cf->waiting_requests)) 
    return NULL;
  
  max_connections_peer_data_t *peer_data = queue_oldest (maxconn_cf);

  queue_remove (peer_data);

  return peer_data;
}

/* adds a request to the end of the queue */
static ngx_int_t
queue_push (max_connections_srv_conf_t *maxconn_cf, max_connections_peer_data_t *peer_data)
{
  if(maxconn_cf->queue_length >= maxconn_cf->max_queue_length)
    return NGX_ERROR;
  
  /* if this is the first element ensure we set the queue_check_event */
  if(ngx_queue_empty(&maxconn_cf->waiting_requests)) {
    ngx_add_timer((&maxconn_cf->queue_check_event), maxconn_cf->queue_timeout); 
  }
  ngx_queue_insert_head(&maxconn_cf->waiting_requests, &peer_data->queue);

  maxconn_cf->queue_length += 1;

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, peer_data->r->connection->log, 0,
               "max_connections add queue (new size %ui)", maxconn_cf->queue_length);
  
  return NGX_OK;
}

/* This function takes the oldest request on the queue
 * (maxconn_cf->waiting_requests) and dispatches it to the backends.  This
 * calls ngx_http_upstream_connect() which will in turn call the peer get
 * callback, peer_get(). peer_get() will do
 * the actual selection of backend. Here we're just giving the request the
 * go-ahead to proceed.
 */
static void
dispatch (max_connections_srv_conf_t *maxconn_cf)
{
  if (ngx_queue_empty(&maxconn_cf->waiting_requests)) return;
  if (maxconn_cf->connections >= maxconn_cf->max_connections) return;

  max_connections_peer_data_t *peer_data = queue_shift(maxconn_cf);
  ngx_http_request_t *r = peer_data->r;

  ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
               "max_connections dispatch (queue_length: %ui, conn: %ui)",
               maxconn_cf->queue_length, maxconn_cf->connections);
  
  peer_data->processing = 1;
  maxconn_cf->connections++; /* keep track of how many slots are occupied */

  ngx_http_upstream_connect(r, r->upstream);
}

/*
 * finalize_request() been called before peer_free(), should not call dispatch()
 */
static void
finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    max_connections_peer_data_t *peer_data = r->upstream->peer.data;
    max_connections_srv_conf_t *cf = peer_data->maxconn_cf;

    peer_data->original_finalize_request(r, rc);
    
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "finalize http max connections request: %d", cf->connections);
    
    if (peer_data->processing) {
        cf->connections --;
        peer_data->processing = 0;
    }else {
        queue_remove(peer_data);
    }
}

static void
queue_check_event(ngx_event_t *ev)
{
  max_connections_srv_conf_t *maxconn_cf = ev->data;
  max_connections_peer_data_t *oldest; 
    
  while ( (oldest = queue_oldest(maxconn_cf))
       && ngx_current_msec - oldest->accessed > maxconn_cf->queue_timeout
        ) 
  {
    max_connections_peer_data_t *peer_data = queue_shift(maxconn_cf);
    ngx_log_error(NGX_LOG_INFO, peer_data->r->connection->log, 0,
                 "max_connections expire");
    ngx_http_finalize_request(peer_data->r, NGX_HTTP_GATEWAY_TIME_OUT);
  }

  /* check the check timer */
  if( !ngx_queue_empty(&maxconn_cf->waiting_requests) 
   && !maxconn_cf->queue_check_event.timer_set
    ) 
  {
    ngx_add_timer((&maxconn_cf->queue_check_event), maxconn_cf->queue_timeout); 
  }
}


/* The peer free function which is part of all NGINX upstream modules */
static void
peer_free (ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
  max_connections_peer_data_t *peer_data = data;
  
  peer_data->original_free_peer(pc, peer_data->data, state);

  /* request finalized and upstream connection released, then dispatch more requests*/
  dispatch(peer_data->maxconn_cf);
}

static ngx_int_t
peer_get (ngx_peer_connection_t *pc, void *data)
{
  max_connections_peer_data_t *peer_data = data;

  return peer_data->original_get_peer(pc, peer_data->data);
}

static ngx_int_t
peer_init (ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *uscf)
{
  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);
  
  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "http max connections peer init");

  if (maxconn_cf->original_init_peer(r, uscf) != NGX_OK) {
    return NGX_ERROR;
  }

  max_connections_peer_data_t *peer_data = 
    ngx_palloc(r->pool, sizeof(max_connections_peer_data_t));
  if(peer_data == NULL) return NGX_ERROR;

  peer_data->maxconn_cf = maxconn_cf;
  peer_data->r = r;
  peer_data->accessed = ngx_current_msec; 
  peer_data->processing = 0;
  
  peer_data->original_finalize_request = r->upstream->finalize_request;
  peer_data->original_get_peer = r->upstream->peer.get;
  peer_data->original_free_peer = r->upstream->peer.free;
  peer_data->data = r->upstream->peer.data;

  r->upstream->finalize_request = finalize_request;
  r->upstream->peer.get   = peer_get;
  r->upstream->peer.free  = peer_free;
  r->upstream->peer.data  = peer_data;

  if (maxconn_cf->connections < maxconn_cf->max_connections) {
      peer_data->processing = 1;
      maxconn_cf->connections ++; /* keep track of how many slots are occupied */
      return NGX_OK;
  }

  if (queue_push(maxconn_cf, peer_data) == NGX_ERROR) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
               "max connections queue push failed (queue_length: %ui, conn: %ui)",
               maxconn_cf->queue_length, maxconn_cf->connections);
      return NGX_ERROR;
  }
  
  ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
               "max connections put in queue (queue_length: %ui, conn: %ui)",
               maxconn_cf->queue_length, maxconn_cf->connections);

  return NGX_BUSY;
}

static ngx_int_t
max_connections_init(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *uscf)
{
  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
               "max connections init");

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  if (maxconn_cf->original_init_upstream(cf, uscf) != NGX_OK) {
      return NGX_ERROR;
  }

  maxconn_cf->original_init_peer = uscf->peer.init;

  uscf->peer.init = peer_init;

  maxconn_cf->connections = 0;
  maxconn_cf->queue_length = 0;
  
  ngx_queue_init(&maxconn_cf->waiting_requests);

  maxconn_cf->queue_check_event.handler = queue_check_event;
  maxconn_cf->queue_check_event.log = cf->log;
  maxconn_cf->queue_check_event.data = maxconn_cf;
  
  return NGX_OK;
}

/* TODO This function is probably not neccesary. Nginx provides a means of
 * easily setting scalar time values with ngx_conf_set_msec_slot() in the
 * ngx_command_t structure. I couldn't manage to make it work, not knowing
 * what I should be using for the two offset parameters. 
 */ 
static char *
max_connections_queue_timeout_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_upstream_srv_conf_t *uscf = 
    ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  ngx_str_t        *value; 

  value = cf->args->elts;    

  ngx_msec_t ms = ngx_parse_time(&value[1], 0); 
  if (ms == (ngx_msec_t) NGX_ERROR) {
      return "invalid value";
  }

  maxconn_cf->queue_timeout = ms;

  return NGX_CONF_OK;
}

/* TODO same as above */
static char *
max_connections_max_queue_length_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_upstream_srv_conf_t *uscf = 
    ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  ngx_str_t *value = cf->args->elts;    
  ngx_int_t n = ngx_atoi(value[1].data, value[1].len);
  if (n == NGX_ERROR) {
    return "invalid number";        
  }

  maxconn_cf->max_queue_length = n;

  return NGX_CONF_OK;
}

static char *
max_connections_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_upstream_srv_conf_t *uscf = 
    ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);
  
  /* 1. set the initialization function */
  maxconn_cf->original_init_upstream = uscf->peer.init_upstream
                                  ? uscf->peer.init_upstream
                                  : ngx_http_upstream_init_round_robin;
  uscf->peer.init_upstream = max_connections_init;

  /* 2. set the number of max_connections */
  ngx_str_t *value = cf->args->elts;
  ngx_int_t max_connections = ngx_atoi(value[1].data, value[1].len);

  if (max_connections == NGX_ERROR || max_connections == 0) {
    ngx_conf_log_error( NGX_LOG_EMERG
                      , cf
                      , 0
                      , "invalid value \"%V\" in \"%V\" directive"
                      , &value[1]
                      , &cmd->name
                      );
    return NGX_CONF_ERROR;
  }

  maxconn_cf->max_connections = (ngx_uint_t)max_connections;

  return NGX_CONF_OK;
}

static void *
max_connections_create_conf(ngx_conf_t *cf)
{
    max_connections_srv_conf_t  *conf = 
      ngx_pcalloc(cf->pool, sizeof(max_connections_srv_conf_t));

    if (conf == NULL) return NGX_CONF_ERROR;
    conf->max_connections = 1;
    conf->max_queue_length = 10000; /* default max queue length 10000 */
    conf->queue_timeout = 10000;  /* default queue timeout 10 seconds */
    return conf;
}

