
/*
* Copyright 2016 Anthony Deroche <anthony@deroche.me>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>

// RFC 7519 compliant library
#include <jwt.h>

#include "apr_strings.h"
#include "apr_lib.h"                /* for apr_isspace */

#include "ap_config.h"
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "ap_provider.h"

#include "mod_auth.h"

#define JWT_LOGIN_HANDLER "jwt-login-handler"
#define JWT_LOGOUT_HANDLER "jwt-login-handler"
#define USER_INDEX 0
#define PASSWORD_INDEX 1
#define FORM_SIZE 512


/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  CONFIGURATION STRUCTURE ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */

typedef struct {
    authn_provider_list *providers;

    const char* signature_algorithm;
    int signature_algorithm_set;

    const char* signature_secret;
    int signature_secret_set;

    int exp_delay;
    int exp_delay_set;

    int nbf_delay;
    int nbf_delay_set;

    int leeway;
    int leeway_set;

    const char* iss;
    int iss_set;

    const char* sub;
    int sub_set;

    const char* aud;
    int aud_set;

    char *dir;

} auth_jwt_config_rec;

typedef enum { dir_signature_algorithm, dir_signature_secret, dir_exp_delay, dir_nbf_delay, dir_iss, dir_sub, dir_aud, dir_leeway} jwt_directive;
//typedef struct jwt_t token_t;

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  FUNCTIONS HEADERS ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */


static void *create_auth_jwt_dir_config(apr_pool_t *p, char *d);
static void *create_auth_jwt_config(apr_pool_t * p, server_rec *s);
static void register_hooks(apr_pool_t * p);

static const char *add_authn_provider(cmd_parms * cmd, void *config, const char *arg);
static const char *set_jwt_param(cmd_parms * cmd, void* config, const char* value);
static const char *set_jwt_int_param(cmd_parms * cmd, void* config, const char* value);
static void* get_config_value(request_rec *r, jwt_directive directive);

static int check_key_length(request_rec *r, const char* key, const char* algorithm);
static int auth_jwt_login_handler(request_rec *r);
static int check_authn(request_rec *r, const char *username, const char *password);
static int create_token(request_rec *r, char** token_str, const char* username);

static int auth_jwt_authn_with_token(request_rec *r);

static int token_check(request_rec *r, jwt_t **jwt, const char *token, const unsigned char *key);
static int token_new(jwt_t **jwt);
static const char* token_get_claim(jwt_t *token, const char* claim);
static int token_add_claim(jwt_t *jwt, const char *claim, const char *val);
static void token_free(jwt_t *token);
static int token_set_alg(jwt_t *jwt, jwt_alg_t alg, unsigned char *key, int len);
static char *token_encode_str(jwt_t *jwt);


/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  DECLARE DIRECTIVES ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */

static const command_rec auth_jwt_cmds[] =
{

   AP_INIT_TAKE1("AuthJWTSignatureAlgorithm", set_jwt_param, (void *)dir_signature_algorithm, RSRC_CONF|ACCESS_CONF,
                    "The algorithm to use to sign tokens"),
   AP_INIT_TAKE1("AuthJWTSignatureSecret", set_jwt_param, (void *)dir_signature_secret, RSRC_CONF|ACCESS_CONF,
                     "The secret to use to sign tokens with HMACs"),
   AP_INIT_TAKE1("AuthJWTIss", set_jwt_param, (void *)dir_iss, RSRC_CONF|ACCESS_CONF,
                     "The issuer of delievered tokens"),
   AP_INIT_TAKE1("AuthJWTSub", set_jwt_param, (void *)dir_sub, RSRC_CONF|ACCESS_CONF,
                     "The subject of delivered tokens"),
   AP_INIT_TAKE1("AuthJWTAud", set_jwt_param, (void *)dir_aud, RSRC_CONF|ACCESS_CONF,
                     "The audience of delivered tokens"),
   AP_INIT_TAKE1("AuthJWTExpDelay", set_jwt_int_param, (void *)dir_exp_delay, RSRC_CONF|ACCESS_CONF,
                     "The time delay in seconds after which delivered tokens are considered invalid"),
   AP_INIT_TAKE1("AuthJWTNbfDelay", set_jwt_int_param, (void *)dir_nbf_delay, RSRC_CONF|ACCESS_CONF,
                     "The time delay in seconds before which delivered tokens must not be processed"),
   AP_INIT_TAKE1("AuthJWTLeeway", set_jwt_int_param, (void *)dir_leeway, RSRC_CONF|ACCESS_CONF,
                     "The leeway to account for clock skew in token validation process"),
   AP_INIT_ITERATE("AuthJWTProvider", add_authn_provider, NULL, ACCESS_CONF,
                "Specify the auth providers for a directory or location"),
    {NULL}
};

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  DEFAULT CONFIGURATION ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */

/* PER DIR CONFIGURATION */
static void *create_auth_jwt_dir_config(apr_pool_t *p, char *d){
    auth_jwt_config_rec *conf = (auth_jwt_config_rec*) apr_pcalloc(p, sizeof(*conf));
    conf->dir = d;
    //conf->form_size = HUGE_STRING_LEN;

    conf->leeway = 0;
    conf->exp_delay = 3600;
    conf->nbf_delay = 0;

    conf->signature_algorithm_set = 0;
    conf->signature_secret_set = 0;
    conf->exp_delay_set = 0;
    conf->nbf_delay_set = 0;
    conf->leeway_set = 0;
    conf->iss_set = 0;
    conf->aud_set = 0;
    conf->sub_set = 0;

    return (void *)conf;
}

/* GLOBAL CONFIGURATION */
static void *create_auth_jwt_config(apr_pool_t * p, server_rec *s){
    auth_jwt_config_rec *conf = (auth_jwt_config_rec*) apr_pcalloc(p, sizeof(*conf));

    conf->signature_algorithm_set = 0;
    conf->signature_secret_set = 0;
    conf->exp_delay_set = 0;
    conf->nbf_delay_set = 0;
    conf->leeway_set = 0;
    conf->iss_set = 0;
    conf->aud_set = 0;
    conf->sub_set = 0;

    return (void *)conf;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  DECLARE MODULE IN HTTPD CORE ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */

AP_DECLARE_MODULE(auth_jwt) = {
  STANDARD20_MODULE_STUFF,
  create_auth_jwt_dir_config,
  NULL,
  create_auth_jwt_config,
  NULL,
  auth_jwt_cmds,
  register_hooks
};


/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  FILL OUT CONF STRUCTURES ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */

static void* get_config_value(request_rec *r, jwt_directive directive){
    auth_jwt_config_rec *dconf = (auth_jwt_config_rec *) ap_get_module_config(r->per_dir_config,
                                                    &auth_jwt_module);

    auth_jwt_config_rec *sconf = (auth_jwt_config_rec *) ap_get_module_config(r->server->module_config,
                                                    &auth_jwt_module);
    void* value;

    switch ((jwt_directive) directive) {
        case dir_signature_algorithm:
            if(dconf->signature_algorithm_set && dconf->signature_algorithm){
                value = (void*)dconf->signature_algorithm;
            }else if(sconf->signature_algorithm){
                value = (void*)sconf->signature_algorithm;
            }else{
                return NULL;
            }
            break;
        case dir_signature_secret:
            if(dconf->signature_secret_set && dconf->signature_secret){
                value = (void*)dconf->signature_secret;
            }else if(sconf->signature_algorithm_set && sconf->signature_secret){
                value = (void*)sconf->signature_secret;
            }else{
                return NULL;
            }
            break;
        case dir_iss:
            if(dconf->iss_set && dconf->iss){
                value = (void*)dconf->iss;
            }else if(sconf->iss_set && sconf->iss){
                value = (void*)sconf->iss;
            }else{
                return NULL;
            }
            break;
        case dir_aud:
            if(dconf->aud_set && dconf->aud){
                value = (void*)dconf->aud;
            }else if(sconf->iss_set && sconf->aud){
                value = (void*)sconf->aud;
            }else{
                return NULL;
            }
            break;
        case dir_sub:
            if(dconf->sub_set && dconf->sub){
                value = (void*)dconf->sub;
            }else if(sconf->sub_set && sconf->sub){
                value = (void*)sconf->sub;
            }else{
                return NULL;
            }
            break;
        case dir_exp_delay:
            if(dconf->exp_delay_set){
                value = (void*)&dconf->exp_delay;
            }else if(sconf->exp_delay_set){
                value = (void*)&sconf->exp_delay;
            }else{
                return NULL;
            }
            break;
        case dir_nbf_delay:
            if(dconf->nbf_delay_set){
                value = (void*)&dconf->nbf_delay;
            }else if(sconf->nbf_delay_set){
                value = (void*)&sconf->nbf_delay;
            }else{
                return NULL;
            }
            break;
        case dir_leeway:
            if(dconf->leeway){
                value = (void*)&dconf->leeway;
            }else if(sconf->leeway_set){
                value = (void*)&sconf->leeway;
            }else{
                return NULL;
            }
            break;
        default:
            return NULL;
    }
    return value;
}


/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  REGISTER HOOKS ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */

static void register_hooks(apr_pool_t * p){
  ap_hook_handler(auth_jwt_login_handler, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_check_authn(auth_jwt_authn_with_token, NULL, NULL, APR_HOOK_MIDDLE,
                        AP_AUTH_INTERNAL_PER_CONF);
}


/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  DIRECTIVE HANDLERS ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */

static const char *add_authn_provider(cmd_parms * cmd, void *config,
                                           const char *arg)
{
    auth_jwt_config_rec *conf = (auth_jwt_config_rec *) config;
    authn_provider_list *newp;

    newp = apr_pcalloc(cmd->pool, sizeof(authn_provider_list));
    newp->provider_name = arg;

    newp->provider = ap_lookup_provider(AUTHN_PROVIDER_GROUP,
                                        newp->provider_name,
                                        AUTHN_PROVIDER_VERSION);

    if (newp->provider == NULL) {
        return apr_psprintf(cmd->pool,
                            "Unknown Authn provider: %s",
                            newp->provider_name);
    }

    if (!newp->provider->check_password) {
        return apr_psprintf(cmd->pool,
                            "The '%s' Authn provider doesn't support "
                            "Form Authentication", newp->provider_name);
    }

    if (!conf->providers) {
        conf->providers = newp;
    }
    else {
        authn_provider_list *last = conf->providers;

        while (last->next) {
            last = last->next;
        }
        last->next = newp;
    }

    return NULL;
}

static const char *set_jwt_param(cmd_parms * cmd, void* config, const char* value){

    auth_jwt_config_rec *conf;
    if(!cmd->path){
        conf = (auth_jwt_config_rec *) ap_get_module_config(cmd->server->module_config, &auth_jwt_module);
    }else{
        conf = (auth_jwt_config_rec *) config;
    }

    switch ((jwt_directive) cmd->info) {
        case dir_signature_algorithm:
            conf->signature_algorithm = value;
            conf->signature_algorithm_set = 1;
        break;
        case dir_signature_secret:
            conf->signature_secret = value;
            conf->signature_secret_set = 1;
        break;
        case dir_iss:
            conf->iss = value;
            conf->iss_set = 1;
        break;
        case dir_aud:
            conf->aud = value;
            conf->aud_set = 1;
        break;
        case dir_sub:
            conf->sub = value;
            conf->sub_set = 1;
        break;
    }

  return NULL;
}

static const char *set_jwt_int_param(cmd_parms * cmd, void* config, const char* value){

    auth_jwt_config_rec *conf;
    if(!cmd->path){
        conf = (auth_jwt_config_rec *) ap_get_module_config(cmd->server->module_config, &auth_jwt_module);
    }else{
        conf = (auth_jwt_config_rec *) config;
    }

    const char *digit;
    for (digit = value; *digit; ++digit) {
        if (!apr_isdigit(*digit)) {
            return "Argument must be numeric!";
        }
    }

    switch ((long) cmd->info) {
        case dir_exp_delay:
            conf->exp_delay = atoi(value);
            conf->exp_delay_set = 1;
        break;
        case dir_nbf_delay:
            conf->nbf_delay = atoi(value);
            conf->nbf_delay_set = 1;
        break;
        case dir_leeway:
            conf->leeway = atoi(value);
            conf->leeway_set = 1;
        break;
    }
    return NULL;
}


/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  AUTHENTICATION HANDLERS ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */


static int auth_jwt_login_handler(request_rec *r){

  if(!r->handler || strcmp(r->handler, JWT_LOGIN_HANDLER)){
    return DECLINED;
  }

  int res;
  char* buffer;
  apr_off_t len;
  apr_size_t size;
  int rv;

  if(r->method_number != M_POST){
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01811)
          "the " JWT_LOGIN_HANDLER " only supports the POST method for %s",
                      r->uri);
    return HTTP_METHOD_NOT_ALLOWED;
  }

  apr_array_header_t *pairs = NULL;
  res = ap_parse_form_data(r, NULL, &pairs, -1, FORM_SIZE);
  if (res != OK) {
    return res;
  }
  char* fields_name[] = {"user", "password"};
  char* fields[] = {fields_name[USER_INDEX], fields_name[PASSWORD_INDEX]};

  char* sent_values[2];

  int i;
  while (pairs && !apr_is_empty_array(pairs)) {
    ap_form_pair_t *pair = (ap_form_pair_t *) apr_array_pop(pairs);
    for(i=0;i<2;i++){
      if (fields[i] && !strcmp(pair->name, fields[i]) && &sent_values[i]) {
        apr_brigade_length(pair->value, 1, &len);
        size = (apr_size_t) len;
        buffer = apr_palloc(r->pool, size + 1);
        apr_brigade_flatten(pair->value, buffer, &size);
        buffer[len] = 0;
        sent_values[i] = buffer;

      }
    }
  }

  for(i=0;i<2;i++){
    if(!sent_values[i]){
      return HTTP_UNAUTHORIZED;
    }
  }

  r->user = sent_values[USER_INDEX];

  rv = check_authn(r, sent_values[USER_INDEX], sent_values[PASSWORD_INDEX]);

  if(rv == OK){
    char* token;
    rv = create_token(r, &token, sent_values[USER_INDEX]);
    if(rv == OK){
      apr_table_setn(r->err_headers_out, "Content-Type", "application/json");
      ap_rprintf(r, "{\"token\":\"%s\"}", token);
      free(token);
    }
  }

  return rv;
}


