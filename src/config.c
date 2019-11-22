/* Configuration file parsing and CONFIG GET/SET commands implementation.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "cluster.h"

#include <fcntl.h>
#include <sys/stat.h>

/*-----------------------------------------------------------------------------
 * Config file name-value maps.
 *----------------------------------------------------------------------------*/

typedef struct configEnum {
    const char *name;
    const int val;
} configEnum;

configEnum maxmemory_policy_enum[] = {
    {"volatile-lru", MAXMEMORY_VOLATILE_LRU},
    {"volatile-lfu", MAXMEMORY_VOLATILE_LFU},
    {"volatile-random",MAXMEMORY_VOLATILE_RANDOM},
    {"volatile-ttl",MAXMEMORY_VOLATILE_TTL},
    {"allkeys-lru",MAXMEMORY_ALLKEYS_LRU},
    {"allkeys-lfu",MAXMEMORY_ALLKEYS_LFU},
    {"allkeys-random",MAXMEMORY_ALLKEYS_RANDOM},
    {"noeviction",MAXMEMORY_NO_EVICTION},
    {NULL, 0}
};

configEnum syslog_facility_enum[] = {
    {"user",    LOG_USER},
    {"local0",  LOG_LOCAL0},
    {"local1",  LOG_LOCAL1},
    {"local2",  LOG_LOCAL2},
    {"local3",  LOG_LOCAL3},
    {"local4",  LOG_LOCAL4},
    {"local5",  LOG_LOCAL5},
    {"local6",  LOG_LOCAL6},
    {"local7",  LOG_LOCAL7},
    {NULL, 0}
};

configEnum loglevel_enum[] = {
    {"debug", LL_DEBUG},
    {"verbose", LL_VERBOSE},
    {"notice", LL_NOTICE},
    {"warning", LL_WARNING},
    {NULL,0}
};

configEnum supervised_mode_enum[] = {
    {"upstart", SUPERVISED_UPSTART},
    {"systemd", SUPERVISED_SYSTEMD},
    {"auto", SUPERVISED_AUTODETECT},
    {"no", SUPERVISED_NONE},
    {NULL, 0}
};

configEnum aof_fsync_enum[] = {
    {"everysec", AOF_FSYNC_EVERYSEC},
    {"always", AOF_FSYNC_ALWAYS},
    {"no", AOF_FSYNC_NO},
    {NULL, 0}
};

configEnum repl_diskless_load_enum[] = {
    {"disabled", REPL_DISKLESS_LOAD_DISABLED},
    {"on-empty-db", REPL_DISKLESS_LOAD_WHEN_DB_EMPTY},
    {"swapdb", REPL_DISKLESS_LOAD_SWAPDB},
    {NULL, 0}
};

/* Output buffer limits presets. */
clientBufferLimitsConfig clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT] = {
    {0, 0, 0}, /* normal */
    {1024*1024*256, 1024*1024*64, 60}, /* slave */
    {1024*1024*32, 1024*1024*8, 60}  /* pubsub */
};

/* Configuration values that require no special handling to set, get, load or 
 * rewrite. */
typedef struct boolConfigData {
    int *config; /* The pointer to the server config this value is stored in */
    const int default_value; /* The default value of the config on rewrite */
} boolConfigData;

typedef struct stringConfigData {
    char **config; /* The pointer to the server config this value is stored in */
    const char *default_value; /* The default value of the config on rewrite */
    int convert_empty_to_null; /* A boolean indicating if empty strings should 
                                  be stored as a NULL value. */
                                  
} stringConfigData;

typedef struct enumConfigData {
    int *config; /* The pointer to the server config this value is stored in */
    configEnum *enum_value; /* The underlying enum type this data represents */
    const int default_value; /* The default value of the config on rewrite */
} enumConfigData;

typedef enum numericType {
    NUMERIC_TYPE_INT, 
    NUMERIC_TYPE_LONG_LONG, 
    NUMERIC_TYPE_UNSIGNED_LONG, 
    NUMERIC_TYPE_SIZE_T
} numericType;

typedef struct numericConfigData {
    union {
        int *i;
        long long *ll;
        unsigned long *ul; 
        size_t *st;
    } config; /* The pointer to the numeric config this value is stored in */
    int is_memory; /* Indicates if this value can be loaded as a memory value */
    numericType numeric_type; /* An enum indicating the type of this value */
    long long lower_bound; /* The lower bound of this numeric value */
    long long upper_bound; /* The upper bound of this numeric value */
    const long long default_value; /* The default value of the config on rewrite */
} numericConfigData;

typedef union typeData {
    boolConfigData bool;
    stringConfigData string;
    enumConfigData enumd;
    numericConfigData numeric;
} typeData;

typedef struct typeInterface {
    /* Called on server start, should return 1 on success, 0 on error and should set err */
    int (*load)(typeData data, sds *argc, int argv, char **err);  
    /* Called on CONFIG SET, returns 1 on success, 0 on error */
    int (*set)(typeData data, sds value); 
    /* Called on CONFIG GET, required to add output to the client */
    void (*get)(client *c, typeData data);
    /* Called on CONFIG REWRITE, required to rewrite the config state */
    void (*rewrite)(typeData data, const char *name, struct rewriteConfigState *state);
} typeInterface;

typedef struct standardConfig {
    const char *name; /* The user visible name of this config */
    const char *alias; /* An alias that can also be used for this config */
    const int modifiable; /* Can this value be updated by CONFIG SET? */
    typeInterface interface; /* The function pointers that define the type interface */
    typeData data; /* The type specific data exposed used by the interface */
} standardConfig;

standardConfig configs[];

/*-----------------------------------------------------------------------------
 * Enum access functions
 *----------------------------------------------------------------------------*/

/* Get enum value from name. If there is no match INT_MIN is returned. */
int configEnumGetValue(configEnum *ce, char *name) {
    while(ce->name != NULL) {
        if (!strcasecmp(ce->name,name)) return ce->val;
        ce++;
    }
    return INT_MIN;
}

/* Get enum name from value. If no match is found NULL is returned. */
const char *configEnumGetName(configEnum *ce, int val) {
    while(ce->name != NULL) {
        if (ce->val == val) return ce->name;
        ce++;
    }
    return NULL;
}

/* Wrapper for configEnumGetName() returning "unknown" instead of NULL if
 * there is no match. */
const char *configEnumGetNameOrUnknown(configEnum *ce, int val) {
    const char *name = configEnumGetName(ce,val);
    return name ? name : "unknown";
}

/* Used for INFO generation. */
const char *evictPolicyToString(void) {
    return configEnumGetNameOrUnknown(maxmemory_policy_enum,server.maxmemory_policy);
}

/*-----------------------------------------------------------------------------
 * Config file parsing
 *----------------------------------------------------------------------------*/

int yesnotoi(char *s) {
    if (!strcasecmp(s,"yes")) return 1;
    else if (!strcasecmp(s,"no")) return 0;
    else return -1;
}

void appendServerSaveParams(time_t seconds, int changes) {
    server.saveparams = zrealloc(server.saveparams,sizeof(struct saveparam)*(server.saveparamslen+1));
    server.saveparams[server.saveparamslen].seconds = seconds;
    server.saveparams[server.saveparamslen].changes = changes;
    server.saveparamslen++;
}

void resetServerSaveParams(void) {
    zfree(server.saveparams);
    server.saveparams = NULL;
    server.saveparamslen = 0;
}

void queueLoadModule(sds path, sds *argv, int argc) {
    int i;
    struct moduleLoadQueueEntry *loadmod;

    loadmod = zmalloc(sizeof(struct moduleLoadQueueEntry));
    loadmod->argv = zmalloc(sizeof(robj*)*argc);
    loadmod->path = sdsnew(path);
    loadmod->argc = argc;
    for (i = 0; i < argc; i++) {
        loadmod->argv[i] = createRawStringObject(argv[i],sdslen(argv[i]));
    }
    listAddNodeTail(server.loadmodule_queue,loadmod);
}

