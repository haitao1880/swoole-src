/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Fang  <coooold@live.com>                        |
  +----------------------------------------------------------------------+
*/

#include "php_swoole.h"
#include "thirdparty/php_http_parser.h"
#include "ext/standard/basic_functions.h"
#include "ext/standard/php_http.h"

#ifdef SW_ASYNC_HTTPCLIENT

typedef struct
{
    zval* gc_list[128];
    uint gc_idx;
} http_client_callback;

typedef struct
{
    swClient *cli;
    char *host;
    zend_size_t host_len;
    long port;
    double timeout;
    char* uri;
    zend_size_t uri_len;
    
    char *tmp_header_field_name;
    zend_size_t tmp_header_field_name_len;
    
    char *body;

    php_http_parser parser;
    
    int phase;  //0 wait 1 ready 2 busy
    int keep_alive;  //0 no 1 keep

} http_client;

static int http_client_parser_on_header_field(php_http_parser *parser, const char *at, size_t length);
static int http_client_parser_on_header_value(php_http_parser *parser, const char *at, size_t length);
static int http_client_parser_on_body(php_http_parser *parser, const char *at, size_t length);
static int http_client_parser_on_message_complete(php_http_parser *parser);

static sw_inline void http_client_swString_append_headers(swString* swStr, char* key, zend_size_t key_len, char* data, zend_size_t data_len)
{
    swString_append_ptr(swStr, key, key_len);
    swString_append_ptr(swStr, ZEND_STRL(": "));
    swString_append_ptr(swStr, data, data_len);
    swString_append_ptr(swStr, ZEND_STRL("\r\n"));
}

static const php_http_parser_settings http_parser_settings =
{
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    http_client_parser_on_header_field,
    http_client_parser_on_header_value,
    NULL,
    http_client_parser_on_body,
    http_client_parser_on_message_complete
};


static PHP_METHOD(swoole_http_client, __construct);
static PHP_METHOD(swoole_http_client, __destruct);
static PHP_METHOD(swoole_http_client, set);
static PHP_METHOD(swoole_http_client, setHeaders);
static PHP_METHOD(swoole_http_client, setData);
static PHP_METHOD(swoole_http_client, execute);
static PHP_METHOD(swoole_http_client, isConnected);
static PHP_METHOD(swoole_http_client, close);
static PHP_METHOD(swoole_http_client, on);

static void http_client_free(zval *object, http_client *http);
static int http_client_error_callback(zval *zobject, swEvent *event, int error TSRMLS_DC);
static int http_client_send_http_request(zval *zobject TSRMLS_DC);
static http_client* http_client_create(zval *object TSRMLS_DC);

static zval* http_client_get_cb(zval *zobject, char *cb_name, int cb_name_len TSRMLS_DC);
static void http_client_set_cb(zval *zobject, char *cb_name, int cb_name_len, zval *zcb TSRMLS_DC);


static zval* http_client_get_cb(zval *zobject, char *cb_name, int cb_name_len TSRMLS_DC)
{
    return sw_zend_read_property(
        swoole_http_client_class_entry_ptr,
        zobject, cb_name, cb_name_len, 1 TSRMLS_CC);
}

static void http_client_set_cb(zval *zobject, char *cb_name, int cb_name_len, zval *zcb TSRMLS_DC)
{
    if (zcb == NULL)
    {
        zend_update_property_null(swoole_http_client_class_entry_ptr, zobject, cb_name, cb_name_len TSRMLS_CC);
        return;
    }
    
    sw_zval_add_ref(&zcb);
    zend_update_property(swoole_http_client_class_entry_ptr, zobject, cb_name, cb_name_len, zcb TSRMLS_CC);
    
    http_client_callback *hcc = swoole_get_property(zobject, 0);
    if(hcc->gc_idx >= 128)
    {
        swoole_php_fatal_error(E_ERROR, "Too many callbacks");
    }

    hcc->gc_list[hcc->gc_idx++] = zcb;
}

