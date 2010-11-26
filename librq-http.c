//-----------------------------------------------------------------------------
// librq-http
// 
// Library to interact with the rq-http service.   It is used by the consumers
// that rq-http sends requests to.   The rq-http daemon also uses it to handle
// the results from the consumers.
//-----------------------------------------------------------------------------


#include "rq-http.h"
#include <assert.h>
#include <parsehttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if (RQ_HTTP_VERSION != 0x00000600)
#error "Compiling against incorrect version of rq-http.h"
#endif

#if (LIBPARSEHTTP_VERSION < 0x00000500)
#error "Requires libparsehttp at least 0.05 or higher"
#endif


typedef struct {
	char *key;
	char *value;
} param_t;


//-----------------------------------------------------------------------------
static rq_http_req_t * req_new(rq_http_t *http, void *arg)
{
	rq_http_req_t *req;

	assert(http);

	req = calloc(1, sizeof(*req));
	assert(req);

	// we dont actually process the params until a parameter is requested, so we
	// dont need to initialise the object yet, and is a good indicator to
	// determine if the params have been parsed or not.
	req->param_list = NULL;

	// PERF: get this buffer from the bufpool.
	req->reply = expbuf_init(NULL, 0);

	req->http = http;
	req->arg = arg;

	return(req);
}

//-----------------------------------------------------------------------------
static void req_free(rq_http_req_t *req)
{
	param_t *param;
	
	assert(req);

	assert(req->reply);
	assert(BUF_LENGTH(req->reply) == 0);
	req->reply = expbuf_free(req->reply);
	assert(req->reply == NULL);

	// if the message is not NULL, then it means that we didn't send off the reply.  
	assert(req->msg == NULL);

	if (req->param_list) {
		assert(req->params);
		while((param = ll_pop_head(req->param_list))) {
			assert(param->key);
			free(param->key);
			assert(param->value);
			free(param->value);
			free(param);
		}
		req->param_list = ll_free(req->param_list);
		assert(req->param_list == NULL);
	}

	if (req->host)   { free(req->host); }
	if (req->path)   { free(req->path); }
	if (req->params) { free(req->params); }

	free(req);
}




//-----------------------------------------------------------------------------
// This callback function is to be fired when the CMD_CLEAR command is 
// received. In this case, it is not necessary, and should just check that
// everything is already in a cleared state.  It wouldn't make sense to receive
// a CLEAR in the middle of the payload, so we will treat it as a programmer
// error.
static void cmdClear(rq_http_req_t *req) 
{
 	assert(req);

	assert(req->http);
	assert(req->method == 0);
	assert(req->code == 0);
	assert(req->host == NULL);
	assert(req->path == NULL);
	assert(req->params == NULL);
	assert(req->param_list == NULL);
}


//-----------------------------------------------------------------------------
// This command means that all the information has been provided, and it needs
// to be actioned.  We dont return anything here.  The callback function will
// need to return somethng.
static void cmdExecute(rq_http_req_t *req)
{
	rq_http_t *http;
	
 	assert(req);

 	assert(req->http);
 	http = req->http;

 	assert(req->path);
	assert(http->handler);
	
	req->arg = http->arg;
	http->handler(req);
}


static void cmdMethodGet(rq_http_req_t *req)
{
 	assert(req);

 	assert(req->method == 0);
 	req->method = 'G';
}

static void cmdMethodPost(rq_http_req_t *req)
{
 	assert(req);

 	assert(req->method == 0);
 	req->method = 'P';
}

static void cmdMethodHead(rq_http_req_t *req)
{
 	assert(req);

 	assert(req->method == 0);
 	req->method = 'H';
}

static void cmdHost(rq_http_req_t *req, risp_length_t length, void *data)
{
	assert(req);
	assert(length > 0);
	assert(data != NULL);

	assert(req->host == NULL);
	req->host = (char *) malloc(length+1);
	memcpy(req->host, data, length);
	req->host[length] = '\0';
}

static void cmdPath(rq_http_req_t *req, risp_length_t length, void *data)
{
	assert(req);
	assert(length > 0);
	assert(data != NULL);

	assert(req->path == NULL);
	req->path = (char *) malloc(length+1);
	memcpy(req->path, data, length);
	req->path[length] = '\0';
}