void loadServerConfigFromString(char *config) {
    char *err = NULL;
    int linenum = 0, totlines, i;
    int slaveof_linenum = 0;
    sds *lines;

    lines = sdssplitlen(config,strlen(config),"\n",1,&totlines);

    for (i = 0; i < totlines; i++) {
        sds *argv;
        int argc;

        linenum = i+1;
        lines[i] = sdstrim(lines[i]," \t\r\n");

        /* Skip comments and blank lines */
        if (lines[i][0] == '#' || lines[i][0] == '\0') continue;

        /* Split into arguments */
        argv = sdssplitargs(lines[i],&argc);
        if (argv == NULL) {
            err = "Unbalanced quotes in configuration line";
            goto loaderr;
        }

        /* Skip this line if the resulting command vector is empty. */
        if (argc == 0) {
            sdsfreesplitres(argv,argc);
            continue;
        }
        sdstolower(argv[0]);

        /* Iterate the configs that are standard */
        int match = 0;
        for (standardConfig *config = configs; config->name != NULL; config++) {
            if ((!strcasecmp(argv[0],config->name) ||
                (config->alias && !strcasecmp(argv[0],config->alias)))) 
            {
                if (!config->interface.load(config->data, argv, argc, &err)) {
                    goto loaderr;
                }

                match = 1;
                break;
            }
        }

        if (match) {
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* Execute config directives */
        if (!strcasecmp(argv[0],"bind") && argc >= 2) {
            int j, addresses = argc-1;

            if (addresses > CONFIG_BINDADDR_MAX) {
                err = "Too many bind addresses specified"; goto loaderr;
            }
            for (j = 0; j < addresses; j++)
                server.bindaddr[j] = zstrdup(argv[j+1]);
            server.bindaddr_count = addresses;
        } else if (!strcasecmp(argv[0],"unixsocketperm") && argc == 2) {
            errno = 0;
            server.unixsocketperm = (mode_t)strtol(argv[1], NULL, 8);
            if (errno || server.unixsocketperm > 0777) {
                err = "Invalid socket file permissions"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"save")) {
            if (argc == 3) {
                int seconds = atoi(argv[1]);
                int changes = atoi(argv[2]);
                if (seconds < 1 || changes < 0) {
                    err = "Invalid save parameters"; goto loaderr;
                }
                appendServerSaveParams(seconds,changes);
            } else if (argc == 2 && !strcasecmp(argv[1],"")) {
                resetServerSaveParams();
            }
        } else if (!strcasecmp(argv[0],"dir") && argc == 2) {
            if (chdir(argv[1]) == -1) {
                serverLog(LL_WARNING,"Can't chdir to '%s': %s",
                    argv[1], strerror(errno));
                exit(1);
            }
        } else if (!strcasecmp(argv[0],"logfile") && argc == 2) {
            FILE *logfp;

            zfree(server.logfile);
            server.logfile = zstrdup(argv[1]);
            if (server.logfile[0] != '\0') {
                /* Test if we are able to open the file. The server will not
                 * be able to abort just for this problem later... */
                logfp = fopen(server.logfile,"a");
                if (logfp == NULL) {
                    err = sdscatprintf(sdsempty(),
                        "Can't open the log file: %s", strerror(errno));
                    goto loaderr;
                }
                fclose(logfp);
            }
        } else if (!strcasecmp(argv[0],"syslog-enabled") && argc == 2) {
            if ((server.syslog_enabled = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"syslog-ident") && argc == 2) {
            if (server.syslog_ident) zfree(server.syslog_ident);
            server.syslog_ident = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"include") && argc == 2) {
            loadServerConfig(argv[1],NULL);
        } else if (!strcasecmp(argv[0],"maxclients") && argc == 2) {
            server.maxclients = atoi(argv[1]);
            if (server.maxclients < 1) {
                err = "Invalid max clients limit"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"maxmemory") && argc == 2) {
            server.maxmemory = memtoll(argv[1],NULL);
	    } else if ((!strcasecmp(argv[0],"client-query-buffer-limit")) && argc == 2) {
             server.client_max_querybuf_len = memtoll(argv[1],NULL);
        } else if ((!strcasecmp(argv[0],"slaveof") ||
                    !strcasecmp(argv[0],"replicaof")) && argc == 3) {
            slaveof_linenum = linenum;
            server.masterhost = sdsnew(argv[1]);
            server.masterport = atoi(argv[2]);
            server.repl_state = REPL_STATE_CONNECT;
        } else if (!strcasecmp(argv[0],"repl-backlog-size") && argc == 2) {
            long long size = memtoll(argv[1],NULL);
            if (size <= 0) {
                err = "repl-backlog-size must be 1 or greater.";
                goto loaderr;
            }
            resizeReplicationBacklog(size);
        } else if (!strcasecmp(argv[0],"repl-backlog-ttl") && argc == 2) {
            server.repl_backlog_time_limit = atoi(argv[1]);
            if (server.repl_backlog_time_limit < 0) {
                err = "repl-backlog-ttl can't be negative ";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"activedefrag") && argc == 2) {
            if ((server.active_defrag_enabled = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
            if (server.active_defrag_enabled) {
#ifndef HAVE_DEFRAG
                err = "active defrag can't be enabled without proper jemalloc support"; goto loaderr;
#endif
            }
        } else if (!strcasecmp(argv[0],"hz") && argc == 2) {
            server.config_hz = atoi(argv[1]);
            if (server.config_hz < CONFIG_MIN_HZ) server.config_hz = CONFIG_MIN_HZ;
            if (server.config_hz > CONFIG_MAX_HZ) server.config_hz = CONFIG_MAX_HZ;
        } else if (!strcasecmp(argv[0],"appendonly") && argc == 2) {
            if ((server.aof_enabled = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
            server.aof_state = server.aof_enabled ? AOF_ON : AOF_OFF;
        } else if (!strcasecmp(argv[0],"appendfilename") && argc == 2) {
            if (!pathIsBaseName(argv[1])) {
                err = "appendfilename can't be a path, just a filename";
                goto loaderr;
            }
            zfree(server.aof_filename);
            server.aof_filename = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"requirepass") && argc == 2) {
            if (strlen(argv[1]) > CONFIG_AUTHPASS_MAX_LEN) {
                err = "Password is longer than CONFIG_AUTHPASS_MAX_LEN";
                goto loaderr;
            }
            /* The old "requirepass" directive just translates to setting
             * a password to the default user. */
            ACLSetUser(DefaultUser,"resetpass",-1);
            sds aclop = sdscatprintf(sdsempty(),">%s",argv[1]);
            ACLSetUser(DefaultUser,aclop,sdslen(aclop));
            sdsfree(aclop);
        } else if (!strcasecmp(argv[0],"dbfilename") && argc == 2) {
            if (!pathIsBaseName(argv[1])) {
                err = "dbfilename can't be a path, just a filename";
                goto loaderr;
            }
            zfree(server.rdb_filename);
            server.rdb_filename = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"list-max-ziplist-entries") && argc == 2){
            /* DEAD OPTION */
        } else if (!strcasecmp(argv[0],"list-max-ziplist-value") && argc == 2) {
            /* DEAD OPTION */
        } else if (!strcasecmp(argv[0],"rename-command") && argc == 3) {
            struct redisCommand *cmd = lookupCommand(argv[1]);
            int retval;

            if (!cmd) {
                err = "No such command in rename-command";
                goto loaderr;
            }

            /* If the target command name is the empty string we just
             * remove it from the command table. */
            retval = dictDelete(server.commands, argv[1]);
            serverAssert(retval == DICT_OK);

            /* Otherwise we re-add the command under a different name. */
            if (sdslen(argv[2]) != 0) {
                sds copy = sdsdup(argv[2]);

                retval = dictAdd(server.commands, copy, cmd);
                if (retval != DICT_OK) {
                    sdsfree(copy);
                    err = "Target command name already exists"; goto loaderr;
                }
            }
        } else if (!strcasecmp(argv[0],"cluster-enabled") && argc == 2) {
            if ((server.cluster_enabled = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"cluster-config-file") && argc == 2) {
            zfree(server.cluster_configfile);
            server.cluster_configfile = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"client-output-buffer-limit") &&
                   argc == 5)
        {
            int class = getClientTypeByName(argv[1]);
            unsigned long long hard, soft;
            int soft_seconds;

            if (class == -1 || class == CLIENT_TYPE_MASTER) {
                err = "Unrecognized client limit class: the user specified "
                "an invalid one, or 'master' which has no buffer limits.";
                goto loaderr;
            }
            hard = memtoll(argv[2],NULL);
            soft = memtoll(argv[3],NULL);
            soft_seconds = atoi(argv[4]);
            if (soft_seconds < 0) {
                err = "Negative number of seconds in soft limit is invalid";
                goto loaderr;
            }
            server.client_obuf_limits[class].hard_limit_bytes = hard;
            server.client_obuf_limits[class].soft_limit_bytes = soft;
            server.client_obuf_limits[class].soft_limit_seconds = soft_seconds;
        } else if ((!strcasecmp(argv[0],"min-slaves-to-write") ||
                    !strcasecmp(argv[0],"min-replicas-to-write")) && argc == 2)
        {
            server.repl_min_slaves_to_write = atoi(argv[1]);
            if (server.repl_min_slaves_to_write < 0) {
                err = "Invalid value for min-replicas-to-write."; goto loaderr;
            }
        } else if ((!strcasecmp(argv[0],"min-slaves-max-lag") ||
                    !strcasecmp(argv[0],"min-replicas-max-lag")) && argc == 2)
        {
            server.repl_min_slaves_max_lag = atoi(argv[1]);
            if (server.repl_min_slaves_max_lag < 0) {
                err = "Invalid value for min-replicas-max-lag."; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"notify-keyspace-events") && argc == 2) {
            int flags = keyspaceEventsStringToFlags(argv[1]);

            if (flags == -1) {
                err = "Invalid event class character. Use 'g$lshzxeA'.";
                goto loaderr;
            }
            server.notify_keyspace_events = flags;
        } else if (!strcasecmp(argv[0],"user") && argc >= 2) {
            int argc_err;
            if (ACLAppendUserForLoading(argv,argc,&argc_err) == C_ERR) {
                char buf[1024];
                char *errmsg = ACLSetUserStringError();
                snprintf(buf,sizeof(buf),"Error in user declaration '%s': %s",
                    argv[argc_err],errmsg);
                err = buf;
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"loadmodule") && argc >= 2) {
            queueLoadModule(argv[1],&argv[2],argc-2);
        } else if (!strcasecmp(argv[0],"sentinel")) {
            /* argc == 1 is handled by main() as we need to enter the sentinel
             * mode ASAP. */
            if (argc != 1) {
                if (!server.sentinel_mode) {
                    err = "sentinel directive while not in sentinel mode";
                    goto loaderr;
                }
                err = sentinelHandleConfiguration(argv+1,argc-1);
                if (err) goto loaderr;
            }
#ifdef USE_OPENSSL
        } else if (!strcasecmp(argv[0],"tls-port") && argc == 2) {
            server.tls_port = atoi(argv[1]);
            if (server.port < 0 || server.port > 65535) {
                err = "Invalid tls-port"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"tls-cluster") && argc == 2) {
            server.tls_cluster = yesnotoi(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-replication") && argc == 2) {
            server.tls_replication = yesnotoi(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-auth-clients") && argc == 2) {
            server.tls_auth_clients = yesnotoi(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-cert-file") && argc == 2) {
            zfree(server.tls_ctx_config.cert_file);
            server.tls_ctx_config.cert_file = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-key-file") && argc == 2) {
            zfree(server.tls_ctx_config.key_file);
            server.tls_ctx_config.key_file = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-dh-params-file") && argc == 2) {
            zfree(server.tls_ctx_config.dh_params_file);
            server.tls_ctx_config.dh_params_file = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-ca-cert-file") && argc == 2) {
            zfree(server.tls_ctx_config.ca_cert_file);
            server.tls_ctx_config.ca_cert_file = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-ca-cert-dir") && argc == 2) {
            zfree(server.tls_ctx_config.ca_cert_dir);
            server.tls_ctx_config.ca_cert_dir = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-protocols") && argc >= 2) {
            zfree(server.tls_ctx_config.protocols);
            server.tls_ctx_config.protocols = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-ciphers") && argc == 2) {
            zfree(server.tls_ctx_config.ciphers);
            server.tls_ctx_config.ciphers = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-ciphersuites") && argc == 2) {
            zfree(server.tls_ctx_config.ciphersuites);
            server.tls_ctx_config.ciphersuites = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"tls-prefer-server-ciphers") && argc == 2) {
            server.tls_ctx_config.prefer_server_ciphers = yesnotoi(argv[1]);
#endif  /* USE_OPENSSL */
        } else {
            err = "Bad directive or wrong number of arguments"; goto loaderr;
        }
        sdsfreesplitres(argv,argc);
    }

    /* Sanity checks. */
    if (server.cluster_enabled && server.masterhost) {
        linenum = slaveof_linenum;
        i = linenum-1;
        err = "replicaof directive not allowed in cluster mode";
        goto loaderr;
    }

    sdsfreesplitres(lines,totlines);
    return;

loaderr:
    fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
    fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
    fprintf(stderr, ">>> '%s'\n", lines[i]);
    fprintf(stderr, "%s\n", err);
    exit(1);
}

/* Load the server configuration from the specified filename.
 * The function appends the additional configuration directives stored
 * in the 'options' string to the config file before loading.
 *
 * Both filename and options can be NULL, in such a case are considered
 * empty. This way loadServerConfig can be used to just load a file or
 * just load a string. */
void loadServerConfig(char *filename, char *options) {
    sds config = sdsempty();
    char buf[CONFIG_MAX_LINE+1];

    /* Load the file content */
    if (filename) {
        FILE *fp;

        if (filename[0] == '-' && filename[1] == '\0') {
            fp = stdin;
        } else {
            if ((fp = fopen(filename,"r")) == NULL) {
                serverLog(LL_WARNING,
                    "Fatal error, can't open config file '%s'", filename);
                exit(1);
            }
        }
        while(fgets(buf,CONFIG_MAX_LINE+1,fp) != NULL)
            config = sdscat(config,buf);
        if (fp != stdin) fclose(fp);
    }
    /* Append the additional options */
    if (options) {
        config = sdscat(config,"\n");
        config = sdscat(config,options);
    }
    loadServerConfigFromString(config);
    sdsfree(config);
}

/*-----------------------------------------------------------------------------
 * CONFIG SET implementation
 *----------------------------------------------------------------------------*/

#define config_set_bool_field(_name,_var) \
    } else if (!strcasecmp(c->argv[2]->ptr,_name)) { \
        int yn = yesnotoi(o->ptr); \
        if (yn == -1) goto badfmt; \
        _var = yn;

#define config_set_numerical_field(_name,_var,min,max) \
    } else if (!strcasecmp(c->argv[2]->ptr,_name)) { \
        if (getLongLongFromObject(o,&ll) == C_ERR) goto badfmt; \
        if (min != LLONG_MIN && ll < min) goto badfmt; \
        if (max != LLONG_MAX && ll > max) goto badfmt; \
        _var = ll;

#define config_set_memory_field(_name,_var) \
    } else if (!strcasecmp(c->argv[2]->ptr,_name)) { \
        ll = memtoll(o->ptr,&err); \
        if (err || ll < 0) goto badfmt; \
        _var = ll;

#define config_set_special_field(_name) \
    } else if (!strcasecmp(c->argv[2]->ptr,_name)) {

#define config_set_special_field_with_alias(_name,_name2) \
    } else if (!strcasecmp(c->argv[2]->ptr,_name) || \
               !strcasecmp(c->argv[2]->ptr,_name2)) {

#define config_set_else } else

void configSetCommand(client *c) {
    robj *o;
    long long ll;
    int err;
    serverAssertWithInfo(c,c->argv[2],sdsEncodedObject(c->argv[2]));
    serverAssertWithInfo(c,c->argv[3],sdsEncodedObject(c->argv[3]));
    o = c->argv[3];

    /* Iterate the configs that are standard */
    for (standardConfig *config = configs; config->name != NULL; config++) {
        if(config->modifiable && (!strcasecmp(c->argv[2]->ptr,config->name) ||
            (config->alias && !strcasecmp(c->argv[2]->ptr,config->alias))))  
        {
            if (!config->interface.set(config->data,o->ptr)) {
                goto badfmt;
            }
            addReply(c,shared.ok);
            return;
        }
    }

    if (0) { /* this starts the config_set macros else-if chain. */

    /* Special fields that can't be handled with general macros. */
    config_set_special_field("dbfilename") {
        if (!pathIsBaseName(o->ptr)) {
            addReplyError(c, "dbfilename can't be a path, just a filename");
            return;
        }
        zfree(server.rdb_filename);
        server.rdb_filename = zstrdup(o->ptr);
    } config_set_special_field("requirepass") {
        if (sdslen(o->ptr) > CONFIG_AUTHPASS_MAX_LEN) goto badfmt;
        /* The old "requirepass" directive just translates to setting
         * a password to the default user. */
        ACLSetUser(DefaultUser,"resetpass",-1);
        sds aclop = sdscatprintf(sdsempty(),">%s",(char*)o->ptr);
        ACLSetUser(DefaultUser,aclop,sdslen(aclop));
        sdsfree(aclop);
    } config_set_special_field("maxclients") {
        int orig_value = server.maxclients;

        if (getLongLongFromObject(o,&ll) == C_ERR || ll < 1) goto badfmt;

        /* Try to check if the OS is capable of supporting so many FDs. */
        server.maxclients = ll;
        if (ll > orig_value) {
            adjustOpenFilesLimit();
            if (server.maxclients != ll) {
                addReplyErrorFormat(c,"The operating system is not able to handle the specified number of clients, try with %d", server.maxclients);
                server.maxclients = orig_value;
                return;
            }
            if ((unsigned int) aeGetSetSize(server.el) <
                server.maxclients + CONFIG_FDSET_INCR)
            {
                if (aeResizeSetSize(server.el,
                    server.maxclients + CONFIG_FDSET_INCR) == AE_ERR)
                {
                    addReplyError(c,"The event loop API used by Redis is not able to handle the specified number of clients");
                    server.maxclients = orig_value;
                    return;
                }
            }
        }
    } config_set_special_field("appendonly") {
        int enable = yesnotoi(o->ptr);

        if (enable == -1) goto badfmt;
        server.aof_enabled = enable;
        if (enable == 0 && server.aof_state != AOF_OFF) {
            stopAppendOnly();
        } else if (enable && server.aof_state == AOF_OFF) {
            if (startAppendOnly() == C_ERR) {
                addReplyError(c,
                    "Unable to turn on AOF. Check server logs.");
                return;
            }
        }
    } config_set_special_field("save") {
        int vlen, j;
        sds *v = sdssplitlen(o->ptr,sdslen(o->ptr)," ",1,&vlen);

        /* Perform sanity check before setting the new config:
         * - Even number of args
         * - Seconds >= 1, changes >= 0 */
        if (vlen & 1) {
            sdsfreesplitres(v,vlen);
            goto badfmt;
        }
        for (j = 0; j < vlen; j++) {
            char *eptr;
            long val;

            val = strtoll(v[j], &eptr, 10);
            if (eptr[0] != '\0' ||
                ((j & 1) == 0 && val < 1) ||
                ((j & 1) == 1 && val < 0)) {
                sdsfreesplitres(v,vlen);
                goto badfmt;
            }
        }
        /* Finally set the new config */
        resetServerSaveParams();
        for (j = 0; j < vlen; j += 2) {
            time_t seconds;
            int changes;

            seconds = strtoll(v[j],NULL,10);
            changes = strtoll(v[j+1],NULL,10);
            appendServerSaveParams(seconds, changes);
        }
        sdsfreesplitres(v,vlen);
    } config_set_special_field("dir") {
        if (chdir((char*)o->ptr) == -1) {
            addReplyErrorFormat(c,"Changing directory: %s", strerror(errno));
            return;
        }
    } config_set_special_field("client-output-buffer-limit") {
        int vlen, j;
        sds *v = sdssplitlen(o->ptr,sdslen(o->ptr)," ",1,&vlen);

        /* We need a multiple of 4: <class> <hard> <soft> <soft_seconds> */
        if (vlen % 4) {
            sdsfreesplitres(v,vlen);
            goto badfmt;
        }

        /* Sanity check of single arguments, so that we either refuse the
         * whole configuration string or accept it all, even if a single
         * error in a single client class is present. */
        for (j = 0; j < vlen; j++) {
            long val;

            if ((j % 4) == 0) {
                int class = getClientTypeByName(v[j]);
                if (class == -1 || class == CLIENT_TYPE_MASTER) {
                    sdsfreesplitres(v,vlen);
                    goto badfmt;
                }
            } else {
                val = memtoll(v[j], &err);
                if (err || val < 0) {
                    sdsfreesplitres(v,vlen);
                    goto badfmt;
                }
            }
        }
        /* Finally set the new config */
        for (j = 0; j < vlen; j += 4) {
            int class;
            unsigned long long hard, soft;
            int soft_seconds;

            class = getClientTypeByName(v[j]);
            hard = memtoll(v[j+1],NULL);
            soft = memtoll(v[j+2],NULL);
            soft_seconds = strtoll(v[j+3],NULL,10);

            server.client_obuf_limits[class].hard_limit_bytes = hard;
            server.client_obuf_limits[class].soft_limit_bytes = soft;
            server.client_obuf_limits[class].soft_limit_seconds = soft_seconds;
        }
        sdsfreesplitres(v,vlen);
    } config_set_special_field("notify-keyspace-events") {
        int flags = keyspaceEventsStringToFlags(o->ptr);

        if (flags == -1) goto badfmt;
        server.notify_keyspace_events = flags;
    /* Boolean fields.
     * config_set_bool_field(name,var). */
    } config_set_bool_field(
      "activedefrag",server.active_defrag_enabled) {
#ifndef HAVE_DEFRAG
        if (server.active_defrag_enabled) {
            server.active_defrag_enabled = 0;
            addReplyError(c,
                "-DISABLED Active defragmentation cannot be enabled: it "
                "requires a Redis server compiled with a modified Jemalloc "
                "like the one shipped by default with the Redis source "
                "distribution");
            return;
        }
#endif
    /* Numerical fields.
     * config_set_numerical_field(name,var,min,max) */
    } config_set_numerical_field(
      "repl-backlog-ttl",server.repl_backlog_time_limit,0,LONG_MAX) {
    } config_set_numerical_field(
      "min-slaves-to-write",server.repl_min_slaves_to_write,0,INT_MAX) {
        refreshGoodSlavesCount();
    } config_set_numerical_field(
      "min-replicas-to-write",server.repl_min_slaves_to_write,0,INT_MAX) {
        refreshGoodSlavesCount();
    } config_set_numerical_field(
      "min-slaves-max-lag",server.repl_min_slaves_max_lag,0,INT_MAX) {
        refreshGoodSlavesCount();
    } config_set_numerical_field(
      "min-replicas-max-lag",server.repl_min_slaves_max_lag,0,INT_MAX) {
        refreshGoodSlavesCount();
    } config_set_numerical_field(
      "hz",server.config_hz,0,INT_MAX) {
        /* Hz is more an hint from the user, so we accept values out of range
         * but cap them to reasonable values. */
        if (server.config_hz < CONFIG_MIN_HZ) server.config_hz = CONFIG_MIN_HZ;
        if (server.config_hz > CONFIG_MAX_HZ) server.config_hz = CONFIG_MAX_HZ;
    } config_set_numerical_field(
      "watchdog-period",ll,0,INT_MAX) {
        if (ll)
            enableWatchdog(ll);
        else
            disableWatchdog();
    /* Memory fields.
     * config_set_memory_field(name,var) */
    } config_set_memory_field("maxmemory",server.maxmemory) {
        if (server.maxmemory) {
            if (server.maxmemory < zmalloc_used_memory()) {
                serverLog(LL_WARNING,"WARNING: the new maxmemory value set via CONFIG SET is smaller than the current memory usage. This will result in key eviction and/or the inability to accept new write commands depending on the maxmemory-policy.");
            }
            freeMemoryIfNeededAndSafe();
        }
     } config_set_memory_field(
       "client-query-buffer-limit",server.client_max_querybuf_len) {
    } config_set_memory_field("repl-backlog-size",ll) {
        resizeReplicationBacklog(ll);
#ifdef USE_OPENSSL
    /* TLS fields. */
    } config_set_special_field("tls-cert-file") {
        redisTLSContextConfig tmpctx = server.tls_ctx_config;
        tmpctx.cert_file = (char *) o->ptr;
        if (tlsConfigure(&tmpctx) == C_ERR) {
            addReplyError(c,
                    "Unable to configure tls-cert-file. Check server logs.");
            return;
        }
        zfree(server.tls_ctx_config.cert_file);
        server.tls_ctx_config.cert_file = zstrdup(o->ptr);
    } config_set_special_field("tls-key-file") {
        redisTLSContextConfig tmpctx = server.tls_ctx_config;
        tmpctx.key_file = (char *) o->ptr;
        if (tlsConfigure(&tmpctx) == C_ERR) {
            addReplyError(c,
                    "Unable to configure tls-key-file. Check server logs.");
            return;
        }
        zfree(server.tls_ctx_config.key_file);
        server.tls_ctx_config.key_file = zstrdup(o->ptr);
    } config_set_special_field("tls-dh-params-file") {
        redisTLSContextConfig tmpctx = server.tls_ctx_config;
        tmpctx.dh_params_file = (char *) o->ptr;
        if (tlsConfigure(&tmpctx) == C_ERR) {
            addReplyError(c,
                    "Unable to configure tls-dh-params-file. Check server logs.");
            return;
        }
        zfree(server.tls_ctx_config.dh_params_file);
        server.tls_ctx_config.dh_params_file = zstrdup(o->ptr);
    } config_set_special_field("tls-ca-cert-file") {
        redisTLSContextConfig tmpctx = server.tls_ctx_config;
        tmpctx.ca_cert_file = (char *) o->ptr;
        if (tlsConfigure(&tmpctx) == C_ERR) {
            addReplyError(c,
                    "Unable to configure tls-ca-cert-file. Check server logs.");
            return;
        }
        zfree(server.tls_ctx_config.ca_cert_file);
        server.tls_ctx_config.ca_cert_file = zstrdup(o->ptr);
    } config_set_special_field("tls-ca-cert-dir") {
        redisTLSContextConfig tmpctx = server.tls_ctx_config;
        tmpctx.ca_cert_dir = (char *) o->ptr;
        if (tlsConfigure(&tmpctx) == C_ERR) {
            addReplyError(c,
                    "Unable to configure tls-ca-cert-dir. Check server logs.");
            return;
        }
        zfree(server.tls_ctx_config.ca_cert_dir);
        server.tls_ctx_config.ca_cert_dir = zstrdup(o->ptr);
    } config_set_bool_field("tls-auth-clients", server.tls_auth_clients) {
    } config_set_bool_field("tls-replication", server.tls_replication) {
    } config_set_bool_field("tls-cluster", server.tls_cluster) {
    } config_set_special_field("tls-protocols") {
        redisTLSContextConfig tmpctx = server.tls_ctx_config;
        tmpctx.protocols = (char *) o->ptr;
        if (tlsConfigure(&tmpctx) == C_ERR) {
            addReplyError(c,
                    "Unable to configure tls-protocols. Check server logs.");
            return;
        }
        zfree(server.tls_ctx_config.protocols);
        server.tls_ctx_config.protocols = zstrdup(o->ptr);
    } config_set_special_field("tls-ciphers") {
        redisTLSContextConfig tmpctx = server.tls_ctx_config;
        tmpctx.ciphers = (char *) o->ptr;
        if (tlsConfigure(&tmpctx) == C_ERR) {
            addReplyError(c,
                "Unable to configure tls-ciphers. Check server logs.");
            return;
        }
        zfree(server.tls_ctx_config.ciphers);
        server.tls_ctx_config.ciphers = zstrdup(o->ptr);
    } config_set_special_field("tls-ciphersuites") {
        redisTLSContextConfig tmpctx = server.tls_ctx_config;
        tmpctx.ciphersuites = (char *) o->ptr;
        if (tlsConfigure(&tmpctx) == C_ERR) {
            addReplyError(c,
                "Unable to configure tls-ciphersuites. Check server logs.");
            return;
        }
        zfree(server.tls_ctx_config.ciphersuites);
        server.tls_ctx_config.ciphersuites = zstrdup(o->ptr);
    } config_set_special_field("tls-prefer-server-ciphers") {
        redisTLSContextConfig tmpctx = server.tls_ctx_config;
        tmpctx.prefer_server_ciphers = yesnotoi(o->ptr);
        if (tlsConfigure(&tmpctx) == C_ERR) {
            addReplyError(c, "Unable to reconfigure TLS. Check server logs.");
            return;
        }
        server.tls_ctx_config.prefer_server_ciphers = tmpctx.prefer_server_ciphers;
#endif  /* USE_OPENSSL */
    /* Everyhing else is an error... */
    } config_set_else {
        addReplyErrorFormat(c,"Unsupported CONFIG parameter: %s",
            (char*)c->argv[2]->ptr);
        return;
    }

    /* On success we just return a generic OK for all the options. */
    addReply(c,shared.ok);
    return;

badfmt: /* Bad format errors */
    addReplyErrorFormat(c,"Invalid argument '%s' for CONFIG SET '%s'",
            (char*)o->ptr,
            (char*)c->argv[2]->ptr);
}

/*-----------------------------------------------------------------------------
 * CONFIG GET implementation
 *----------------------------------------------------------------------------*/

#define config_get_string_field(_name,_var) do { \
    if (stringmatch(pattern,_name,1)) { \
        addReplyBulkCString(c,_name); \
        addReplyBulkCString(c,_var ? _var : ""); \
        matches++; \
    } \
} while(0);

#define config_get_bool_field(_name,_var) do { \
    if (stringmatch(pattern,_name,1)) { \
        addReplyBulkCString(c,_name); \
        addReplyBulkCString(c,_var ? "yes" : "no"); \
        matches++; \
    } \
} while(0);

#define config_get_numerical_field(_name,_var) do { \
    if (stringmatch(pattern,_name,1)) { \
        ll2string(buf,sizeof(buf),_var); \
        addReplyBulkCString(c,_name); \
        addReplyBulkCString(c,buf); \
        matches++; \
    } \
} while(0);


void configGetCommand(client *c) {
    robj *o = c->argv[2];
    void *replylen = addReplyDeferredLen(c);
    char *pattern = o->ptr;
    char buf[128];
    int matches = 0;
    serverAssertWithInfo(c,o,sdsEncodedObject(o));

    /* Iterate the configs that are standard */
    for (standardConfig *config = configs; config->name != NULL; config++) {
        if (stringmatch(pattern,config->name,1)) {
            addReplyBulkCString(c,config->name);
            config->interface.get(c,config->data);
            matches++;
        }
        if (config->alias && stringmatch(pattern,config->alias,1)) {
            addReplyBulkCString(c,config->alias);
            config->interface.get(c,config->data);
            matches++;
        }
    }

    /* String values */
    config_get_string_field("dbfilename",server.rdb_filename);
    config_get_string_field("logfile",server.logfile);
#ifdef USE_OPENSSL
    config_get_string_field("tls-cert-file",server.tls_ctx_config.cert_file);
    config_get_string_field("tls-key-file",server.tls_ctx_config.key_file);
    config_get_string_field("tls-dh-params-file",server.tls_ctx_config.dh_params_file);
    config_get_string_field("tls-ca-cert-file",server.tls_ctx_config.ca_cert_file);
    config_get_string_field("tls-ca-cert-dir",server.tls_ctx_config.ca_cert_dir);
    config_get_string_field("tls-protocols",server.tls_ctx_config.protocols);
    config_get_string_field("tls-ciphers",server.tls_ctx_config.ciphers);
    config_get_string_field("tls-ciphersuites",server.tls_ctx_config.ciphersuites);
#endif

    /* Numerical values */
    config_get_numerical_field("maxmemory",server.maxmemory);
    config_get_numerical_field("client-query-buffer-limit",server.client_max_querybuf_len);
    config_get_numerical_field("tls-port",server.tls_port);
    config_get_numerical_field("repl-backlog-size",server.repl_backlog_size);
    config_get_numerical_field("repl-backlog-ttl",server.repl_backlog_time_limit);
    config_get_numerical_field("maxclients",server.maxclients);
    config_get_numerical_field("watchdog-period",server.watchdog_period);
    config_get_numerical_field("min-slaves-to-write",server.repl_min_slaves_to_write);
    config_get_numerical_field("min-replicas-to-write",server.repl_min_slaves_to_write);
    config_get_numerical_field("min-slaves-max-lag",server.repl_min_slaves_max_lag);
    config_get_numerical_field("min-replicas-max-lag",server.repl_min_slaves_max_lag);
    config_get_numerical_field("hz",server.config_hz);

    /* Bool (yes/no) values */
    config_get_bool_field("activedefrag", server.active_defrag_enabled);
    config_get_bool_field("tls-cluster",server.tls_cluster);
    config_get_bool_field("tls-replication",server.tls_replication);
    config_get_bool_field("tls-auth-clients",server.tls_auth_clients);
    config_get_bool_field("tls-prefer-server-ciphers",
            server.tls_ctx_config.prefer_server_ciphers);

    /* Everything we can't handle with macros follows. */

    if (stringmatch(pattern,"appendonly",1)) {
        addReplyBulkCString(c,"appendonly");
        addReplyBulkCString(c,server.aof_enabled ? "yes" : "no");
        matches++;
    }
    if (stringmatch(pattern,"dir",1)) {
        char buf[1024];

        if (getcwd(buf,sizeof(buf)) == NULL)
            buf[0] = '\0';

        addReplyBulkCString(c,"dir");
        addReplyBulkCString(c,buf);
        matches++;
    }
    if (stringmatch(pattern,"save",1)) {
        sds buf = sdsempty();
        int j;

        for (j = 0; j < server.saveparamslen; j++) {
            buf = sdscatprintf(buf,"%jd %d",
                    (intmax_t)server.saveparams[j].seconds,
                    server.saveparams[j].changes);
            if (j != server.saveparamslen-1)
                buf = sdscatlen(buf," ",1);
        }
        addReplyBulkCString(c,"save");
        addReplyBulkCString(c,buf);
        sdsfree(buf);
        matches++;
    }
    if (stringmatch(pattern,"client-output-buffer-limit",1)) {
        sds buf = sdsempty();
        int j;

        for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++) {
            buf = sdscatprintf(buf,"%s %llu %llu %ld",
                    getClientTypeName(j),
                    server.client_obuf_limits[j].hard_limit_bytes,
                    server.client_obuf_limits[j].soft_limit_bytes,
                    (long) server.client_obuf_limits[j].soft_limit_seconds);
            if (j != CLIENT_TYPE_OBUF_COUNT-1)
                buf = sdscatlen(buf," ",1);
        }
        addReplyBulkCString(c,"client-output-buffer-limit");
        addReplyBulkCString(c,buf);
        sdsfree(buf);
        matches++;
    }
    if (stringmatch(pattern,"unixsocketperm",1)) {
        char buf[32];
        snprintf(buf,sizeof(buf),"%o",server.unixsocketperm);
        addReplyBulkCString(c,"unixsocketperm");
        addReplyBulkCString(c,buf);
        matches++;
    }
    if (stringmatch(pattern,"slaveof",1) ||
        stringmatch(pattern,"replicaof",1))
    {
        char *optname = stringmatch(pattern,"slaveof",1) ?
                        "slaveof" : "replicaof";
        char buf[256];

        addReplyBulkCString(c,optname);
        if (server.masterhost)
            snprintf(buf,sizeof(buf),"%s %d",
                server.masterhost, server.masterport);
        else
            buf[0] = '\0';
        addReplyBulkCString(c,buf);
        matches++;
    }
    if (stringmatch(pattern,"notify-keyspace-events",1)) {
        sds flags = keyspaceEventsFlagsToString(server.notify_keyspace_events);

        addReplyBulkCString(c,"notify-keyspace-events");
        addReplyBulkSds(c,flags);
        matches++;
    }
    if (stringmatch(pattern,"bind",1)) {
        sds aux = sdsjoin(server.bindaddr,server.bindaddr_count," ");

        addReplyBulkCString(c,"bind");
        addReplyBulkCString(c,aux);
        sdsfree(aux);
        matches++;
    }
    if (stringmatch(pattern,"requirepass",1)) {
        addReplyBulkCString(c,"requirepass");
        sds password = ACLDefaultUserFirstPassword();
        if (password) {
            addReplyBulkCBuffer(c,password,sdslen(password));
        } else {
            addReplyBulkCString(c,"");
        }
        matches++;
    }

    setDeferredMapLen(c,replylen,matches);
}

/*-----------------------------------------------------------------------------
 * CONFIG REWRITE implementation
 *----------------------------------------------------------------------------*/

#define REDIS_CONFIG_REWRITE_SIGNATURE "# Generated by CONFIG REWRITE"

/* We use the following dictionary type to store where a configuration
 * option is mentioned in the old configuration file, so it's
 * like "maxmemory" -> list of line numbers (first line is zero). */
uint64_t dictSdsCaseHash(const void *key);
int dictSdsKeyCaseCompare(void *privdata, const void *key1, const void *key2);
void dictSdsDestructor(void *privdata, void *val);
void dictListDestructor(void *privdata, void *val);

/* Sentinel config rewriting is implemented inside sentinel.c by
 * rewriteConfigSentinelOption(). */
void rewriteConfigSentinelOption(struct rewriteConfigState *state);

dictType optionToLineDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictListDestructor          /* val destructor */
};

dictType optionSetDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL                        /* val destructor */
};