static int create_token(request_rec *r, char** token_str, const char* username){
    jwt_t *token;
    int allocate = token_new(&token);

    char* signature_secret = (char*)get_config_value(r, dir_signature_secret);
    char* signature_algorithm = (char *)get_config_value(r, dir_signature_algorithm);
    char* iss = (char *)get_config_value(r, dir_iss);
    char* aud = (char *)get_config_value(r, dir_aud);
    char* sub = (char *)get_config_value(r, dir_sub);
    int* exp_delay_ptr = (int*)get_config_value(r, dir_exp_delay);
    int* nbf_delay_ptr = (int*)get_config_value(r, dir_nbf_delay);

    if(!signature_secret){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)
                      "You must specify AuthJWTSignatureSecret directive in configuration");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if(!signature_algorithm){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)
            "Cannot retrieve specified signature algorithm. This error should not happen since a default algorithm is set.");
        return HTTP_INTERNAL_SERVER_ERROR;
    }


    if(check_key_length(r, signature_secret, signature_algorithm)!=OK){
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if(!strcmp(signature_algorithm, "HS512")){
        token_set_alg(token, JWT_ALG_HS512, (unsigned char*)signature_secret, 64);
    }else if(!strcmp(signature_algorithm, "HS384")){
        token_set_alg(token, JWT_ALG_HS256, (unsigned char*)signature_secret, 48);
    }else if(!strcmp(signature_algorithm, "HS256")){
        token_set_alg(token, JWT_ALG_HS256, (unsigned char*)signature_secret, 32);
    }else{
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    time_t now = time(NULL);
    time_t iat = now;
    time_t exp = now;
    time_t nbf = now;

    char time_buffer_str[11];
    if(exp_delay_ptr && *exp_delay_ptr >= 0){
        exp += *exp_delay_ptr;
        sprintf(time_buffer_str, "%ld", exp);
        token_add_claim(token, "exp", time_buffer_str);
    }

    if(nbf_delay_ptr && *nbf_delay_ptr >= 0){
        nbf += *nbf_delay_ptr;
        sprintf(time_buffer_str, "%ld", nbf);
        token_add_claim(token, "nbf", time_buffer_str);
    }

    sprintf(time_buffer_str, "%ld", iat);
    token_add_claim(token, "iat", time_buffer_str);

    if(iss){
        token_add_claim(token, "iss", iss);
    }

    if(sub){
        token_add_claim(token, "sub", sub);
    }

    if(aud){
        token_add_claim(token, "aud", aud);
    }

    token_add_claim(token, "user", username);

    *token_str = token_encode_str(token);
    token_free(token);
    return OK;
}

static int check_authn(request_rec *r, const char *username, const char *password){
    authn_status authn_result;
    authn_provider_list *current_provider;
    auth_jwt_config_rec *conf = ap_get_module_config(r->per_dir_config,
                                                      &auth_jwt_module);

    current_provider = conf->providers;
    do {
        const authn_provider *provider;

        if (!current_provider) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01806)
                          "no authn provider configured");
            authn_result = AUTH_GENERAL_ERROR;
            break;
        }
        else {
            provider = current_provider->provider;
            apr_table_setn(r->notes, AUTHN_PROVIDER_NAME_NOTE, current_provider->provider_name);
        }

        if (!username || !password) {
            authn_result = AUTH_USER_NOT_FOUND;
            break;
        }

        authn_result = provider->check_password(r, username, password);

        apr_table_unset(r->notes, AUTHN_PROVIDER_NAME_NOTE);

        if (authn_result != AUTH_USER_NOT_FOUND) {
            break;
        }

        if (!conf->providers) {
            break;
        }

        current_provider = current_provider->next;
    } while (current_provider);

    if (authn_result != AUTH_GRANTED) {
        int return_code;

        /*if (authn_result != AUTH_DENIED) && !(conf->authoritative))
            return DECLINED;
        }*/

        switch (authn_result) {
          case AUTH_DENIED:
              ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01807)
                            "user '%s': authentication failure for \"%s\": "
                            "password Mismatch",
                            username, r->uri);
              return_code = HTTP_UNAUTHORIZED;
              break;
          case AUTH_USER_NOT_FOUND:
              ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01808)
                            "user '%s' not found: %s", username, r->uri);
              return_code = HTTP_UNAUTHORIZED;
              break;
          case AUTH_GENERAL_ERROR:
          default:
              return_code = HTTP_INTERNAL_SERVER_ERROR;
              break;
        }

        return return_code;
    }

    return OK;
}