// pointer points to a string with hex ascii values;
static char hexchar(char *ptr) 
{
	char c = 0;
	char x,y;
	
	x = ptr[0];
	y = ptr[1];
	
	if (x >= '0' && x <= '9') { c = (x - '0') << 4; }
	else if (x >= 'a' && x <= 'f') { c = (10 + (x - 'a')) << 4; }
	else if (x >= 'A' && x <= 'F') { c = (10 + (x - 'A')) << 4; }
	
	if (y >= '0' && y <= '9') { c += (y - '0'); }
	else if (y >= 'a' && y <= 'f') { c += (y - 'a') + 10; }
	else if (y >= 'A' && y <= 'F') { c += (y - 'A') + 10; }
	
	return (c);
}


static void param_handler(const char *key, const char *value, void *arg) 
{
	list_t *list = arg;
	param_t *param;
	int i, j, l;
	
	assert(key);
	assert(value);
	assert(list);
	
	param = malloc(sizeof(*param));
	
	// FIX: need to parse the key to replace meta chars.
	l = strlen(key);
	param->key = malloc(l);
	for (i=0,j=0; i<l; i++,j++) {
		if (key[i] == '%' && i+2<l) {
			param->key[j] = hexchar(&key[i+1]);
			i += 2;
		}
		else if (key[i] == '+') {
			param->key[j] = ' ';
		}
		else {
			param->key[j] = key[i];
		}
	}
	param->key[j] = 0;
	
	// FIX: need to parse the value to replace the meta chars..
	l = strlen(value);
	param->value = malloc(l+1);
	for (i=0,j=0; i<l; i++,j++) {
		if (value[i] == '%' && i+2<l) {
			param->value[j] = hexchar(&value[i+1]);
			i += 2;
		}
		else if (value[i] == '+') {
			param->value[j] = ' ';
		}
		else {
			param->value[j] = value[i];
		}
	}	
	param->value[j] = 0;
	
	printf("key: '%s', value: '%s' \n", param->key, param->value);
	
	ll_push_tail(list, param);
}


static void cmdParams(rq_http_req_t *req, const risp_length_t length, const risp_data_t *data)
{
	assert(req);
	assert(length > 0);
	assert(data != NULL);

	// make sure there is at least something in the data buffer.
	assert(data[0] != 0);
	
	// copy the params data into a null terminated string.
	assert(req->params == NULL);
	req->params = (char *) malloc(length+1);
	assert(req->params);
	memcpy(req->params, data, length);
	req->params[length] = '\0';
	
	// create an empty parameters list.
	assert(req->param_list == NULL);
	req->param_list = ll_init(NULL);
	assert(req->param_list);

	// parse the parameters into the param_list.
	parse_params(req->params, param_handler, req->param_list);
}

static void cmdCode(rq_http_req_t *req, risp_int_t value)
{
	assert(req);
	
	assert(req->code == 0);
	req->code = value;
}


//-----------------------------------------------------------------------------
// This callback function is used when a complete message is received to
// consume.  We basically need to create a request to handle it, add it to the
// list.  If a reply is sent during the processing, then it will close out the
// request automatically, otherwise it will be up to something else to close it
// out.
static void message_handler(rq_message_t *msg, void *arg)
{
	int processed;
	rq_http_t *http;
	rq_http_req_t *req;

	assert(msg);
	assert(arg);
	
	http = (rq_http_t *) arg;
	assert(http);

	// We dont know what the use of this object will be, so we need to create it
	// and put it in a list (to keep track of it) until something gets rid of it.
	req = req_new(http, http->arg);
	req->msg = msg;

	assert(req->reply);
	assert(BUF_LENGTH(req->reply) == 0);

	assert(msg->data);
	assert(http->risp);
	processed = risp_process(http->risp, req, BUF_LENGTH(msg->data), BUF_DATA(msg->data));
	assert(processed == BUF_LENGTH(msg->data));

	// if we still have the msg pointer as part of the request, then the message
	// hasn't been replied yet, so we need to add the request to the list and
	// let it finish elsewhere. 
	if (req->msg) {
		assert(req->inprocess == 0);
		req->inprocess++;

		// then we need to add this request to the list.
		assert(http->req_list);
		ll_push_head(http->req_list, req);
		req = NULL;
	}
	else {
		// We have already replied to the request, so we dont need it anymore.
		req_free(req);
		req = NULL;
	}
}