/* The config rewrite state. */
struct rewriteConfigState {
    dict *option_to_line; /* Option -> list of config file lines map */
    dict *rewritten;      /* Dictionary of already processed options */
    int numlines;         /* Number of lines in current config */
    sds *lines;           /* Current lines as an array of sds strings */
    int has_tail;         /* True if we already added directives that were
                             not present in the original config file. */
};

/* Append the new line to the current configuration state. */
void rewriteConfigAppendLine(struct rewriteConfigState *state, sds line) {
    state->lines = zrealloc(state->lines, sizeof(char*) * (state->numlines+1));
    state->lines[state->numlines++] = line;
}

/* Populate the option -> list of line numbers map. */
void rewriteConfigAddLineNumberToOption(struct rewriteConfigState *state, sds option, int linenum) {
    list *l = dictFetchValue(state->option_to_line,option);

    if (l == NULL) {
        l = listCreate();
        dictAdd(state->option_to_line,sdsdup(option),l);
    }
    listAddNodeTail(l,(void*)(long)linenum);
}

/* Add the specified option to the set of processed options.
 * This is useful as only unused lines of processed options will be blanked
 * in the config file, while options the rewrite process does not understand
 * remain untouched. */
void rewriteConfigMarkAsProcessed(struct rewriteConfigState *state, const char *option) {
    sds opt = sdsnew(option);

    if (dictAdd(state->rewritten,opt,NULL) != DICT_OK) sdsfree(opt);
}