/*
If we are configured to handle authentication, let's look up headers to find
whether or not 'Authorization' is set. If so, exepected format is
Authorization: Bearer json_web_token. Then we check if the token is valid.
*/
static int auth_jwt_authn_with_token(request_rec *r){
    const char *current_auth = NULL;
    current_auth = ap_auth_type(r);
    int rv;

    if (!current_auth || strcmp(current_auth, "jwt")) {
        return DECLINED;
    }

    /* We need an authentication realm. */
    if (!ap_auth_name(r)) {
       ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)
                     "need AuthName: %s", r->uri);
       return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (!ap_auth_name(r)) {
         ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)
                       "need AuthName: %s", r->uri);
         return HTTP_INTERNAL_SERVER_ERROR;
    }

    r->ap_auth_type = (char *) current_auth;

    char* authorization_header = (char*)apr_table_get( r->headers_in, "Authorization");
    char* token_str;

    char* signature_secret = (char*)get_config_value(r, dir_signature_secret);
    if(signature_secret == NULL){
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)
                    "You must specify AuthJWTSignatureSecret directive in configuration");
      return HTTP_INTERNAL_SERVER_ERROR;
    }

    if(!authorization_header){
        apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool, "Bearer realm=\"", ap_auth_name(r),"\"", NULL));
        return HTTP_UNAUTHORIZED;
    }

    int header_len = strlen(authorization_header);
    if(header_len > 7 && !strncmp(authorization_header, "Bearer ", 7)){
        token_str = authorization_header+7;
        jwt_t* token;
        rv = token_check(r, &token, token_str, signature_secret);
        if(OK == rv){
            char* maybe_user = (char *)token_get_claim(token, "user");
            if(maybe_user == NULL){
                ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)
                  "Username was not in token");
                apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool,
                  "Bearer realm=\"", ap_auth_name(r),"\", error=\"invalid_token\", error_description=\"Username was not in token\"",
                   NULL));
                return HTTP_UNAUTHORIZED;
            }
            r->user = maybe_user;
            return OK;
        }else{
            return rv;
        }

        if(token)
            token_free(token);
    }else{
        apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool,
          "Bearer realm=\"", ap_auth_name(r),"\", error=\"invalid_request\", error_description=\"Authentication type must be Bearer\"",
           NULL));
        return HTTP_BAD_REQUEST;
    }
}