static const zend_function_entry swoole_http_client_methods[] =
{
    PHP_ME(swoole_http_client, __construct, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(swoole_http_client, __destruct, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_DTOR)
    PHP_ME(swoole_http_client, set, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client, setHeaders, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client, setData, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client, execute, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client, isConnected, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client, close, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client, on, NULL, ZEND_ACC_PUBLIC)
    PHP_FE_END
};


zend_class_entry swoole_http_client_ce;
zend_class_entry *swoole_http_client_class_entry_ptr;

void swoole_http_client_init(int module_number TSRMLS_DC)
{
    INIT_CLASS_ENTRY(swoole_http_client_ce, "swoole_http_client", swoole_http_client_methods);
    swoole_http_client_class_entry_ptr = zend_register_internal_class(&swoole_http_client_ce TSRMLS_CC);

    zend_declare_property_long(swoole_http_client_class_entry_ptr, SW_STRL("errCode")-1, 0, ZEND_ACC_PUBLIC TSRMLS_CC);
    zend_declare_property_long(swoole_http_client_class_entry_ptr, SW_STRL("sock")-1, 0, ZEND_ACC_PUBLIC TSRMLS_CC);
}

/**
 * @zobject: swoole_http_client object
 */
static void http_client_onClose(swClient *cli)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    zval *zcallback = NULL;
    zval *retval = NULL;
    zval **args[1];
    zval *zobject = cli->object;

    http_client *http = swoole_get_object(zobject);
    if (!http || !http->cli)
    {
        swoole_php_fatal_error(E_WARNING, "object is not instanceof swoole_http_client.");
        return;
    }

    if (http->cli->socket->closed)
    {
        return;
    }
 
    zcallback = http_client_get_cb(zobject, ZEND_STRL("close") TSRMLS_CC);
    if (zcallback == NULL || ZVAL_IS_NULL(zcallback))
    {
        return;
    }
    args[0] = &zobject;
    if (sw_call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL TSRMLS_CC)  == FAILURE)
    {
        swoole_php_fatal_error(E_ERROR, "swoole_client->close[4]: onClose handler error");
    }
    if (EG(exception))
    {
        zend_exception_error(EG(exception), E_ERROR TSRMLS_CC);
    }
    //free the callback return value
    if (retval != NULL)
    {
        sw_zval_ptr_dtor(&retval);
    }
    if (EG(exception))
    {
        zend_exception_error(EG(exception), E_ERROR TSRMLS_CC);
    }
    sw_zval_ptr_dtor(&zobject);
}

/**
 * @zobject: swoole_http_client object
 */
static void http_client_onError(swClient *cli)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    zval *zcallback = NULL;
    zval *retval = NULL;
    zval **args[1];
    zval *zobject = cli->object;

    http_client *http = swoole_get_object(zobject);
    if (!http || !http->cli)
    {
        swoole_php_fatal_error(E_WARNING, "object is not instanceof swoole_http_client.");
        return;
    }

    if (http->cli->socket->closed)
    {
        return;
    }

    zcallback = http_client_get_cb(zobject, ZEND_STRL("error") TSRMLS_CC);
    if (zcallback == NULL || ZVAL_IS_NULL(zcallback))
    {
        swoole_php_fatal_error(E_ERROR, "swoole_client->close[3]: no close callback.");
    }
    args[0] = &zobject;
    if (sw_call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL TSRMLS_CC)  == FAILURE)
    {
        swoole_php_fatal_error(E_ERROR, "swoole_client->close[4]: onClose handler error");
    }
    if (EG(exception))
    {
        zend_exception_error(EG(exception), E_ERROR TSRMLS_CC);
    }
    //free the callback return value
    if (retval != NULL)
    {
        sw_zval_ptr_dtor(&retval);
    }
    if (EG(exception))
    {
        zend_exception_error(EG(exception), E_ERROR TSRMLS_CC);
    }
    sw_zval_ptr_dtor(&zobject);
}