/* Read the old file, split it into lines to populate a newly created
 * config rewrite state, and return it to the caller.
 *
 * If it is impossible to read the old file, NULL is returned.
 * If the old file does not exist at all, an empty state is returned. */
struct rewriteConfigState *rewriteConfigReadOldFile(char *path) {
    FILE *fp = fopen(path,"r");
    if (fp == NULL && errno != ENOENT) return NULL;

    char buf[CONFIG_MAX_LINE+1];
    int linenum = -1;
    struct rewriteConfigState *state = zmalloc(sizeof(*state));
    state->option_to_line = dictCreate(&optionToLineDictType,NULL);
    state->rewritten = dictCreate(&optionSetDictType,NULL);
    state->numlines = 0;
    state->lines = NULL;
    state->has_tail = 0;
    if (fp == NULL) return state;

    /* Read the old file line by line, populate the state. */
    while(fgets(buf,CONFIG_MAX_LINE+1,fp) != NULL) {
        int argc;
        sds *argv;
        sds line = sdstrim(sdsnew(buf),"\r\n\t ");

        linenum++; /* Zero based, so we init at -1 */

        /* Handle comments and empty lines. */
        if (line[0] == '#' || line[0] == '\0') {
            if (!state->has_tail && !strcmp(line,REDIS_CONFIG_REWRITE_SIGNATURE))
                state->has_tail = 1;
            rewriteConfigAppendLine(state,line);
            continue;
        }

        /* Not a comment, split into arguments. */
        argv = sdssplitargs(line,&argc);
        if (argv == NULL) {
            /* Apparently the line is unparsable for some reason, for
             * instance it may have unbalanced quotes. Load it as a
             * comment. */
            sds aux = sdsnew("# ??? ");
            aux = sdscatsds(aux,line);
            sdsfree(line);
            rewriteConfigAppendLine(state,aux);
            continue;
        }

        sdstolower(argv[0]); /* We only want lowercase config directives. */

        /* Now we populate the state according to the content of this line.
         * Append the line and populate the option -> line numbers map. */
        rewriteConfigAppendLine(state,line);

        /* Translate options using the word "slave" to the corresponding name
         * "replica", before adding such option to the config name -> lines
         * mapping. */
        char *p = strstr(argv[0],"slave");
        if (p) {
            sds alt = sdsempty();
            alt = sdscatlen(alt,argv[0],p-argv[0]);;
            alt = sdscatlen(alt,"replica",7);
            alt = sdscatlen(alt,p+5,strlen(p+5));
            sdsfree(argv[0]);
            argv[0] = alt;
        }
        rewriteConfigAddLineNumberToOption(state,argv[0],linenum);
        sdsfreesplitres(argv,argc);
    }
    fclose(fp);
    return state;
}