rq_http_t * rq_http_new (rq_t *rq, char *queue, void (*handler)(rq_http_req_t *req), void *arg)
{
	rq_http_t *http;

	assert(rq);
	assert(queue);
	assert(handler);
	assert((arg && handler) || (arg == NULL));

	// create http object.
	http = malloc(sizeof(*http));
	http->rq = rq;
	http->queue = strdup(queue);
	assert(strlen(queue) < 256);
	http->handler = handler;
	http->arg = arg;

	http->req_list = ll_init(NULL);
	assert(http->req_list);
	
	http->safe_buffer = NULL;
	http->safe_len = 0;

	// create RISP object
	http->risp = risp_init(NULL);
	assert(http->risp);
	risp_add_command(http->risp, HTTP_CMD_CLEAR, 	   &cmdClear);
	risp_add_command(http->risp, HTTP_CMD_EXECUTE,     &cmdExecute);
	risp_add_command(http->risp, HTTP_CMD_METHOD_GET,  &cmdMethodGet);
	risp_add_command(http->risp, HTTP_CMD_METHOD_POST, &cmdMethodPost);
	risp_add_command(http->risp, HTTP_CMD_METHOD_HEAD, &cmdMethodHead);
	risp_add_command(http->risp, HTTP_CMD_HOST,        &cmdHost);
	risp_add_command(http->risp, HTTP_CMD_PATH,        &cmdPath);
	risp_add_command(http->risp, HTTP_CMD_PARAMS,      &cmdParams);
	risp_add_command(http->risp, HTTP_CMD_CODE,        &cmdCode);

// 	risp_add_command(http->risp, HTTP_CMD_SET_HEADER,  &cmdHeader);
// 	risp_add_command(http->risp, HTTP_CMD_LENGTH,      &cmdLength);
// 	risp_add_command(http->risp, HTTP_CMD_REMOTE_HOST, &cmdRemoteHost);
// 	risp_add_command(http->risp, HTTP_CMD_LANGUAGE,    &cmdLanguage);
// 	risp_add_command(http->risp, HTTP_CMD_FILE,        &cmdParams);
// 	risp_add_command(http->risp, HTTP_CMD_KEY,         &cmdKey);
// 	risp_add_command(http->risp, HTTP_CMD_VALUE,       &cmdValue);
// 	risp_add_command(http->risp, HTTP_CMD_FILENAME,    &cmdFilename);

	rq_consume(rq, http->queue, 200, RQ_PRIORITY_NORMAL, 0, message_handler, NULL, NULL, http);

	return(http);
}


void rq_http_free(rq_http_t *http)
{
	assert(http);

	assert(http->risp);
	http->risp = risp_shutdown(http->risp);
	assert(http->risp == NULL);

	if (http->safe_buffer) {
		assert(http->safe_len >= 0);
		free(http->safe_buffer);
		http->safe_buffer = NULL;
		http->safe_len = 0;
	}
	
	assert(http->queue);
	free(http->queue);
	http->queue  = NULL;

	http->rq = NULL;
	http->handler = NULL;
	http->arg = NULL;

	assert(http->req_list);
	assert(ll_count(http->req_list) == 0);
	http->req_list = ll_free(http->req_list);
	assert(http->req_list == NULL);

	free(http);
}


//-----------------------------------------------------------------------------
// given a filename, will return the mime-type for it, based on the extension.
//
// NOTE: variable names are shortened because we use them a lot in all the
//       comparisons.  Long variables just clutter things up.
//       To clarify, f=filename, l=length.
char * rq_http_getmimetype(char *f)
{
	char *ext;
	
	// find the last occurance of '.'.
	assert(f);
	ext = rindex(f, '.');
	if (ext) {
		
		// rindex returned a pointer to the '.', so we go to the next character.
		ext++;
		assert(*ext != '\0');
		
		if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) return "text/html";
		if (strcmp(ext, "jpeg") == 0 || strcmp(ext, "jpg") == 0) return "image/jpeg";
	}
	
	// if the file wasn't found elsewhere, then use the default.
	return "text/plain";
}