static void http_client_free(zval *object, http_client *http)
{
    //printf("http_client_free()\n");
    if (!http)
    {
        return;
    }
    swoole_set_object(object, NULL);

    if (http->cli)
    {

#if PHP_MAJOR_VERSION >= 7
        //for php7 object was allocated sizeof(zval) when execute
        if (http->cli->socket->object)
        {
            //printf("free http->cli->socket->object\n");
            efree(http->cli->socket->object);
        }
#endif
        http->cli->socket->object = NULL;

        //close connect when __destruct
        if (http->cli->socket->fd != 0)
        {
            //printf("http->cli->close()\n");
            http->cli->close(http->cli);
        }

        //printf("free http->cli\n");
        efree(http->cli);
        efree(http->uri);
    }
    //printf("free http\n");
    efree(http);
}

static void http_client_onReceive(swClient *cli, char *data, uint32_t length)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    zval *zobject = cli->object;
    http_client *http = swoole_get_object(zobject);
    if (!http->cli)
    {
        swoole_php_fatal_error(E_WARNING, "object is not instanceof swoole_http_client.");
        return;
    }

    long parsed_n = php_http_parser_execute(&http->parser, &http_parser_settings, data, length);
    if (parsed_n < 0)
    {
        swSysError("Parsing http over socket[%d] failed.", cli->socket->fd);
        cli->close(cli);
    }
}

static void http_client_onConnect(swClient *cli)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    zval *zobject = cli->object;
    http_client *http = swoole_get_object(zobject);
    if (!http->cli)
    {
        swoole_php_fatal_error(E_WARNING, "object is not instanceof swoole_http_client.");
        return;
    }
    //send http request on write
    http_client_send_http_request(zobject TSRMLS_CC);
}

static int http_client_send_http_request(zval *zobject TSRMLS_DC)
{
    http_client *http = swoole_get_object(zobject);
    if (!http->cli)
    {
        swoole_php_fatal_error(E_WARNING, "object is not instanceof swoole_http_client.");
        return SW_ERR;
    }
    
    if (!http->cli->socket && http->cli->socket->active == 0)
    {
        swoole_php_error(E_WARNING, "server is not connected.");
        return SW_ERR;
    }
    
    if (http->phase != 1)
    {
        swoole_php_error(E_WARNING, "http client is not ready.");
        return SW_ERR;
    }
    
    http->phase = 2;
     //clear errno
    SwooleG.error = 0;

    swString* req_buff = swString_new(512);
    swString* header_buff = swString_new(512);
    swString* post_buff = swString_new(512);

    http_client_swString_append_headers(header_buff, ZEND_STRL("Host"), http->host, http->host_len);

    zval* zdata = sw_zend_read_property(swoole_http_client_class_entry_ptr, zobject, ZEND_STRL("set_data"),
            1 TSRMLS_CC);
    if(zdata && Z_TYPE_P(zdata) != IS_NULL)
    {
        char post_len_str[64];
        swString_append_ptr(req_buff, ZEND_STRL("POST "));

        convert_to_string(zdata);
        swString_append_ptr(post_buff, Z_STRVAL_P(zdata), Z_STRLEN_P(zdata));
        sprintf(post_len_str, "%d", Z_STRLEN_P(zdata));

        http_client_swString_append_headers(header_buff, ZEND_STRL("Content-Type"), ZEND_STRL("application/x-www-form-urlencoded"));
        http_client_swString_append_headers(header_buff, ZEND_STRL("Content-Length"), post_len_str, strlen(post_len_str));
    }
    else
    {
        swString_append_ptr(req_buff, ZEND_STRL("GET "));
    }
    zend_update_property_null(swoole_http_client_class_entry_ptr, zobject, ZEND_STRL("set_data") TSRMLS_CC);

    swString_append_ptr(req_buff, http->uri, http->uri_len);
    swString_append_ptr(req_buff, ZEND_STRL(" HTTP/1.1\r\n"));

    zval* zheaders = sw_zend_read_property(
            swoole_http_client_class_entry_ptr,
            zobject, ZEND_STRL("set_headers"), 1 TSRMLS_CC);
    char *key;
    uint32_t keylen;
    int keytype;
    zval *value;
    if (zheaders && Z_TYPE_P(zheaders) == IS_ARRAY)
    {
        SW_HASHTABLE_FOREACH_START2(Z_ARRVAL_P(zheaders), key, keylen, keytype, value)
            if (HASH_KEY_IS_STRING != keytype)
            {
                continue;
            }
            convert_to_string(value);
            http_client_swString_append_headers(header_buff, key, keylen, Z_STRVAL_P(value), Z_STRLEN_P(value));
        SW_HASHTABLE_FOREACH_END();
    }
    zend_update_property_null(swoole_http_client_class_entry_ptr, zobject, ZEND_STRL("set_headers") TSRMLS_CC);

    if (http->keep_alive)
    {
        http_client_swString_append_headers(header_buff, ZEND_STRL("Connection"), ZEND_STRL("keep-alive"));
    }
    else
    {
        http_client_swString_append_headers(header_buff, ZEND_STRL("Connection"), ZEND_STRL("closed"));
    }

    swString_append(req_buff, header_buff);
    swString_append_ptr(req_buff, ZEND_STRL("\r\n"));
    swString_append(req_buff, post_buff);

    int ret;
    int flags = MSG_DONTWAIT;  //http://www.cnblogs.com/blankqdb/archive/2012/08/30/2663859.html
    

    ret = http->cli->send(http->cli, req_buff->str, req_buff->length, flags);
    if (ret < 0)
    {
        SwooleG.error = errno;
        swoole_php_sys_error(E_WARNING, "send(%d) %d bytes failed.", http->cli->socket->fd, (int)req_buff->length);
        zend_update_property_long(swoole_http_client_class_entry_ptr, zobject, SW_STRL("errCode")-1, SwooleG.error TSRMLS_CC);
    }
    
    //printf("\n%s\n", req_buff->str);

    swString_free(req_buff);
    swString_free(header_buff);
    swString_free(post_buff);
    
    return ret;
}