/* Rewrite the specified configuration option with the new "line".
 * It progressively uses lines of the file that were already used for the same
 * configuration option in the old version of the file, removing that line from
 * the map of options -> line numbers.
 *
 * If there are lines associated with a given configuration option and
 * "force" is non-zero, the line is appended to the configuration file.
 * Usually "force" is true when an option has not its default value, so it
 * must be rewritten even if not present previously.
 *
 * The first time a line is appended into a configuration file, a comment
 * is added to show that starting from that point the config file was generated
 * by CONFIG REWRITE.
 *
 * "line" is either used, or freed, so the caller does not need to free it
 * in any way. */
void rewriteConfigRewriteLine(struct rewriteConfigState *state, const char *option, sds line, int force) {
    sds o = sdsnew(option);
    list *l = dictFetchValue(state->option_to_line,o);

    rewriteConfigMarkAsProcessed(state,option);

    if (!l && !force) {
        /* Option not used previously, and we are not forced to use it. */
        sdsfree(line);
        sdsfree(o);
        return;
    }

    if (l) {
        listNode *ln = listFirst(l);
        int linenum = (long) ln->value;

        /* There are still lines in the old configuration file we can reuse
         * for this option. Replace the line with the new one. */
        listDelNode(l,ln);
        if (listLength(l) == 0) dictDelete(state->option_to_line,o);
        sdsfree(state->lines[linenum]);
        state->lines[linenum] = line;
    } else {
        /* Append a new line. */
        if (!state->has_tail) {
            rewriteConfigAppendLine(state,
                sdsnew(REDIS_CONFIG_REWRITE_SIGNATURE));
            state->has_tail = 1;
        }
        rewriteConfigAppendLine(state,line);
    }
    sdsfree(o);
}

/* Write the long long 'bytes' value as a string in a way that is parsable
 * inside redis.conf. If possible uses the GB, MB, KB notation. */
int rewriteConfigFormatMemory(char *buf, size_t len, long long bytes) {
    int gb = 1024*1024*1024;
    int mb = 1024*1024;
    int kb = 1024;

    if (bytes && (bytes % gb) == 0) {
        return snprintf(buf,len,"%lldgb",bytes/gb);
    } else if (bytes && (bytes % mb) == 0) {
        return snprintf(buf,len,"%lldmb",bytes/mb);
    } else if (bytes && (bytes % kb) == 0) {
        return snprintf(buf,len,"%lldkb",bytes/kb);
    } else {
        return snprintf(buf,len,"%lld",bytes);
    }
}