//-----------------------------------------------------------------------------
// External function that is used to reply to a http request.
void rq_http_reply(rq_http_req_t *req, const int code, char *ctype, expbuf_t *data)
{
	rq_http_t *http;
	
	assert(req && code > 0);
	
	assert(req->reply);
	assert(BUF_LENGTH(req->reply) == 0);
	
	addCmd(req->reply, HTTP_CMD_CLEAR);
	if (ctype) { addCmdShortStr(req->reply, HTTP_CMD_CONTENT_TYPE, strlen(ctype), ctype); }
	if (data && BUF_LENGTH(data)) { addCmdLargeStr(req->reply, HTTP_CMD_FILE, BUF_LENGTH(data), BUF_DATA(data)); }
	addCmdInt(req->reply, HTTP_CMD_CODE, code);
	addCmd(req->reply, HTTP_CMD_REPLY);

	// If we already have a reply, then we send it and then close off the request object.
	assert(req->msg);
	rq_reply(req->msg, BUF_LENGTH(req->reply), BUF_DATA(req->reply));
	expbuf_clear(req->reply);
	req->msg = NULL;

	if (req->inprocess > 0) {

		// need to remove the request from the list.
		assert(req->http);
		http = req->http;
		
		assert(http->req_list);
		ll_remove(http->req_list, req);

		req_free(req);
		req = NULL;
	}
}

// Return the path of the request.
char * rq_http_getpath(rq_http_req_t *req)
{
	assert(req);
	assert(req->path);
	return(req->path);
}


//-----------------------------------------------------------------------------
// Look in the internal params list and return the value if the name exists.  
// If the name doesnt exist, then return NULL.
const char * rq_http_param(rq_http_req_t *req, char *name)
{
	param_t *param;
	char *value = NULL;
	
	assert(req);
	assert(name);
	
	if(req->param_list) {
		ll_start(req->param_list);
		param = ll_next(req->param_list);
		while (param) {
			assert(param->key);
			assert(param->value);
			if (strcmp(param->key, name) == 0) {
				value = param->value;
				param = NULL;
			}
			else {
				param = ll_next(req->param_list);
			}
		}
		ll_finish(req->param_list);
	}
	
	return(value);
}


// take some text that needs to be used as input to a form (or similar).  This means that it has to expand " chars.
const char * rq_http_safe_input(rq_http_t *http, const char *text)
{
	int tlen;
	int i, j;
	
	assert(http && text);
	
	// get the length of the original string.
	tlen = strlen(text);

	// if our buffer is not potentially big enough, then we want to increase it.
	if (http->safe_buffer == NULL) {
		http->safe_len = tlen * 5;
		http->safe_buffer = malloc(http->safe_len + 1);
	}
	else if ((tlen * 5) > http->safe_len) {
		http->safe_len = tlen * 5;
		http->safe_buffer = realloc(http->safe_buffer, http->safe_len + 1);
	}
	assert(http->safe_buffer);
	
	for (i=0,j=0; i<tlen; i++) {
		if (text[i] == '"') {
			http->safe_buffer[j++] = '&';
			http->safe_buffer[j++] = 'q';
			http->safe_buffer[j++] = 'u';
			http->safe_buffer[j++] = 'o';
			http->safe_buffer[j++] = 't';
			http->safe_buffer[j++] = ';';
		}
		else if (text[i] == '&') {
			http->safe_buffer[j++] = '&';
			http->safe_buffer[j++] = 'a';
			http->safe_buffer[j++] = 'm';
			http->safe_buffer[j++] = 'p';
			http->safe_buffer[j++] = ';';
		}
		else if (text[i] == '<') {
			http->safe_buffer[j++] = '&';
			http->safe_buffer[j++] = 'l';
			http->safe_buffer[j++] = 't';
			http->safe_buffer[j++] = ';';
		}
		else if (text[i] == '>') {
			http->safe_buffer[j++] = '&';
			http->safe_buffer[j++] = 'g';
			http->safe_buffer[j++] = 't';
			http->safe_buffer[j++] = ';';
		}
		else {
			http->safe_buffer[j++] = text[i];
		}
	}
	assert(j >= i);
	assert(j <= http->safe_len);
	http->safe_buffer[j] = 0;
	
	return(http->safe_buffer);
}