static int http_client_error_callback(zval *zobject, swEvent *event, int error TSRMLS_DC)
{
    zval *zcallback;
    zval *retval = NULL;
    zval **args[1];

    if (error != 0)
    {
        http_client *http = swoole_get_object(zobject);
        if (http)
        {
            swoole_php_fatal_error(E_WARNING, "connect to server [%s:%ld] failed. Error: %s [%d].", http->host, http->port, strerror(error), error);
        }
    }

    SwooleG.main_reactor->del(SwooleG.main_reactor, event->fd);

    zcallback = http_client_get_cb(zobject, ZEND_STRL("error") TSRMLS_CC);
    zend_update_property_long(swoole_http_client_class_entry_ptr, zobject, ZEND_STRL("errCode"), error TSRMLS_CC);

    args[0] = &zobject;
    if (zcallback == NULL || ZVAL_IS_NULL(zcallback))
    {
        swoole_php_fatal_error(E_WARNING, "object have not error callback.");
        return SW_ERR;
    }
    if (sw_call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
    {
        swoole_php_fatal_error(E_WARNING, "onError handler error");
        return SW_ERR;
    }
    if (EG(exception))
    {
        zend_exception_error(EG(exception), E_ERROR TSRMLS_CC);
    }
    if (retval)
    {
        sw_zval_ptr_dtor(&retval);
    }
    
    //printf("sw_zval_ptr_dtor(&zobject) on error;\n");
    sw_zval_ptr_dtor(&zobject);
    return SW_OK;
}

static http_client* http_client_create(zval *object TSRMLS_DC)
{
    zval *ztmp;
    http_client *http;
    HashTable *vht;

    http = (http_client*) emalloc(sizeof(http_client));
    bzero(http, sizeof(http_client));

    swoole_set_object(object, http);

    php_http_parser_init(&http->parser, PHP_HTTP_RESPONSE);
    http->parser.data = http;

    ztmp = sw_zend_read_property(swoole_http_client_class_entry_ptr, object, ZEND_STRL("host"), 0 TSRMLS_CC);
    http->host = Z_STRVAL_P(ztmp);
    http->host_len = Z_STRLEN_P(ztmp);
    ztmp = sw_zend_read_property(swoole_http_client_class_entry_ptr, object, ZEND_STRL("port"), 0 TSRMLS_CC);
    convert_to_long(ztmp);
    http->port = Z_LVAL_P(ztmp);

    http->timeout = SW_CLIENT_DEFAULT_TIMEOUT;
    http->keep_alive = 0;

    zval *zset = sw_zend_read_property(swoole_http_client_class_entry_ptr, object, ZEND_STRL("setting"), 1 TSRMLS_CC);
    if (zset && !ZVAL_IS_NULL(zset))
    {
        vht = Z_ARRVAL_P(zset);
        /**
         * timeout
         */
        if (sw_zend_hash_find(vht, ZEND_STRS("timeout"), (void **) &ztmp) == SUCCESS)
        {
            http->timeout = (double) Z_DVAL_P(ztmp);
        }
        /**
         * keep_alive
         */
        if (sw_zend_hash_find(vht, ZEND_STRS("keep_alive"), (void **) &ztmp) == SUCCESS)
        {
            http->keep_alive = (int) Z_LVAL_P(ztmp);
        }
    }

    http->phase = 1;

    return http;
}

static PHP_METHOD(swoole_http_client, __construct)
{
    char *host;
    zend_size_t host_len;
    long port = 80;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|l", &host, &host_len, &port) == FAILURE)
    {
        return;
    }
    
    if (host_len <= 0)
    {
        swoole_php_fatal_error(E_ERROR, "host is empty.");
        RETURN_FALSE;
    }

    zend_update_property_stringl(swoole_http_client_class_entry_ptr, getThis(), ZEND_STRL("host"), host, host_len TSRMLS_CC);
    
    zend_update_property_long(swoole_http_client_class_entry_ptr,
    getThis(), ZEND_STRL("port"), port TSRMLS_CC);

    php_swoole_check_reactor();

    //init
    swoole_set_object(getThis(), NULL);

    zval *headers;
    SW_MAKE_STD_ZVAL(headers);
    array_init(headers);
    zend_update_property(swoole_http_client_class_entry_ptr,
    getThis(), ZEND_STRL("headers"), headers TSRMLS_CC);

    zval *body;
    SW_MAKE_STD_ZVAL(body);
    SW_ZVAL_STRING(body, "", 1);
    zend_update_property(swoole_http_client_class_entry_ptr,
    getThis(), ZEND_STRL("body"), body TSRMLS_CC);

    http_client_callback *hcc;
    hcc = (http_client_callback*) emalloc(sizeof(http_client_callback));
    bzero(hcc, sizeof(http_client_callback));
    swoole_set_property(getThis(), 0, hcc);

    zval *ztype;
    SW_MAKE_STD_ZVAL(ztype);
    Z_LVAL_P(ztype) = SW_SOCK_TCP | SW_FLAG_ASYNC;
    zend_update_property(swoole_client_class_entry_ptr, getThis(), ZEND_STRL("type"), ztype TSRMLS_CC);
    
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_client, __destruct)
{
    zval *headers = sw_zend_read_property(swoole_http_client_class_entry_ptr, getThis(), ZEND_STRL("headers"), 0 TSRMLS_CC);
    zval *body = sw_zend_read_property(swoole_http_client_class_entry_ptr, getThis(), ZEND_STRL("body"), 0 TSRMLS_CC);
    
    sw_zval_ptr_dtor(&headers);
    sw_zval_ptr_dtor(&body);
    
    http_client_set_cb(getThis(), ZEND_STRL("finish"), NULL TSRMLS_CC);
    http_client_set_cb(getThis(), ZEND_STRL("close"), NULL TSRMLS_CC);
    http_client_set_cb(getThis(), ZEND_STRL("error"), NULL TSRMLS_CC);
    
    http_client_callback *hcc = swoole_get_property(getThis(), 0);
    int i;
    for (i = 0; i < hcc->gc_idx; i++)
    {
        zval *zcb = hcc->gc_list[i];
        sw_zval_ptr_dtor(&zcb);
    }
    efree(hcc);
    swoole_set_property(getThis(), 0, NULL);
    
    //printf("zim_swoole_http_client___destruct()\n");
    http_client *http = swoole_get_object(getThis());
    if (http)
    {
        http_client_free(getThis(), http);
    }
}

static PHP_METHOD(swoole_http_client, set)
{
    zval *zset;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &zset) == FAILURE)
    {
        return;
    }
    zend_update_property(swoole_http_client_class_entry_ptr, getThis(), ZEND_STRL("setting"), zset TSRMLS_CC);
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_client, setHeaders)
{
    zval *zset;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &zset) == FAILURE)
    {
        return;
    }
    zend_update_property(swoole_http_client_class_entry_ptr, getThis(), ZEND_STRL("set_headers"), zset TSRMLS_CC);
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_client, setData)
{
    zval *zset;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &zset) == FAILURE)
    {
        return;
    }
    zend_update_property(swoole_http_client_class_entry_ptr, getThis(), ZEND_STRL("set_data"), zset TSRMLS_CC);
    RETURN_TRUE;
}