/* Rewrite a simple "option-name <bytes>" configuration option. */
void rewriteConfigBytesOption(struct rewriteConfigState *state, const char *option, long long value, long long defvalue) {
    char buf[64];
    int force = value != defvalue;
    sds line;

    rewriteConfigFormatMemory(buf,sizeof(buf),value);
    line = sdscatprintf(sdsempty(),"%s %s",option,buf);
    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite a yes/no option. */
void rewriteConfigYesNoOption(struct rewriteConfigState *state, const char *option, int value, int defvalue) {
    int force = value != defvalue;
    sds line = sdscatprintf(sdsempty(),"%s %s",option,
        value ? "yes" : "no");

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite a string option. */
void rewriteConfigStringOption(struct rewriteConfigState *state, const char *option, char *value, const char *defvalue) {
    int force = 1;
    sds line;

    /* String options set to NULL need to be not present at all in the
     * configuration file to be set to NULL again at the next reboot. */
    if (value == NULL) {
        rewriteConfigMarkAsProcessed(state,option);
        return;
    }

    /* Set force to zero if the value is set to its default. */
    if (defvalue && strcmp(value,defvalue) == 0) force = 0;

    line = sdsnew(option);
    line = sdscatlen(line, " ", 1);
    line = sdscatrepr(line, value, strlen(value));

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite a numerical (long long range) option. */
void rewriteConfigNumericalOption(struct rewriteConfigState *state, const char *option, long long value, long long defvalue) {
    int force = value != defvalue;
    sds line = sdscatprintf(sdsempty(),"%s %lld",option,value);

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite a octal option. */
void rewriteConfigOctalOption(struct rewriteConfigState *state, char *option, int value, int defvalue) {
    int force = value != defvalue;
    sds line = sdscatprintf(sdsempty(),"%s %o",option,value);

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite an enumeration option. It takes as usually state and option name,
 * and in addition the enumeration array and the default value for the
 * option. */
void rewriteConfigEnumOption(struct rewriteConfigState *state, const char *option, int value, configEnum *ce, int defval) {
    sds line;
    const char *name = configEnumGetNameOrUnknown(ce,value);
    int force = value != defval;

    line = sdscatprintf(sdsempty(),"%s %s",option,name);
    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite the save option. */
void rewriteConfigSaveOption(struct rewriteConfigState *state) {
    int j;
    sds line;

    /* Note that if there are no save parameters at all, all the current
     * config line with "save" will be detected as orphaned and deleted,
     * resulting into no RDB persistence as expected. */
    for (j = 0; j < server.saveparamslen; j++) {
        line = sdscatprintf(sdsempty(),"save %ld %d",
            (long) server.saveparams[j].seconds, server.saveparams[j].changes);
        rewriteConfigRewriteLine(state,"save",line,1);
    }
    /* Mark "save" as processed in case server.saveparamslen is zero. */
    rewriteConfigMarkAsProcessed(state,"save");
}

/* Rewrite the user option. */
void rewriteConfigUserOption(struct rewriteConfigState *state) {
    /* If there is a user file defined we just mark this configuration
     * directive as processed, so that all the lines containing users
     * inside the config file gets discarded. */
    if (server.acl_filename[0] != '\0') {
        rewriteConfigMarkAsProcessed(state,"user");
        return;
    }

    /* Otherwise scan the list of users and rewrite every line. Note that
     * in case the list here is empty, the effect will just be to comment
     * all the users directive inside the config file. */
    raxIterator ri;
    raxStart(&ri,Users);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        user *u = ri.data;
        sds line = sdsnew("user ");
        line = sdscatsds(line,u->name);
        line = sdscatlen(line," ",1);
        sds descr = ACLDescribeUser(u);
        line = sdscatsds(line,descr);
        sdsfree(descr);
        rewriteConfigRewriteLine(state,"user",line,1);
    }
    raxStop(&ri);

    /* Mark "user" as processed in case there are no defined users. */
    rewriteConfigMarkAsProcessed(state,"user");
}

/* Rewrite the dir option, always using absolute paths.*/
void rewriteConfigDirOption(struct rewriteConfigState *state) {
    char cwd[1024];

    if (getcwd(cwd,sizeof(cwd)) == NULL) {
        rewriteConfigMarkAsProcessed(state,"dir");
        return; /* no rewrite on error. */
    }
    rewriteConfigStringOption(state,"dir",cwd,NULL);
}

/* Rewrite the slaveof option. */
void rewriteConfigSlaveofOption(struct rewriteConfigState *state, char *option) {
    sds line;

    /* If this is a master, we want all the slaveof config options
     * in the file to be removed. Note that if this is a cluster instance
     * we don't want a slaveof directive inside redis.conf. */
    if (server.cluster_enabled || server.masterhost == NULL) {
        rewriteConfigMarkAsProcessed(state,option);
        return;
    }
    line = sdscatprintf(sdsempty(),"%s %s %d", option,
        server.masterhost, server.masterport);
    rewriteConfigRewriteLine(state,option,line,1);
}

/* Rewrite the notify-keyspace-events option. */
void rewriteConfigNotifykeyspaceeventsOption(struct rewriteConfigState *state) {
    int force = server.notify_keyspace_events != 0;
    char *option = "notify-keyspace-events";
    sds line, flags;

    flags = keyspaceEventsFlagsToString(server.notify_keyspace_events);
    line = sdsnew(option);
    line = sdscatlen(line, " ", 1);
    line = sdscatrepr(line, flags, sdslen(flags));
    sdsfree(flags);
    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite the client-output-buffer-limit option. */
void rewriteConfigClientoutputbufferlimitOption(struct rewriteConfigState *state) {
    int j;
    char *option = "client-output-buffer-limit";

    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++) {
        int force = (server.client_obuf_limits[j].hard_limit_bytes !=
                    clientBufferLimitsDefaults[j].hard_limit_bytes) ||
                    (server.client_obuf_limits[j].soft_limit_bytes !=
                    clientBufferLimitsDefaults[j].soft_limit_bytes) ||
                    (server.client_obuf_limits[j].soft_limit_seconds !=
                    clientBufferLimitsDefaults[j].soft_limit_seconds);
        sds line;
        char hard[64], soft[64];

        rewriteConfigFormatMemory(hard,sizeof(hard),
                server.client_obuf_limits[j].hard_limit_bytes);
        rewriteConfigFormatMemory(soft,sizeof(soft),
                server.client_obuf_limits[j].soft_limit_bytes);

        char *typename = getClientTypeName(j);
        if (!strcmp(typename,"slave")) typename = "replica";
        line = sdscatprintf(sdsempty(),"%s %s %s %s %ld",
                option, typename, hard, soft,
                (long) server.client_obuf_limits[j].soft_limit_seconds);
        rewriteConfigRewriteLine(state,option,line,force);
    }
}

/* Rewrite the bind option. */
void rewriteConfigBindOption(struct rewriteConfigState *state) {
    int force = 1;
    sds line, addresses;
    char *option = "bind";

    /* Nothing to rewrite if we don't have bind addresses. */
    if (server.bindaddr_count == 0) {
        rewriteConfigMarkAsProcessed(state,option);
        return;
    }

    /* Rewrite as bind <addr1> <addr2> ... <addrN> */
    addresses = sdsjoin(server.bindaddr,server.bindaddr_count," ");
    line = sdsnew(option);
    line = sdscatlen(line, " ", 1);
    line = sdscatsds(line, addresses);
    sdsfree(addresses);

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite the requirepass option. */
void rewriteConfigRequirepassOption(struct rewriteConfigState *state, char *option) {
    int force = 1;
    sds line;
    sds password = ACLDefaultUserFirstPassword();

    /* If there is no password set, we don't want the requirepass option
     * to be present in the configuration at all. */
    if (password == NULL) {
        rewriteConfigMarkAsProcessed(state,option);
        return;
    }

    line = sdsnew(option);
    line = sdscatlen(line, " ", 1);
    line = sdscatsds(line, password);

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Glue together the configuration lines in the current configuration
 * rewrite state into a single string, stripping multiple empty lines. */
sds rewriteConfigGetContentFromState(struct rewriteConfigState *state) {
    sds content = sdsempty();
    int j, was_empty = 0;

    for (j = 0; j < state->numlines; j++) {
        /* Every cluster of empty lines is turned into a single empty line. */
        if (sdslen(state->lines[j]) == 0) {
            if (was_empty) continue;
            was_empty = 1;
        } else {
            was_empty = 0;
        }
        content = sdscatsds(content,state->lines[j]);
        content = sdscatlen(content,"\n",1);
    }
    return content;
}

/* Free the configuration rewrite state. */
void rewriteConfigReleaseState(struct rewriteConfigState *state) {
    sdsfreesplitres(state->lines,state->numlines);
    dictRelease(state->option_to_line);
    dictRelease(state->rewritten);
    zfree(state);
}

/* At the end of the rewrite process the state contains the remaining
 * map between "option name" => "lines in the original config file".
 * Lines used by the rewrite process were removed by the function
 * rewriteConfigRewriteLine(), all the other lines are "orphaned" and
 * should be replaced by empty lines.
 *
 * This function does just this, iterating all the option names and
 * blanking all the lines still associated. */
void rewriteConfigRemoveOrphaned(struct rewriteConfigState *state) {
    dictIterator *di = dictGetIterator(state->option_to_line);
    dictEntry *de;

    while((de = dictNext(di)) != NULL) {
        list *l = dictGetVal(de);
        sds option = dictGetKey(de);

        /* Don't blank lines about options the rewrite process
         * don't understand. */
        if (dictFind(state->rewritten,option) == NULL) {
            serverLog(LL_DEBUG,"Not rewritten option: %s", option);
            continue;
        }

        while(listLength(l)) {
            listNode *ln = listFirst(l);
            int linenum = (long) ln->value;

            sdsfree(state->lines[linenum]);
            state->lines[linenum] = sdsempty();
            listDelNode(l,ln);
        }
    }
    dictReleaseIterator(di);
}

/* This function overwrites the old configuration file with the new content.
 *
 * 1) The old file length is obtained.
 * 2) If the new content is smaller, padding is added.
 * 3) A single write(2) call is used to replace the content of the file.
 * 4) Later the file is truncated to the length of the new content.
 *
 * This way we are sure the file is left in a consistent state even if the
 * process is stopped between any of the four operations.
 *
 * The function returns 0 on success, otherwise -1 is returned and errno
 * set accordingly. */
int rewriteConfigOverwriteFile(char *configfile, sds content) {
    int retval = 0;
    int fd = open(configfile,O_RDWR|O_CREAT,0644);
    int content_size = sdslen(content), padding = 0;
    struct stat sb;
    sds content_padded;

    /* 1) Open the old file (or create a new one if it does not
     *    exist), get the size. */
    if (fd == -1) return -1; /* errno set by open(). */
    if (fstat(fd,&sb) == -1) {
        close(fd);
        return -1; /* errno set by fstat(). */
    }

    /* 2) Pad the content at least match the old file size. */
    content_padded = sdsdup(content);
    if (content_size < sb.st_size) {
        /* If the old file was bigger, pad the content with
         * a newline plus as many "#" chars as required. */
        padding = sb.st_size - content_size;
        content_padded = sdsgrowzero(content_padded,sb.st_size);
        content_padded[content_size] = '\n';
        memset(content_padded+content_size+1,'#',padding-1);
    }

    /* 3) Write the new content using a single write(2). */
    if (write(fd,content_padded,strlen(content_padded)) == -1) {
        retval = -1;
        goto cleanup;
    }

    /* 4) Truncate the file to the right length if we used padding. */
    if (padding) {
        if (ftruncate(fd,content_size) == -1) {
            /* Non critical error... */
        }
    }

cleanup:
    sdsfree(content_padded);
    close(fd);
    return retval;
}

/* Rewrite the configuration file at "path".
 * If the configuration file already exists, we try at best to retain comments
 * and overall structure.
 *
 * Configuration parameters that are at their default value, unless already
 * explicitly included in the old configuration file, are not rewritten.
 *
 * On error -1 is returned and errno is set accordingly, otherwise 0. */
int rewriteConfig(char *path) {
    struct rewriteConfigState *state;
    sds newcontent;
    int retval;

    /* Step 1: read the old config into our rewrite state. */
    if ((state = rewriteConfigReadOldFile(path)) == NULL) return -1;

    /* Step 2: rewrite every single option, replacing or appending it inside
     * the rewrite state. */

    /* Iterate the configs that are standard */
    for (standardConfig *config = configs; config->name != NULL; config++) {
        config->interface.rewrite(config->data, config->name, state);
    }

    rewriteConfigBindOption(state);
    rewriteConfigOctalOption(state,"unixsocketperm",server.unixsocketperm,CONFIG_DEFAULT_UNIX_SOCKET_PERM);
    rewriteConfigStringOption(state,"logfile",server.logfile,CONFIG_DEFAULT_LOGFILE);
    rewriteConfigYesNoOption(state,"syslog-enabled",server.syslog_enabled,CONFIG_DEFAULT_SYSLOG_ENABLED);
    rewriteConfigStringOption(state,"syslog-ident",server.syslog_ident,CONFIG_DEFAULT_SYSLOG_IDENT);
    rewriteConfigSaveOption(state);
    rewriteConfigUserOption(state);
    rewriteConfigStringOption(state,"dbfilename",server.rdb_filename,CONFIG_DEFAULT_RDB_FILENAME);
    rewriteConfigDirOption(state);
    rewriteConfigSlaveofOption(state,"replicaof");
    rewriteConfigBytesOption(state,"repl-backlog-size",server.repl_backlog_size,CONFIG_DEFAULT_REPL_BACKLOG_SIZE);
    rewriteConfigBytesOption(state,"repl-backlog-ttl",server.repl_backlog_time_limit,CONFIG_DEFAULT_REPL_BACKLOG_TIME_LIMIT);
    rewriteConfigRequirepassOption(state,"requirepass");
    rewriteConfigNumericalOption(state,"maxclients",server.maxclients,CONFIG_DEFAULT_MAX_CLIENTS);
    rewriteConfigBytesOption(state,"maxmemory",server.maxmemory,CONFIG_DEFAULT_MAXMEMORY);
    rewriteConfigBytesOption(state,"client-query-buffer-limit",server.client_max_querybuf_len,PROTO_MAX_QUERYBUF_LEN);
    rewriteConfigYesNoOption(state,"appendonly",server.aof_enabled,0);
    rewriteConfigStringOption(state,"appendfilename",server.aof_filename,CONFIG_DEFAULT_AOF_FILENAME);
    rewriteConfigYesNoOption(state,"cluster-enabled",server.cluster_enabled,0);
    rewriteConfigStringOption(state,"cluster-config-file",server.cluster_configfile,CONFIG_DEFAULT_CLUSTER_CONFIG_FILE);
    rewriteConfigNotifykeyspaceeventsOption(state);
    rewriteConfigYesNoOption(state,"activedefrag",server.active_defrag_enabled,CONFIG_DEFAULT_ACTIVE_DEFRAG);
    rewriteConfigClientoutputbufferlimitOption(state);
    rewriteConfigNumericalOption(state,"hz",server.config_hz,CONFIG_DEFAULT_HZ);

#ifdef USE_OPENSSL
    rewriteConfigYesNoOption(state,"tls-cluster",server.tls_cluster,0);
    rewriteConfigYesNoOption(state,"tls-replication",server.tls_replication,0);
    rewriteConfigYesNoOption(state,"tls-auth-clients",server.tls_auth_clients,1);
    rewriteConfigStringOption(state,"tls-cert-file",server.tls_ctx_config.cert_file,NULL);
    rewriteConfigStringOption(state,"tls-key-file",server.tls_ctx_config.key_file,NULL);
    rewriteConfigStringOption(state,"tls-dh-params-file",server.tls_ctx_config.dh_params_file,NULL);
    rewriteConfigStringOption(state,"tls-ca-cert-file",server.tls_ctx_config.ca_cert_file,NULL);
    rewriteConfigStringOption(state,"tls-ca-cert-dir",server.tls_ctx_config.ca_cert_dir,NULL);
    rewriteConfigStringOption(state,"tls-protocols",server.tls_ctx_config.protocols,NULL);
    rewriteConfigStringOption(state,"tls-ciphers",server.tls_ctx_config.ciphers,NULL);
    rewriteConfigStringOption(state,"tls-ciphersuites",server.tls_ctx_config.ciphersuites,NULL);
    rewriteConfigYesNoOption(state,"tls-prefer-server-ciphers",server.tls_ctx_config.prefer_server_ciphers,0);
#endif

    /* Rewrite Sentinel config if in Sentinel mode. */
    if (server.sentinel_mode) rewriteConfigSentinelOption(state);

    /* Step 3: remove all the orphaned lines in the old file, that is, lines
     * that were used by a config option and are no longer used, like in case
     * of multiple "save" options or duplicated options. */
    rewriteConfigRemoveOrphaned(state);

    /* Step 4: generate a new configuration file from the modified state
     * and write it into the original file. */
    newcontent = rewriteConfigGetContentFromState(state);
    retval = rewriteConfigOverwriteFile(server.configfile,newcontent);

    sdsfree(newcontent);
    rewriteConfigReleaseState(state);
    return retval;
}

/*-----------------------------------------------------------------------------
 * Configs that fit one of the major types and require no special handling
 *----------------------------------------------------------------------------*/
#define LOADBUF_SIZE 256
static char loadbuf[LOADBUF_SIZE];

#define MODIFIABLE_CONFIG 1
#define IMMUTABLE_CONFIG 0

#define embedCommonConfig(config_name, config_alias, is_modifiable) \
    .name = (config_name), \
    .alias = (config_alias), \
    .modifiable = (is_modifiable),

#define embedConfigInterface(loadfn, setfn, getfn, rewritefn) .interface = { \
    .load = (loadfn), \
    .set = (setfn), \
    .get = (getfn), \
    .rewrite = (rewritefn) \
},

/* 
 * What follows is the generic config types that are supported. To add a new
 * config with one of these types, add it to the standardConfig table with
 * the creation macro for each type.
 * 
 * Each type contains the following:
 * * A function defining how to load this type on startup.
 * * A function defining how to update this type on CONFIG SET.
 * * A function defining how to serialize this type on CONFIG SET.
 * * A function defining how to rewrite this type on CONFIG REWRITE.
 * * A Macro defining how to create this type. 
 */
/* Bool Configs */
static int boolConfigLoad(typeData data, sds *argv, int argc, char **err) {
    if (argc != 2) {
        *err = "wrong number of arguments";
        return 0;
    }
    if ((*(data.bool.config) = yesnotoi(argv[1])) == -1) {
        *err = "argument must be 'yes' or 'no'";
        return 0;
    }
    return 1;
}

static int boolConfigSet(typeData data, sds value) {
    int yn = yesnotoi(value);
    if (yn == -1) return 0;
    *(data.bool.config) = yn;
    return 1;
}

static void boolConfigGet(client *c, typeData data) {
    addReplyBulkCString(c, data.bool.config ? "yes" : "no");
}

static void boolConfigRewrite(typeData data, const char *name, struct rewriteConfigState *state) {
    rewriteConfigYesNoOption(state, name,*(data.bool.config), data.bool.default_value);
}

#define createBoolConfig(name, alias, modifiable, config_addr, default) { \
    embedCommonConfig(name, alias, modifiable) \
    embedConfigInterface(boolConfigLoad, boolConfigSet, boolConfigGet, boolConfigRewrite) \
    .data.bool = { \
        .config = &(config_addr), \
        .default_value = (default) \
    } \
}

/* String Configs */
static int stringConfigLoad(typeData data, sds *argv, int argc, char **err) {
    if (argc != 2) {
        *err = "wrong number of arguments";
        return 0;
    }
    zfree(*data.string.config);
    if (data.string.convert_empty_to_null) {
        *data.string.config = argv[1][0] ? zstrdup(argv[1]) : NULL;
    } else {
        *data.string.config = zstrdup(argv[1]);
    }
    return 1;
}

static int stringConfigSet(typeData data, sds value) {
    zfree(*data.string.config);
    if (data.string.convert_empty_to_null) {
        *data.string.config = value[0] ? zstrdup(value) : NULL;
    } else {
        *data.string.config = zstrdup(value);
    }
    return 1;
}

static void stringConfigGet(client *c, typeData data) {
    addReplyBulkCString(c, *data.string.config ? *data.string.config : "");
}

static void stringConfigRewrite(typeData data, const char *name, struct rewriteConfigState *state) {
    rewriteConfigStringOption(state, name,*(data.string.config), data.string.default_value);
}

#define ALLOW_EMPTY_STRING 0
#define EMPTY_STRING_IS_NULL 1

#define createStringConfig(name, alias, modifiable, empty_to_null, config_addr, default) { \
    embedCommonConfig(name, alias, modifiable) \
    embedConfigInterface(stringConfigLoad, stringConfigSet, stringConfigGet, stringConfigRewrite) \
    .data.string = { \
        .config = &(config_addr), \
        .default_value = (default), \
        .convert_empty_to_null = (empty_to_null) \
    } \
}

/* Enum configs */
static int configEnumLoad(typeData data, sds *argv, int argc, char **err) {
    if (argc != 2) {
        *err = "wrong number of arguments";
        return 0;
    }

    int enumval = configEnumGetValue(data.enumd.enum_value, argv[1]);
    if (enumval == INT_MIN) {
        sds enumerr = sdsnew("argument must be one of the following: ");
        configEnum *enumNode = data.enumd.enum_value;
        while(enumNode->name != NULL) {
            sdscatlen(enumerr, enumNode->name, strlen(enumNode->name));
            sdscatlen(enumerr, ", ", 2);
            enumNode++;
        }

        enumerr[sdslen(enumerr) - 2] = '\0';

        /* Make sure we don't overrun the fixed buffer */
        enumerr[LOADBUF_SIZE - 1] = '\0';
        strncpy(loadbuf, enumerr, LOADBUF_SIZE);

        sdsfree(enumerr);
        *err = loadbuf;
        return 0;
    }
    *(data.enumd.config) = enumval;
    return 1;
}

static int configEnumSet(typeData data, sds value) {
    int enumval = configEnumGetValue(data.enumd.enum_value, value);
    if (enumval == INT_MIN) return 0;
    *(data.enumd.config) = enumval;
    return 1;
}

static void configEnumGet(client *c, typeData data) {
    addReplyBulkCString(c, configEnumGetNameOrUnknown(data.enumd.enum_value,*data.enumd.config));
}

static void configEnumRewrite(typeData data, const char *name, struct rewriteConfigState *state) {
    rewriteConfigEnumOption(state, name,*(data.enumd.config), data.enumd.enum_value, data.enumd.default_value);
}

#define createEnumConfig(name, alias, modifiable, enum, config_addr, default) { \
    embedCommonConfig(name, alias, modifiable) \
    embedConfigInterface(configEnumLoad, configEnumSet, configEnumGet, configEnumRewrite) \
    .data.enumd = { \
        .config = &(config_addr), \
        .default_value = (default), \
        .enum_value = (enum) \
    } \
}

/* Numeric configs */
static int numericConfigLoad(typeData data, sds *argv, int argc, char **err) {
    long long ll;

    if (argc != 2) {
        *err = "wrong number of arguments";
        return 0;
    }

    if (data.numeric.is_memory) {
        int memerr;
        ll = memtoll(argv[1], &memerr);
        if (memerr || ll < 0) {
            *err = "argument must be a memory value";
            return 0;
        } 
    } else {
        if (!string2ll(argv[1], sdslen(argv[1]),&ll)) {
            *err = "argument couldn't be parsed into an integer" ;
            return 0;   
        }
    }

    if (ll > data.numeric.upper_bound ||
               ll < data.numeric.lower_bound) {
        snprintf(loadbuf, LOADBUF_SIZE, 
            "argument must be between %lld and %lld inclusive", 
            data.numeric.lower_bound, 
            data.numeric.upper_bound);
        *err = loadbuf;
        return 0;
    }

    if (data.numeric.numeric_type == NUMERIC_TYPE_INT) {
        *(data.numeric.config.i) = (int) ll;
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_UNSIGNED_LONG) {
        *(data.numeric.config.ul) = (unsigned long) ll;
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_LONG_LONG) {
        *(data.numeric.config.ll) = ll;
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_SIZE_T) {
        *(data.numeric.config.st) = (size_t) ll;
    }

    return 1;
}

static int numericConfigSet(typeData data, sds value) {
    long long ll;
    if (data.numeric.is_memory) {
        int memerr;
        ll = memtoll(value, &memerr);
        if (memerr || ll < 0) return 0;
    } else {
        if (!string2ll(value, sdslen(value),&ll)) return 0;
    }

    if (ll > data.numeric.upper_bound ||
               ll < data.numeric.lower_bound) {
        return 0;
    }

    if (data.numeric.numeric_type == NUMERIC_TYPE_INT) {
        *(data.numeric.config.i) = (int) ll;
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_UNSIGNED_LONG) {
        *(data.numeric.config.ul) = (unsigned long) ll;
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_LONG_LONG) {
        *(data.numeric.config.ll) = ll;
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_SIZE_T) {
        *(data.numeric.config.st) = (size_t) ll;
    }

    return 1;
}

static void numericConfigGet(client *c, typeData data) {
    char buf[128];
    long long value = 0;

    if (data.numeric.numeric_type == NUMERIC_TYPE_INT) {
        value = *(data.numeric.config.i);
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_UNSIGNED_LONG) {
        value = *(data.numeric.config.ul);
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_LONG_LONG) {
        value = *(data.numeric.config.ll);
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_SIZE_T) {
        value = *(data.numeric.config.st);
    }

    ll2string(buf, sizeof(buf), value);
    addReplyBulkCString(c, buf);
}

static void numericConfigRewrite(typeData data, const char *name, struct rewriteConfigState *state) {
    long long value;
    if (data.numeric.numeric_type == NUMERIC_TYPE_INT) {
        value = *(data.numeric.config.i);
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_UNSIGNED_LONG) {
        value = *(data.numeric.config.ul);
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_LONG_LONG) {
        value = *(data.numeric.config.ll);
    } else if (data.numeric.numeric_type == NUMERIC_TYPE_SIZE_T) {
        value = *(data.numeric.config.st);
    }

    if (data.numeric.is_memory) {
        rewriteConfigBytesOption(state, name, value, data.numeric.default_value);
    } else {
        rewriteConfigNumericalOption(state, name, value, data.numeric.default_value);
    }
}

#define INTEGER_CONFIG 0
#define MEMORY_CONFIG 1

#define embedCommonNumericalConfig(name, alias, modifiable, lower, upper, config_addr, default, memory) { \
    embedCommonConfig(name, alias, modifiable) \
    embedConfigInterface(numericConfigLoad, numericConfigSet, numericConfigGet, numericConfigRewrite) \
    .data.numeric = { \
        .lower_bound = (lower), \
        .upper_bound = (upper), \
        .default_value = (default), \
        .is_memory = (memory),

#define createIntConfig(name, alias, modifiable, lower, upper, config_addr, default, memory) \
    embedCommonNumericalConfig(name, alias, modifiable, lower, upper, config_addr, default, memory) \
        .numeric_type = NUMERIC_TYPE_INT, \
        .config.i = &(config_addr) \
    } \
}

#define createUnsignedLongConfig(name, alias, modifiable, lower, upper, config_addr, default, memory) \
    embedCommonNumericalConfig(name, alias, modifiable, lower, upper, config_addr, default, memory) \
        .numeric_type = NUMERIC_TYPE_UNSIGNED_LONG, \
        .config.ul = &(config_addr) \
    } \
}