static int check_key_length(request_rec *r, const char* key, const char* algorithm){
    int key_len = (int)strlen(key);
    if(!strcmp(algorithm, "HS512")){
        if(key_len!=64){
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)
                "The secret length must be 64 with HMAC SHA512 algorithm");
            return HTTP_INTERNAL_SERVER_ERROR;
        }
    }else if(!strcmp(algorithm, "HS384")){
        if(key_len!=32){
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)
                "The secret length must be 48 with HMAC SHA384 algorithm (current length is %d)", key_len);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
    }else if(!strcmp(algorithm, "HS256")){
        if(key_len!=32){
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)
                "The secret length must be 32 with HMAC SHA256 algorithm (current length is %d)", key_len);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
    }
    else{
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)
        "The only supported algorithms are HS256 (HMAC SHA256), HS384 (HMAC SHA384), and HS515 (HMAC SHA512)");
        return 2;
    }
    return OK;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  TOKEN OPERATIONS ~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */

static int token_new(jwt_t **jwt){
  return jwt_new(jwt);
}

static int token_check(request_rec *r, jwt_t **jwt, const char *token, const unsigned char *key){

    char* signature_secret = (char*)get_config_value(r, dir_signature_secret);
    char* signature_algorithm = (char *)get_config_value(r, dir_signature_algorithm);

    if(check_key_length(r, key, signature_algorithm)!=OK){
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    int decode_res = jwt_decode(jwt, token, key, strlen(key));

    if(decode_res != 0){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)"Decoding process has failed, token is malformed");
        apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool,
          "Bearer realm=\"", ap_auth_name(r),"\", error=\"invalid_token\", error_description=\"Token is malformed\"",
           NULL));
        return HTTP_UNAUTHORIZED;
    }

    /*
    Trunk of libjwt does not need this check because the bug is fixed
    We should not accept token with provided alg none
    */
    if(*jwt &&  jwt_get_alg(*jwt) == JWT_ALG_NONE){
        apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool,
          "Bearer realm=\"", ap_auth_name(r),"\", error=\"invalid_token\", error_description=\"Token is malformed\"",
           NULL));
        return HTTP_UNAUTHORIZED;
    }

    const char* iss_config = (char *)get_config_value(r, dir_iss);
    const char* aud_config = (char *)get_config_value(r, dir_aud);
    const char* sub_config = (char *)get_config_value(r, dir_sub);
    int leeway = *(int*)get_config_value(r, dir_leeway);

    const char* iss_to_check = token_get_claim(*jwt, "iss");
    if(iss_config && iss_to_check && strcmp(iss_config, iss_to_check)!=0){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)"Token issuer does not match with configured issuer.");
        apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool,
          "Bearer realm=\"", ap_auth_name(r),"\", error=\"invalid_token\", error_description=\"Issuer is not valid\"",
           NULL));
        return HTTP_UNAUTHORIZED;
    }

    const char* aud_to_check = token_get_claim(*jwt, "aud");
    if(aud_config && aud_to_check && strcmp(aud_config, aud_to_check)!=0){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)"Token audience does not match with configured audience.");
        apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool,
          "Bearer realm=\"", ap_auth_name(r),"\", error=\"invalid_token\", error_description=\"Audience is not valid\"",
           NULL));
        return HTTP_UNAUTHORIZED;
    }

    const char* sub_to_check = token_get_claim(*jwt, "sub");
    if(sub_config && sub_to_check && strcmp(sub_config, sub_to_check)!=0){
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)"Token subject does not match with configured subject.");
        apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool,
          "Bearer realm=\"", ap_auth_name(r),"\", error=\"invalid_token\", error_description=\"Subject is not valid\"",
           NULL));
        return HTTP_UNAUTHORIZED;
    }

    /* check exp */
    const char* exp_str = token_get_claim(*jwt, "exp");
    if(exp_str){
        int exp_int = atoi(exp_str);
        time_t now = time(NULL);
        if (exp_int + leeway < now){
            /* token expired */
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)"Token expired.");
            apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool,
              "Bearer realm=\"", ap_auth_name(r),"\", error=\"invalid_token\", error_description=\"Token expired\"",
               NULL));
            return HTTP_UNAUTHORIZED;
        }
    }else{
        /* exp is mandatory parameter */
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)"Missing exp in token.");
        apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool,
          "Bearer realm=\"", ap_auth_name(r),"\", error=\"invalid_token\", error_description=\"Expiration is missing in token\"",
           NULL));
        return HTTP_UNAUTHORIZED;
    }

    /* check nbf */
    const char* nbf_str = token_get_claim(*jwt, "nbf");
    if(nbf_str){
        int nbf_int = atoi(nbf_str);
        time_t now = time(NULL);
        if (nbf_int - leeway > now){
            /* token is too recent to be processed */
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(01810)"Nbf check failed. Token can't be processed now.");
            apr_table_setn(r->err_headers_out, "WWW-Authenticate", apr_pstrcat(r->pool,
              "Bearer realm=\"", ap_auth_name(r),"\", error=\"invalid_token\", error_description=\"Token can't be processed now due to nbf field\"",
               NULL));
            return HTTP_UNAUTHORIZED;
        }
    }
    return OK;
}

static char *token_encode_str(jwt_t *jwt){
    return jwt_encode_str(jwt);
}

static int token_add_claim(jwt_t *jwt, const char *claim, const char *val){
    return jwt_add_grant(jwt, claim, val);
}

static const char* token_get_claim(jwt_t *token, const char* claim){
    return jwt_get_grant(token, claim);
}

static int token_set_alg(jwt_t *jwt, jwt_alg_t alg, unsigned char *key, int len){
    return jwt_set_alg(jwt, alg, key, len);
}

static void token_free(jwt_t *token){
    jwt_free(token);
}