//$http_client->execute();
static PHP_METHOD(swoole_http_client, execute)
{
    int ret;
    http_client *http = NULL;
    char *uri = NULL;
    zend_size_t uri_len = 0;
    zval *finish_cb;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sz", &uri, &uri_len, &finish_cb) == FAILURE)
    {
        return;
    }

    http = swoole_get_object(getThis());
    if (http)   //http is not null when keeping alive
    {
        if (http->phase != 1 || http->cli->socket->active != 1 || http->keep_alive != 1) //http not ready
        {
            swoole_php_fatal_error(E_ERROR, "Operation now in progress phase %d.", http->phase);
            
            swEvent e;
            e.fd = http->cli->socket->fd;
            e.socket = http->cli->socket;
            http_client_error_callback(getThis(), &e, errno TSRMLS_CC);

            RETURN_FALSE;
        }
    }
    else
    {
        http = http_client_create(getThis() TSRMLS_CC);
    }

    if (http == NULL)
    {
        RETURN_FALSE;
    }
    
    if (uri_len <= 0)
    {
        RETURN_FALSE;
    }

    http->uri = estrdup(uri);
    http->uri_len = uri_len;
    
    if (finish_cb == NULL || ZVAL_IS_NULL(finish_cb))
    {
        swoole_php_fatal_error(E_WARNING, "finish callback is not set.");
    }
    http_client_set_cb(getThis(), ZEND_STRL("finish"), finish_cb TSRMLS_CC);
    
    if (http->cli)   //if connection exists
    {
        http_client_send_http_request(getThis() TSRMLS_CC);
        RETURN_TRUE;
    }
    
    swClient *cli = php_swoole_client_create_socket(getThis(), http->host, http->host_len, http->port);
    if (cli == NULL)
    {
        RETURN_FALSE;
    }
    http->cli = cli;

    if (cli->socket->active == 1)
    {
        swoole_php_fatal_error(E_WARNING, "swoole_http_client is already connected.");
        RETURN_FALSE;
    }

    cli->object = getThis();
    cli->reactor_fdtype = PHP_SWOOLE_FD_CLIENT;
    cli->onReceive = http_client_onReceive;
    cli->onConnect = http_client_onConnect;

    cli->onClose = http_client_onClose;
    cli->onError = http_client_onError;

    ret = cli->connect(cli, http->host, http->port, http->timeout, 0);
    SW_CHECK_RETURN(ret);
}