#define createLongLongConfig(name, alias, modifiable, lower, upper, config_addr, default, memory) \
    embedCommonNumericalConfig(name, alias, modifiable, lower, upper, config_addr, default, memory) \
        .numeric_type = NUMERIC_TYPE_LONG_LONG, \
        .config.ll = &(config_addr) \
    } \
}

#define createSizeTConfig(name, alias, modifiable, lower, upper, config_addr, default, memory) \
    embedCommonNumericalConfig(name, alias, modifiable, lower, upper, config_addr, default, memory) \
        .numeric_type = NUMERIC_TYPE_SIZE_T, \
        .config.st = &(config_addr) \
    } \
}

standardConfig configs[] = {
    /* Bool configs */
    createBoolConfig("rdbchecksum", NULL, IMMUTABLE_CONFIG, server.rdb_checksum, CONFIG_DEFAULT_RDB_CHECKSUM), 
    createBoolConfig("daemonize", NULL, IMMUTABLE_CONFIG, server.daemonize, 0), 
    createBoolConfig("io-threads-do-reads", NULL, IMMUTABLE_CONFIG, server.io_threads_do_reads, CONFIG_DEFAULT_IO_THREADS_DO_READS),
    createBoolConfig("lua-replicate-commands", NULL, IMMUTABLE_CONFIG, server.lua_always_replicate_commands, 1),
    createBoolConfig("always-show-logo", NULL, IMMUTABLE_CONFIG, server.always_show_logo, CONFIG_DEFAULT_ALWAYS_SHOW_LOGO), 
    createBoolConfig("protected-mode", NULL, MODIFIABLE_CONFIG, server.protected_mode, CONFIG_DEFAULT_PROTECTED_MODE), 
    createBoolConfig("rdbcompression", NULL, MODIFIABLE_CONFIG, server.rdb_compression, CONFIG_DEFAULT_RDB_COMPRESSION), 
    createBoolConfig("activerehashing", NULL, MODIFIABLE_CONFIG, server.activerehashing, CONFIG_DEFAULT_ACTIVE_REHASHING), 
    createBoolConfig("stop-writes-on-bgsave-error", NULL, MODIFIABLE_CONFIG, server.stop_writes_on_bgsave_err, CONFIG_DEFAULT_STOP_WRITES_ON_BGSAVE_ERROR), 
    createBoolConfig("dynamic-hz", NULL, MODIFIABLE_CONFIG, server.dynamic_hz, CONFIG_DEFAULT_DYNAMIC_HZ), 
    createBoolConfig("lazyfree-lazy-eviction", NULL, MODIFIABLE_CONFIG, server.lazyfree_lazy_eviction, CONFIG_DEFAULT_LAZYFREE_LAZY_EVICTION), 
    createBoolConfig("lazyfree-lazy-expire", NULL, MODIFIABLE_CONFIG, server.lazyfree_lazy_expire, CONFIG_DEFAULT_LAZYFREE_LAZY_EXPIRE), 
    createBoolConfig("lazyfree-lazy-server-del", NULL, MODIFIABLE_CONFIG, server.lazyfree_lazy_server_del, CONFIG_DEFAULT_LAZYFREE_LAZY_SERVER_DEL), 
    createBoolConfig("repl-disable-tcp-nodelay", NULL, MODIFIABLE_CONFIG, server.repl_disable_tcp_nodelay, CONFIG_DEFAULT_REPL_DISABLE_TCP_NODELAY), 
    createBoolConfig("repl-diskless-sync", NULL, MODIFIABLE_CONFIG, server.repl_diskless_sync, CONFIG_DEFAULT_REPL_DISKLESS_SYNC), 
    createBoolConfig("gopher-enabled", NULL, MODIFIABLE_CONFIG, server.gopher_enabled, CONFIG_DEFAULT_GOPHER_ENABLED), 
    createBoolConfig("aof-rewrite-incremental-fsync", NULL, MODIFIABLE_CONFIG, server.aof_rewrite_incremental_fsync, CONFIG_DEFAULT_AOF_REWRITE_INCREMENTAL_FSYNC), 
    createBoolConfig("no-appendfsync-on-rewrite", NULL, MODIFIABLE_CONFIG, server.aof_no_fsync_on_rewrite, CONFIG_DEFAULT_AOF_NO_FSYNC_ON_REWRITE), 
    createBoolConfig("cluster-require-full-coverage", NULL, MODIFIABLE_CONFIG, server.cluster_require_full_coverage, CLUSTER_DEFAULT_REQUIRE_FULL_COVERAGE), 
    createBoolConfig("rdb-save-incremental-fsync", NULL, MODIFIABLE_CONFIG, server.rdb_save_incremental_fsync, CONFIG_DEFAULT_RDB_SAVE_INCREMENTAL_FSYNC), 
    createBoolConfig("aof-load-truncated", NULL, MODIFIABLE_CONFIG, server.aof_load_truncated, CONFIG_DEFAULT_AOF_LOAD_TRUNCATED), 
    createBoolConfig("aof-use-rdb-preamble", NULL, MODIFIABLE_CONFIG, server.aof_use_rdb_preamble, CONFIG_DEFAULT_AOF_USE_RDB_PREAMBLE), 
    createBoolConfig("cluster-replica-no-failover", "cluster-slave-no-failover", MODIFIABLE_CONFIG, server.cluster_slave_no_failover, CLUSTER_DEFAULT_SLAVE_NO_FAILOVER), 
    createBoolConfig("replica-lazy-flush", "slave-lazy-flush", MODIFIABLE_CONFIG, server.repl_slave_lazy_flush, CONFIG_DEFAULT_SLAVE_LAZY_FLUSH), 
    createBoolConfig("replica-serve-stale-data", "slave-serve-stale-data", MODIFIABLE_CONFIG, server.repl_serve_stale_data, CONFIG_DEFAULT_SLAVE_SERVE_STALE_DATA), 
    createBoolConfig("replica-read-only", "slave-read-only", MODIFIABLE_CONFIG, server.repl_slave_ro, CONFIG_DEFAULT_SLAVE_READ_ONLY), 
    createBoolConfig("replica-ignore-maxmemory", "slave-ignore-maxmemory", MODIFIABLE_CONFIG, server.repl_slave_ignore_maxmemory, CONFIG_DEFAULT_SLAVE_IGNORE_MAXMEMORY), 
    createBoolConfig("jemalloc-bg-thread", NULL, MODIFIABLE_CONFIG, server.jemalloc_bg_thread, 1), 

    /* String Configs */
    createStringConfig("aclfile", NULL, IMMUTABLE_CONFIG, ALLOW_EMPTY_STRING, server.acl_filename, CONFIG_DEFAULT_ACL_FILENAME), 
    createStringConfig("unixsocket", NULL, IMMUTABLE_CONFIG, EMPTY_STRING_IS_NULL, server.unixsocket, NULL), 
    createStringConfig("pidfile", NULL, IMMUTABLE_CONFIG, EMPTY_STRING_IS_NULL, server.pidfile, CONFIG_DEFAULT_PID_FILE), 
    createStringConfig("replica-announce-ip", "slave-announce-ip", MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.slave_announce_ip, CONFIG_DEFAULT_SLAVE_ANNOUNCE_IP), 
    createStringConfig("masteruser", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.masteruser, NULL), 
    createStringConfig("masterauth", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.masterauth, NULL), 
    createStringConfig("cluster-announce-ip", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.cluster_announce_ip, NULL), 

    /* Enum Configs */
    createEnumConfig("supervised", NULL, IMMUTABLE_CONFIG, supervised_mode_enum, server.supervised_mode, SUPERVISED_NONE), 
    createEnumConfig("syslog-facility", NULL, IMMUTABLE_CONFIG, syslog_facility_enum, server.syslog_facility, LOG_LOCAL0), 
    createEnumConfig("repl-diskless-load", NULL, MODIFIABLE_CONFIG, repl_diskless_load_enum, server.repl_diskless_load, CONFIG_DEFAULT_REPL_DISKLESS_LOAD), 
    createEnumConfig("loglevel", NULL, MODIFIABLE_CONFIG, loglevel_enum, server.verbosity, CONFIG_DEFAULT_VERBOSITY), 
    createEnumConfig("maxmemory-policy", NULL, MODIFIABLE_CONFIG, maxmemory_policy_enum, server.maxmemory_policy, CONFIG_DEFAULT_MAXMEMORY_POLICY), 
    createEnumConfig("appendfsync", NULL, MODIFIABLE_CONFIG, aof_fsync_enum, server.aof_fsync, CONFIG_DEFAULT_AOF_FSYNC), 

    /* Integer configs */
    createIntConfig("databases", NULL, IMMUTABLE_CONFIG, 1, INT_MAX, server.dbnum, CONFIG_DEFAULT_DBNUM, INTEGER_CONFIG), 
    createIntConfig("port", NULL, IMMUTABLE_CONFIG, 0, 65535, server.port, CONFIG_DEFAULT_SERVER_PORT, INTEGER_CONFIG), 
    createIntConfig("io-threads", NULL, IMMUTABLE_CONFIG, 1, 512, server.io_threads_num, CONFIG_DEFAULT_IO_THREADS_NUM, INTEGER_CONFIG), 
    createIntConfig("auto-aof-rewrite-percentage", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.aof_rewrite_perc, AOF_REWRITE_PERC, INTEGER_CONFIG), 
    createIntConfig("cluster-replica-validity-factor", "cluster-slave-validity-factor", MODIFIABLE_CONFIG, 0, INT_MAX, server.cluster_slave_validity_factor, CLUSTER_DEFAULT_SLAVE_VALIDITY, INTEGER_CONFIG), 
    createIntConfig("list-max-ziplist-size", NULL, MODIFIABLE_CONFIG, INT_MIN, INT_MAX, server.list_max_ziplist_size, OBJ_LIST_MAX_ZIPLIST_SIZE, INTEGER_CONFIG), 
    createIntConfig("tcp-keepalive", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.tcpkeepalive, CONFIG_DEFAULT_TCP_KEEPALIVE, INTEGER_CONFIG), 
    createIntConfig("cluster-migration-barrier", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.cluster_migration_barrier, CLUSTER_DEFAULT_MIGRATION_BARRIER, INTEGER_CONFIG), 
    createIntConfig("active-defrag-cycle-min", NULL, MODIFIABLE_CONFIG, 1, 99, server.active_defrag_cycle_min, CONFIG_DEFAULT_DEFRAG_CYCLE_MIN, INTEGER_CONFIG), 
    createIntConfig("active-defrag-cycle-max", NULL, MODIFIABLE_CONFIG, 1, 99, server.active_defrag_cycle_max, CONFIG_DEFAULT_DEFRAG_CYCLE_MAX, INTEGER_CONFIG), 
    createIntConfig("active-defrag-threshold-lower", NULL, MODIFIABLE_CONFIG, 0, 1000, server.active_defrag_threshold_lower, CONFIG_DEFAULT_DEFRAG_THRESHOLD_LOWER, INTEGER_CONFIG), 
    createIntConfig("active-defrag-threshold-upper", NULL, MODIFIABLE_CONFIG, 0, 1000, server.active_defrag_threshold_upper, CONFIG_DEFAULT_DEFRAG_THRESHOLD_UPPER, INTEGER_CONFIG), 
    createIntConfig("lfu-log-factor", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.lfu_log_factor, CONFIG_DEFAULT_LFU_LOG_FACTOR, INTEGER_CONFIG), 
    createIntConfig("lfu-decay-time", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.lfu_decay_time, CONFIG_DEFAULT_LFU_DECAY_TIME, INTEGER_CONFIG), 
    createIntConfig("replica-priority", "slave-priority", MODIFIABLE_CONFIG, 0, INT_MAX, server.slave_priority, CONFIG_DEFAULT_SLAVE_PRIORITY, INTEGER_CONFIG), 
    createIntConfig("repl-diskless-sync-delay", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.repl_diskless_sync_delay, CONFIG_DEFAULT_REPL_DISKLESS_SYNC_DELAY, INTEGER_CONFIG), 
    createIntConfig("maxmemory-samples", NULL, MODIFIABLE_CONFIG, 1, INT_MAX, server.maxmemory_samples, CONFIG_DEFAULT_MAXMEMORY_SAMPLES, INTEGER_CONFIG), 
    createIntConfig("timeout", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.maxidletime, CONFIG_DEFAULT_CLIENT_TIMEOUT, INTEGER_CONFIG), 
    createIntConfig("replica-announce-port", "slave-announce-port", MODIFIABLE_CONFIG, 0, 65535, server.slave_announce_port, CONFIG_DEFAULT_SLAVE_ANNOUNCE_PORT, INTEGER_CONFIG), 
    createIntConfig("tcp-backlog", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.tcp_backlog, CONFIG_DEFAULT_TCP_BACKLOG, INTEGER_CONFIG), 
    createIntConfig("cluster-announce-bus-port", NULL, MODIFIABLE_CONFIG, 0, 65535, server.cluster_announce_bus_port, CONFIG_DEFAULT_CLUSTER_ANNOUNCE_BUS_PORT, INTEGER_CONFIG), 
    createIntConfig("cluster-announce-port", NULL, MODIFIABLE_CONFIG, 0, 65535, server.cluster_announce_port, CONFIG_DEFAULT_CLUSTER_ANNOUNCE_PORT, INTEGER_CONFIG), 
    createIntConfig("repl-timeout", NULL, MODIFIABLE_CONFIG, 1, INT_MAX, server.repl_timeout, CONFIG_DEFAULT_REPL_TIMEOUT, INTEGER_CONFIG), 
    createIntConfig("repl-ping-replica-period", "repl-ping-slave-period", MODIFIABLE_CONFIG, 1, INT_MAX, server.repl_ping_slave_period, CONFIG_DEFAULT_REPL_PING_SLAVE_PERIOD, INTEGER_CONFIG), 
    createIntConfig("list-compress-depth", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.list_compress_depth, OBJ_LIST_COMPRESS_DEPTH, INTEGER_CONFIG), 
    createIntConfig("rdb-key-save-delay", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.rdb_key_save_delay, CONFIG_DEFAULT_RDB_KEY_SAVE_DELAY, INTEGER_CONFIG), 
    createIntConfig("key-load-delay", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.key_load_delay, CONFIG_DEFAULT_KEY_LOAD_DELAY, INTEGER_CONFIG), 
    createIntConfig("tracking-table-max-fill", NULL, MODIFIABLE_CONFIG, 0, 100, server.tracking_table_max_fill, CONFIG_DEFAULT_TRACKING_TABLE_MAX_FILL, INTEGER_CONFIG), 
    createIntConfig("active-expire-effort", NULL, MODIFIABLE_CONFIG, 1, 10, server.active_expire_effort, CONFIG_DEFAULT_ACTIVE_EXPIRE_EFFORT, INTEGER_CONFIG),

    /* Unsigned Long configs */
    createUnsignedLongConfig("active-defrag-max-scan-fields", NULL, MODIFIABLE_CONFIG, 1, LONG_MAX, server.active_defrag_max_scan_fields, CONFIG_DEFAULT_DEFRAG_MAX_SCAN_FIELDS, INTEGER_CONFIG), 
    createUnsignedLongConfig("slowlog-max-len", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.slowlog_max_len, CONFIG_DEFAULT_SLOWLOG_MAX_LEN, INTEGER_CONFIG), 

    /* Long Long configs */
    createLongLongConfig("stream-node-max-entries", NULL, MODIFIABLE_CONFIG, 0, LLONG_MAX, server.stream_node_max_entries, OBJ_STREAM_NODE_MAX_ENTRIES, INTEGER_CONFIG), 
    createLongLongConfig("lua-time-limit", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.lua_time_limit, LUA_SCRIPT_TIME_LIMIT, INTEGER_CONFIG), 
    createLongLongConfig("cluster-node-timeout", NULL, MODIFIABLE_CONFIG, 0, LLONG_MAX, server.cluster_node_timeout, CLUSTER_DEFAULT_NODE_TIMEOUT, INTEGER_CONFIG), 
    createLongLongConfig("slowlog-log-slower-than", NULL, MODIFIABLE_CONFIG, -1, LLONG_MAX, server.slowlog_log_slower_than, CONFIG_DEFAULT_SLOWLOG_LOG_SLOWER_THAN, INTEGER_CONFIG), 
    createLongLongConfig("latency-monitor-threshold", NULL, MODIFIABLE_CONFIG, 0, LLONG_MAX, server.latency_monitor_threshold, CONFIG_DEFAULT_LATENCY_MONITOR_THRESHOLD, INTEGER_CONFIG), 
    createLongLongConfig("proto-max-bulk-len", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.proto_max_bulk_len, CONFIG_DEFAULT_PROTO_MAX_BULK_LEN, MEMORY_CONFIG), 
    createLongLongConfig("auto-aof-rewrite-min-size", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.aof_rewrite_min_size, AOF_REWRITE_MIN_SIZE, MEMORY_CONFIG), 

    /* Size_t configs */
    createSizeTConfig("hash-max-ziplist-entries", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.hash_max_ziplist_entries, OBJ_HASH_MAX_ZIPLIST_ENTRIES, INTEGER_CONFIG), 
    createSizeTConfig("set-max-intset-entries", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.set_max_intset_entries, OBJ_SET_MAX_INTSET_ENTRIES, INTEGER_CONFIG), 
    createSizeTConfig("zset-max-ziplist-entries", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.zset_max_ziplist_entries, OBJ_ZSET_MAX_ZIPLIST_ENTRIES, INTEGER_CONFIG), 
    createSizeTConfig("active-defrag-ignore-bytes", NULL, MODIFIABLE_CONFIG, 1, LONG_MAX, server.active_defrag_ignore_bytes, CONFIG_DEFAULT_DEFRAG_IGNORE_BYTES, MEMORY_CONFIG), 
    createSizeTConfig("hash-max-ziplist-value", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.hash_max_ziplist_value, OBJ_HASH_MAX_ZIPLIST_VALUE, MEMORY_CONFIG), 
    createSizeTConfig("stream-node-max-bytes", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.stream_node_max_bytes, OBJ_STREAM_NODE_MAX_BYTES, MEMORY_CONFIG), 
    createSizeTConfig("zset-max-ziplist-value", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.zset_max_ziplist_value, OBJ_ZSET_MAX_ZIPLIST_VALUE, MEMORY_CONFIG), 
    createSizeTConfig("hll-sparse-max-bytes", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.hll_sparse_max_bytes, CONFIG_DEFAULT_HLL_SPARSE_MAX_BYTES, MEMORY_CONFIG), 

    /* NULL Terminator */
    {NULL}
};