static PHP_METHOD(swoole_http_client, isConnected)
{
    http_client *http = swoole_get_object(getThis());
    if (!http->cli)
    {
        RETURN_FALSE;
    }
    if (!http->cli->socket)
    {
        RETURN_FALSE;
    }
    RETURN_BOOL(http->cli->socket->active);
}

static PHP_METHOD(swoole_http_client, close)
{
    int ret = 1;

    http_client *http = swoole_get_object(getThis());
    if (!http->cli)
    {
        swoole_php_fatal_error(E_WARNING, "object is not instanceof swoole_http_client.");
        RETURN_FALSE;
    }

    if (!http->cli->socket)
    {
        swoole_php_error(E_WARNING, "not connected to the server");
        RETURN_FALSE;
    }

    if (http->cli->socket->closed)
    {
        swoole_php_error(E_WARNING, "client socket is closed.");
        RETURN_FALSE;
    }

    if (http->cli->async == 1 && SwooleG.main_reactor != NULL)
    {
        ret = http->cli->close(http->cli);
    }
    SW_CHECK_RETURN(ret);
}

static PHP_METHOD(swoole_http_client, on)
{
    char *cb_name;
    zend_size_t cb_name_len;
    zval *zcallback;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sz", &cb_name, &cb_name_len, &zcallback) == FAILURE)
    {
        return;
    }

    if (strncasecmp("finish", cb_name, cb_name_len) == 0 || strncasecmp("error", cb_name, cb_name_len) == 0
            || strncasecmp("close", cb_name, cb_name_len) == 0)
    {
        http_client_set_cb(getThis(), cb_name, cb_name_len, zcallback TSRMLS_CC);
    }
    else
    {
        swoole_php_fatal_error(E_WARNING, "swoole_http_client: event callback[%s] is unknow", cb_name);
        RETURN_FALSE;
    }
    
    zend_update_property(swoole_http_client_class_entry_ptr, getThis(), cb_name, cb_name_len, zcallback TSRMLS_CC);

    RETURN_TRUE;
}

static int http_client_parser_on_header_field(php_http_parser *parser, const char *at, size_t length)
{
// #if PHP_MAJOR_VERSION < 7
//     TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
// #endif
    http_client* http = (http_client*)parser->data;
    //zval* zobject = (zval*)http->cli->socket->object;

    http->tmp_header_field_name = (char *)at;
    http->tmp_header_field_name_len = length;
    return 0;
}


static int http_client_parser_on_header_value(php_http_parser *parser, const char *at, size_t length)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    http_client* http = (http_client*) parser->data;
    zval* zobject = (zval*) http->cli->object;
    
    zval *headers = sw_zend_read_property(swoole_http_client_class_entry_ptr, zobject, ZEND_STRL("headers"), 0 TSRMLS_CC);

    char *header_name = zend_str_tolower_dup(http->tmp_header_field_name, http->tmp_header_field_name_len);
    sw_add_assoc_stringl_ex(headers, header_name, http->tmp_header_field_name_len + 1, (char *) at, length, 1);
    efree(header_name);
    return 0;
}

static int http_client_parser_on_body(php_http_parser *parser, const char *at, size_t length)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    http_client* http = (http_client*) parser->data;
    zval* zobject = (zval*) http->cli->object;
    
    zval *body = sw_zend_read_property(swoole_http_client_class_entry_ptr, zobject, ZEND_STRL("body"), 0 TSRMLS_CC);
    zval *tmp;
    SW_MAKE_STD_ZVAL(tmp);
    SW_ZVAL_STRINGL(tmp, at, length, 1);
#if PHP_MAJOR_VERSION < 7
    add_string_to_string(body, body, tmp);
#else
    concat_function(body, body, tmp);
#endif
    sw_zval_ptr_dtor(&tmp);

    return 0;
}

static int http_client_parser_on_message_complete(php_http_parser *parser)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    http_client* http = (http_client*) parser->data;
    zval* zobject = (zval*) http->cli->object;

    if(http->keep_alive == 1)
    {
        //reset http phase for reuse
        http->phase = 1;
    }

    zval *retval;
    zval *zcallback;

    zcallback = http_client_get_cb(zobject, ZEND_STRL("finish") TSRMLS_CC);

    zval **args[1];
    args[0] = &zobject;
    
    if (zcallback == NULL || ZVAL_IS_NULL(zcallback))
    {
        swoole_php_fatal_error(E_WARNING, "swoole_http_client object have not receive callback.");
    }
    if (sw_call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
    {
        swoole_php_fatal_error(E_WARNING, "onReactorCallback handler error");
    }
    if (EG(exception))
    {
        zend_exception_error(EG(exception), E_ERROR TSRMLS_CC);
    }
    if (retval != NULL)
    {
        sw_zval_ptr_dtor(&retval);
    }

    if (http->keep_alive == 0)
    {
        http->cli->close(http->cli);
    }

    return 0;
}

#endif