/*-----------------------------------------------------------------------------
 * CONFIG command entry point
 *----------------------------------------------------------------------------*/

void configCommand(client *c) {
    /* Only allow CONFIG GET while loading. */
    if (server.loading && strcasecmp(c->argv[1]->ptr,"get")) {
        addReplyError(c,"Only CONFIG GET is allowed during loading");
        return;
    }

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"GET <pattern> -- Return parameters matching the glob-like <pattern> and their values.",
"SET <parameter> <value> -- Set parameter to value.",
"RESETSTAT -- Reset statistics reported by INFO.",
"REWRITE -- Rewrite the configuration file.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"set") && c->argc == 4) {
        configSetCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"get") && c->argc == 3) {
        configGetCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"resetstat") && c->argc == 2) {
        resetServerStats();
        resetCommandTableStats();
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"rewrite") && c->argc == 2) {
        if (server.configfile == NULL) {
            addReplyError(c,"The server is running without a config file");
            return;
        }
        if (rewriteConfig(server.configfile) == -1) {
            serverLog(LL_WARNING,"CONFIG REWRITE failed: %s", strerror(errno));
            addReplyErrorFormat(c,"Rewriting config file: %s", strerror(errno));
        } else {
            serverLog(LL_WARNING,"CONFIG REWRITE executed with success.");
            addReply(c,shared.ok);
        }
    } else {
        addReplySubcommandSyntaxError(c);
        return;
    }
}
