/*
 Copyright (c) 2011, Oracle and/or its affiliates. All rights
 reserved.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; version 2 of
 the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
 */

#include <unistd.h>
#include <stdlib.h>  
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <time.h>

#include "NdbApi.hpp"

#include <memcached/extension_loggers.h>
#include <memcached/util.h>
#include <memcached/genhash.h>

#include "ndbmemcache_global.h"
#include "debug.h"
#include "timing.h"
#include "Config_v1.h"
#include "TableSpec.h"
#include "QueryPlan.h"
#include "Operation.h"
#include "ExternalValue.h"

extern EXTENSION_LOGGER_DESCRIPTOR *logger;

bool kludge_to_help_with_linking() {
  int i;
  i = genhash_string_hash("abc", 4);
  return (i < 6);
}


/* Functions used by genhash */
extern "C" {
  int str_eq(const void *, size_t, const void *, size_t);
  void * str_key_dup(const void *, size_t);
}

struct hash_ops string_to_pointer_hash = {
  genhash_string_hash,   /* hash function */
  str_eq,                /* equality tester */
  str_key_dup,           /* duplicate a key */
  NULL,                  /* duplicate a value */
  free,                  /* free a key */
  NULL                   /* free a value */
};

int str_eq(const void *k1, size_t s1, const void *k2, size_t s2) {
  return s1 == s2 && memcmp(k1, k2, s1) == 0;
}

void * str_key_dup(const void *key, size_t) {
  return strdup((const char *) key);
}



/*********** VERSION 1 METADATA *******************/

/********************* COMMON SCHEMA FOR VERSION 1.x ***************
 CREATE  TABLE IF NOT EXISTS `ndb_clusters` (
 `cluster_id` INT NOT NULL ,
 `ndb_connectstring` VARCHAR(128) NULL ,
 `microsec_rtt` INT UNSIGNED NOT NULL default 250, 
 PRIMARY KEY (`cluster_id`)
 ) ENGINE = ndbcluster; 
 
 CREATE  TABLE IF NOT EXISTS `cache_policies` (
 `policy_name` VARCHAR(40) NOT NULL PRIMARY KEY,
 `get_policy` ENUM('cache_only','ndb_only','caching','disabled') NOT NULL ,
 `set_policy` ENUM('cache_only','ndb_only','caching','disabled') NOT NULL ,
 `delete_policy` ENUM('cache_only','ndb_only','caching','disabled') NOT NULL ,
 `flush_from_db` ENUM('false', 'true') NOT NULL DEFAULT 'false'
 ) ENGINE = ndbcluster;
 
 CREATE  TABLE IF NOT EXISTS `containers` (
 `name` varchar(50) not null primary key,
 `db_schema` VARCHAR(250) NOT NULL,
 `db_table` VARCHAR(250) NOT NULL,
 `key_columns` VARCHAR(250) NOT NULL,
 `value_columns` VARCHAR(250),
 `flags` VARCHAR(250) NOT NULL DEFAULT "0",
 `increment_column` VARCHAR(250),
 `cas_column` VARCHAR(250),
 `expire_time_column` VARCHAR(250)
 ) ENGINE = ndbcluster;
 
 CREATE  TABLE IF NOT EXISTS `key_prefixes` (
 `server_role_id` INT UNSIGNED NOT NULL DEFAULT 0,
 `key_prefix` VARCHAR(250) NOT NULL ,
 `cluster_id` INT UNSIGNED NOT NULL DEFAULT 0,
 `policy` VARCHAR(40) NOT NULL,
 `container` VARCHAR(50), 
 PRIMARY KEY (`server_role_id`, `key_prefix`)
 ) ENGINE = ndbcluster;
 
 CREATE  TABLE IF NOT EXISTS `last_memcached_signon` (
 `ndb_node_id` INT UNSIGNED NOT NULL PRIMARY KEY,
 `hostname` VARCHAR(255) NOT NULL, 
 `server_role` VARCHAR(40) NOT NULL,
 `signon_time` timestamp NOT NULL 
 ) ENGINE = ndbcluster;
 
 
 ********************* SPECIFIC TO VERSION 1.0 ***************
 
 CREATE  TABLE IF NOT EXISTS `memcache_server_roles` (
 `role_name` VARCHAR(40) NOT NULL ,
 `role_id` INT UNSIGNED NOT NULL ,
 `max_tps` INT UNSIGNED NOT NULL default 100000,
 PRIMARY KEY (`role_name`) )
 ENGINE = ndbcluster;
 
 
 ********************* SPECIFIC TO VERSION 1.1 ***************
 
 CREATE  TABLE IF NOT EXISTS `memcache_server_roles` (
 `role_name` VARCHAR(40) NOT NULL ,
 `role_id` INT UNSIGNED NOT NULL ,
 `max_clients` INT UNSIGNED NOT NULL default 100, 
 `config_timestamp` timestamp 
 PRIMARY KEY (`role_name`) )
 ENGINE = ndbcluster;
 
 ********************* SPECIFIC TO VERSION 1.2 ***************
 ALTER TABLE containers add large_values_table VARCHAR(250);
 CREATE TABLE IF NOT EXISTS `external_values` (
   `id` INT UNSIGNED NOT NULL,
   `part` TINYINT NOT NULL,
   `content` VARBINARY(13950),
    PRIMARY KEY (id,part)
 ) ENGINE = ndbcluster PARTITION BY KEY(id);
 


 
 ****************/


config_v1::config_v1(Configuration * cf) :
  conf(*cf),
  server_role_id(-1), 
  nclusters(0),
  policies_map(0),
  containers_map(0)
{};

config_v1::~config_v1() {
  DEBUG_ENTER_METHOD("config_v1 destructor");
  if(containers_map) {
    genhash_clear(containers_map);
  }
  if(policies_map) {
    genhash_clear(policies_map);
  }
}

bool config_v1::read_configuration() {
  DEBUG_ENTER_METHOD("config_v1::read_configuration");
 
  for(int i = 0 ; i < MAX_CLUSTERS ; i++) cluster_ids[i] = 0;
  
  containers_map  = genhash_init(30, string_to_pointer_hash);
  policies_map    = genhash_init(10, string_to_pointer_hash);
  
  bool success = false;  
  server_role_id = get_server_role_id();
  if(! (server_role_id < 0)) success = get_policies();
  if(success) success = get_connections();
  if(success) success = get_prefixes(server_role_id);
  if(success) {
    log_signon();
    set_initial_cas();
    minor_version_config();
    return true;
  }
  
  logger->log(LOG_WARNING, 0, "Configuration failed.\n");
  return false;
}


/* get_server_role_id():
 SELECT role_id, max_tps 
 FROM memcache_server_roles 
 WHERE role_name = conf.server_role;
 Returns the integer ID, or -1 if the record was not found.
 */
int config_v1::get_server_role_id() {
  uint32_t val = -1;
  
  Ndb db(conf.primary_conn);
  db.init(2);
  TableSpec spec("ndbmemcache.memcache_server_roles",
                 "role_name", "role_id,max_tps");
  QueryPlan plan(&db, &spec);
  Operation op(&plan, OP_READ);
  
  op.key_buffer = (char *) malloc(op.requiredKeyBuffer());
  op.buffer =     (char *) malloc(op.requiredBuffer());
  NdbTransaction *tx = db.startTransaction();
  
  op.clearKeyNullBits();
  op.setKeyPart(COL_STORE_KEY, conf.server_role, strlen(conf.server_role));
  op.readTuple(tx);
  tx->execute(NdbTransaction::Commit);
  
  if(tx->getNdbError().classification == NdbError::NoError) {
    val = op.getIntValue(COL_STORE_VALUE+0);
    conf.max_tps = op.getIntValue(COL_STORE_VALUE+1);
  }
  else {
    logger->log(LOG_WARNING, 0, "\nServer role \"%s\" not found in "
                "configuration database.\n\n", conf.server_role);
  }
  
  tx->close();
  free(op.key_buffer);
  free(op.buffer);
  
  DEBUG_PRINT("Name: \"%s\" -- ID: %d", conf.server_role, val);
  return val;
}

/* get_policies():
 SELECT * FROM cache_policies;
 Creates the policies map (name => prefix_info). 
 Returns true on success.
 
 `get_policy` ENUM('cache_only','ndb_only','caching','disabled') NOT NULL ,
 `set_policy` ENUM('cache_only','ndb_only','caching','disabled') NOT NULL ,
 `delete_policy` ENUM('cache_only','ndb_only','caching','disabled') NOT NULL ,
 `flush_from_db` ENUM('false', 'true') NOT NULL DEFAULT 'false' 
 */
bool config_v1::get_policies() {
  DEBUG_ENTER_METHOD("config_v1::get_policies");
  bool success = true;
  int res;
  Ndb db(conf.primary_conn);
  db.init(4);
  TableSpec spec("ndbmemcache.cache_policies",
                 "policy_name",                 
                 "get_policy,set_policy,delete_policy,flush_from_db");
  QueryPlan plan(&db, &spec); 
  Operation op(&plan, OP_SCAN);
  
  NdbTransaction *tx = db.startTransaction();
  NdbScanOperation *scan = op.scanTable(tx);
  if(! scan) {
    logger->log(LOG_WARNING, 0, tx->getNdbError().message);
    success = false;
  }
  if(tx->execute(NdbTransaction::NoCommit)) {
    logger->log(LOG_WARNING, 0, tx->getNdbError().message);
    success = false;
  }
  while((res = scan->nextResult((const char **) &op.buffer, true, false) == 0)) {
    prefix_info_t * info = (prefix_info_t *) calloc(1, sizeof(prefix_info_t));
    
    char name[41];          //   `policy_name` VARCHAR(40) NOT NULL
    op.copyValue(COL_STORE_KEY, name);
    
    /*  ENUM('cache_only','ndb_only','caching','disabled') NOT NULL 
     is:      1            2          3         4                   */
    unsigned int get_policy = op.getIntValue(COL_STORE_VALUE+0);
    assert((get_policy > 0) && (get_policy < 5));
    if(get_policy == 1 || get_policy == 3) info->do_mc_read = 1; 
    if(get_policy == 2 || get_policy == 3) info->do_db_read = 1; 
    
    unsigned int set_policy = op.getIntValue(COL_STORE_VALUE+1);
    assert((set_policy > 0) && (set_policy < 5));
    if(set_policy == 1 || set_policy == 3) info->do_mc_write = 1; 
    if(set_policy == 2 || set_policy == 3) info->do_db_write = 1; 
    
    unsigned int del_policy = op.getIntValue(COL_STORE_VALUE+2); 
    assert((del_policy > 0) && (del_policy < 5));
    if(del_policy == 1 || del_policy == 3) info->do_mc_delete = 1;
    if(del_policy == 2 || del_policy == 3) info->do_db_delete = 1;
    
    
    /* `flush_from_db` ENUM('false', 'true') NOT NULL DEFAULT 'false' 
     is:                   1        2          */
    int flush_policy = op.getIntValue(COL_STORE_VALUE+3);
    if(flush_policy == 2) info->do_db_flush = 1;
    
    DEBUG_PRINT("%s:  get-%d set-%d del-%d flush-%d addr-%p",
                name, get_policy, set_policy, del_policy, flush_policy, info);
    
    genhash_store(policies_map, name, strlen(name), info, sizeof(void *));
    
  }
  if(res == -1) {
    logger->log(LOG_WARNING, 0, scan->getNdbError().message);
    success = false;
  }
  DEBUG_PRINT("map size: %d", genhash_size(policies_map));
  
  tx->close();
  
  return success;
}

/* get_connections():
 SELECT * FROM ndb_clusters.
 Creates the cluster_ids map <cfg_data_cluster_id => connections_index>.
 Returns true on success.
 */
bool config_v1::get_connections() {
  DEBUG_ENTER_METHOD("config_v1::get_connections");
  bool success = true;
  int res;
  Ndb db(conf.primary_conn);
  db.init(4);
  TableSpec spec("ndbmemcache.ndb_clusters",
                 "cluster_id",
                 "ndb_connectstring,microsec_rtt");  
  /* Scan the ndb_clusters table */
  QueryPlan plan(&db, &spec); 
  Operation op(&plan, OP_SCAN);
  
  NdbTransaction *tx = db.startTransaction();
  NdbScanOperation *scan = op.scanTable(tx);
  if(! scan) {
    logger->log(LOG_WARNING, 0, tx->getNdbError().message);
    success = false;
  }
  if(tx->execute(NdbTransaction::NoCommit)) {
    logger->log(LOG_WARNING, 0, tx->getNdbError().message);
    success = false;
  }
  while((res = scan->nextResult((const char **) &op.buffer, true, false) == 0)) {   
    bool str_is_null;
    char connectstring[129];
    unsigned int rtt;
    /*  `cluster_id` INT NOT NULL             */
    int cfg_data_id = op.getIntValue(COL_STORE_KEY);
    
    /* `ndb_connectstring` VARCHAR(128) NULL  */  
    str_is_null = op.isNull(COL_STORE_VALUE+0);
    if(! str_is_null) op.copyValue(COL_STORE_VALUE+0, connectstring);
    
    /* `microsec_rtt` INT UNSIGNED NOT NULL default 300  */
    rtt = op.getIntValue(COL_STORE_VALUE+1);
    
    /* Add the connection to the configuration */
    int connection_idx;
    if(str_is_null)
      connection_idx = conf.storeConnection(0, rtt);
    else 
      connection_idx = conf.storeConnection(strdup(connectstring), rtt);
    
    DEBUG_PRINT("[%d]:  { %d => \"%s\" [rtt: %d]}", connection_idx, 
                cfg_data_id, str_is_null ? "" : connectstring, rtt);
    
    nclusters++;
    cluster_ids[connection_idx] = cfg_data_id;
  }
  if(res == -1) {
    logger->log(LOG_WARNING, 0, scan->getNdbError().message);
    success = false;
  }
  DEBUG_PRINT("clusters: %d", nclusters);
  tx->close();
  return success;
}


TableSpec * config_v1::get_container(char *name) {
  TableSpec *c;
  
  c = (TableSpec *) genhash_find(containers_map, name, strlen(name));
  if(c == NULL) {
    c = get_container_record(name);
    genhash_store(containers_map, name, strlen(name), c, sizeof(void *));
  }
  else {
    DEBUG_PRINT("\"%s\" found in local map (\"%s\").", name, c->table_name);
  }
  assert(c);
  return c;
}


TableSpec * config_v1::get_container_record(char *name) {
  TableSpec *container;
  Ndb db(conf.primary_conn);
  db.init(1);
  TableSpec spec("ndbmemcache.containers",
                 "name",  
                 "db_schema,db_table,key_columns,value_columns,flags,"
                 "increment_column,cas_column,expire_time_column");                 
  QueryPlan plan(&db, &spec); 
  Operation op(&plan, OP_READ);
  
  op.key_buffer = (char *) malloc(op.requiredKeyBuffer());
  op.buffer     = (char *) malloc(op.requiredBuffer());
  NdbTransaction *tx = db.startTransaction();
  
  op.clearKeyNullBits();
  op.setKeyPart(COL_STORE_KEY, name, strlen(name));
  op.readTuple(tx);
  tx->execute(NdbTransaction::Commit);
  
  if(tx->getNdbError().classification == NdbError::NoError) {
    char val[256];
    char *schema, *table, *keycols, *valcols;
    
    //  `db_schema` VARCHAR(250) NOT NULL,
    //  `db_table` VARCHAR(250) NOT NULL,
    //  `key_columns` VARCHAR(250) NOT NULL,
    op.copyValue(COL_STORE_VALUE + 0, val); schema = strdup(val); 
    op.copyValue(COL_STORE_VALUE + 1, val); table = strdup(val);
    op.copyValue(COL_STORE_VALUE + 2, val); keycols = strdup(val);
    
    //  `value_columns` VARCHAR(250),
    // TODO: testcase for value_columns is null
    if(op.isNull(COL_STORE_VALUE + 3)) valcols = 0;
    else {
      op.copyValue(COL_STORE_VALUE + 3, val); 
      valcols = strdup(val);
    }
    
    /* Instantiate a TableSpec for this container */
    container = new TableSpec(0, keycols, valcols);
    container->setTable(schema, table);

    if(keycols) free(keycols);
    if(valcols) free(valcols);
        
    //  `flags` VARCHAR(250) NOT NULL DEFAULT "0",
    /* If the value is non-numeric, use it to set the flags_column field */
    container->flags_column = 0;
    container->static_flags = 0;
    op.copyValue(COL_STORE_VALUE + 4, val); 
    if(!safe_strtoul(val, & container->static_flags))
      container->flags_column = strdup(val);
    
    //  `increment_column` VARCHAR(250),
    if(op.isNull(COL_STORE_VALUE + 5)) container->math_column = 0;
    else {
      op.copyValue(COL_STORE_VALUE + 5, val);
      container->math_column = strdup(val);
    }
    
    //  `cas_column` VARCHAR(250),
    if(op.isNull(COL_STORE_VALUE + 6)) container->cas_column = 0;
    else {
      op.copyValue(COL_STORE_VALUE + 6, val);
      container->cas_column = strdup(val);
    }    
    
    //  `expire_time_column` VARCHAR(250)
    if(op.isNull(COL_STORE_VALUE + 7)) container->exp_column = 0;
    else {
      op.copyValue(COL_STORE_VALUE + 7, val);
      container->exp_column = strdup(val);
    }    
    DEBUG_PRINT("\"%s\" found in database (%s).", name, table);
  }
  else {
    container = 0;
    logger->log(LOG_WARNING, 0, "\"%s\" NOT FOUND in database.\n", name);
  }
  
  tx->close();
  free(op.key_buffer);
  free(op.buffer);
  
  return container;
}


/* get_prefixes():
 SELECT * FROM key_prefixes where server_role_id = role_id;
 This is an ordered index scan.  It fetches key prefixes from the 
 configuration metadata and passes them on to store_prefix().
 Returns true on success. 
 */
bool config_v1::get_prefixes(int role_id) {
  DEBUG_ENTER_METHOD("config_v1::get_prefixes");
  bool success = true;
  int res;
  TableSpec spec("ndbmemcache.key_prefixes",
                 "server_role_id,key_prefix", 
                 "cluster_id,policy,container");
  Ndb db(conf.primary_conn);
  db.init(4);
  QueryPlan plan(&db, &spec, PKScan); 
  Operation op(&plan, OP_SCAN);
  
  // `server_role_id` INT UNSIGNED NOT NULL DEFAULT 0,
  // PRIMARY KEY (`server_role_id`, `key_prefix`) )
  op.key_buffer = (char *) malloc(op.requiredKeyBuffer());
  op.setKeyPartInt(COL_STORE_KEY, role_id);
  
  NdbIndexScanOperation::IndexBound bound;
  bound.low_key = bound.high_key = op.key_buffer;
  bound.low_key_count = bound.high_key_count = 1;
  bound.low_inclusive = bound.high_inclusive = true;
  bound.range_no = 0;
  
  NdbTransaction *tx = db.startTransaction();
  NdbIndexScanOperation *scan = op.scanIndex(tx, &bound);
  if(! scan) {
    logger->log(LOG_WARNING, 0, "scanIndex(): %s\n", tx->getNdbError().message);
    success = false;
  }
  if(tx->execute(NdbTransaction::NoCommit)) {
    logger->log(LOG_WARNING, 0, "execute(): %s\n", tx->getNdbError().message);
    success = false;
  }
  while((res = scan->nextResult((const char **) &op.buffer, true, false) == 0)) {
    char key_prefix[251], policy_name[41], container[51];
    int cluster_id = 0;
    TableSpec * container_spec;
    
    // `key_prefix` VARCHAR(250) NOT NULL ,
    op.copyValue(COL_STORE_KEY + 1, key_prefix);
    
    // `cluster_id` INT UNSIGNED NOT NULL DEFAULT 0,
    cluster_id = op.getIntValue(COL_STORE_VALUE + 0);
    
    // `policy` VARCHAR(40) NOT NULL,
    op.copyValue(COL_STORE_VALUE + 1, policy_name);
    
    // `container` VARCHAR(50), 
    if(op.isNull(COL_STORE_VALUE + 2)) container_spec = 0;
    else {
      op.copyValue(COL_STORE_VALUE + 2, container);
      container_spec = get_container(container);
      if(! container_spec) {
        logger->log(LOG_WARNING, 0, "Cannot find container \"%s\" for "
                    "key prefix \"%s\".\n", container, key_prefix);
        free(op.key_buffer);
        return false;      
      }
    }
    
    if(! store_prefix(key_prefix, container_spec, cluster_id, policy_name)) {
      delete[] op.key_buffer;
      return false;  
    }
  } /* while(res=scan->nextResult()) */
  
  free(op.key_buffer);
  return true;
}


/* store_prefix():
 Takes everything needed to build a KeyPrefix.
 If the config is valid, build the KeyPrefix, store it in the configuration,
 and return true.  Otherwise print a warning to the log and return false.
 */
bool config_v1::store_prefix(const char * name, 
                             TableSpec *table, 
                             int cluster_id, 
                             char *cache_policy) {
  DEBUG_PRINT("%s", name);
  KeyPrefix prefix(name);
  prefix_info_t * info_ptr;
  
  info_ptr = (prefix_info_t *) genhash_find(policies_map, 
                                            cache_policy, strlen(cache_policy));
  if(info_ptr == 0) {  
    /* policy from key_prefixes doesn't exist in cache_policies */
    logger->log(LOG_WARNING, 0, "Invalid cache policy \"%s\" named in "
                "key prefix \"%s\"\n", cache_policy, name);
    return false;
  }
  
  memcpy(& prefix.info, info_ptr, sizeof(prefix_info_t));
  
  if(prefix.info.do_db_read || prefix.info.do_db_write
     || prefix.info.do_db_delete || prefix.info.do_db_flush) {
    prefix.info.use_ndb = 1;
    /* At least one of math_col or value_col is required.  */
    if((! table->math_column) && (! table->value_columns[0])) {
      logger->log(LOG_WARNING, 0, "Error at key prefix \"%s\": "
                  "No value container.\n");
      return false;
    }
    if(table->cas_column)   prefix.info.has_cas_col    = 1;
    if(table->math_column)  prefix.info.has_math_col   = 1;
    if(table->exp_column)   prefix.info.has_expire_col = 1;
    if(table->flags_column) prefix.info.has_flags_col  = 1;
  }
  else {    
    /* If the prefix does not use NDB, you cannot specify a container. */
    if(table != 0) {
      logger->log(LOG_WARNING, 0, "Error at key prefix \"%s\": "
                  "Cache policy \"%s\" does not use NDB, so container "
                  " must be null.\n", name, cache_policy);
      return false;
    }
  }
  
  int internal_cluster_idx = -1;
  
  if(prefix.info.use_ndb) {
    /* The cluster_id must refer to a known cluster: */
    for(int i = 0 ; i < nclusters ; i++) 
      if(cluster_ids[i] == cluster_id)
        internal_cluster_idx = i;
    
    if(internal_cluster_idx == -1) {
      logger->log(LOG_WARNING, 0, "Error at key prefix \"%s\": cluster_id %d "
                  "does not exist in ndb_clusters table.\n", 
                  name, cluster_id);
      return false;
    }
  }
  
  /* Tie it all together */
  prefix.info.cluster_id = internal_cluster_idx;
  prefix.table = table;
  prefix.info.usable = 1;
  
  /* Configuration::storePrefix() will make a copy of the KeyPrefix, 
   and fill in the prefix_id of the copy.
   */
  prefix.info.prefix_id = conf.storePrefix(prefix);
  
  
  return true;
}


/* log_signon() 
 UPDATE last_memcached_signon SET hostname=?, server_role=?, signon_time=?
 WHERE ndb_node_id = MY_NODE_ID.
 This has the side effect of providing us with the global checkpoint ID
 for server startup.
 */
void config_v1::log_signon() {
  DEBUG_ENTER_METHOD("config_v1::log_signon");
  char my_hostname[256];
  gethostname(my_hostname, 256);  
  Ndb db(conf.primary_conn);
  db.init(1);
  TableSpec spec("ndbmemcache.last_memcached_signon",
                 "ndb_node_id", "hostname,server_role,signon_time");
  QueryPlan plan(&db, &spec);
  
  Operation op(&plan, OPERATION_SET);
  op.buffer     = (char *) malloc(op.requiredBuffer());
  op.key_buffer = (char *) malloc(op.requiredKeyBuffer());
  op.setKeyPartInt(COL_STORE_KEY,   db.getNodeId());  // node ID (in key)
  op.setColumnInt(COL_STORE_KEY,    db.getNodeId());  // node ID (in row)
  op.setColumn(COL_STORE_VALUE+0,   my_hostname, strlen(my_hostname));           // hostname
  op.setColumn(COL_STORE_VALUE+1,   conf.server_role, strlen(conf.server_role)); // role
  op.setColumnInt(COL_STORE_VALUE+2,time(NULL));                                 // timestamp
  
  NdbTransaction *tx = db.startTransaction();   // TODO: node selection
  op.writeTuple(tx);
  tx->execute(NdbTransaction::Commit);
  tx->getGCI(&signon_gci);

  tx->close();  
  free(op.key_buffer);
  free(op.buffer);
  return;
}


/* set_initial_cas():  
 Create an initial value for the cas_unique sequence. 
 Use the latest GCI (obtained when signing on) and the NDB node id.
 TODO: This scheme probably spends too many bits on an unchanging initial GCI
 leaving not enough bits for the counter.
 */
void config_v1::set_initial_cas() {
  /*  ---------------------------------------------------------------- 
   |   27 bits Initial GCI    | eng bit|     28 bits counter        |
   |                          | + 8bit |                            | 
   |                          | NodeId |                            |
   ----------------------------------------------------------------   */
  Ndb db(conf.primary_conn);
  db.init(1);
  const uint64_t MASK_GCI   = 0x07FFFFFF00000000LLU; // Use these 27 bits of GCI 
  const uint64_t ENGINE_BIT = 0x0000001000000000LLU; // bit 36
  
  uint64_t node_id = ((uint64_t) db.getNodeId()) << 28;  
  uint64_t gci_bits = (signon_gci & MASK_GCI) << 5;
  uint64_t def_eng_cas = gci_bits | node_id;
  uint64_t ndb_eng_cas = gci_bits | ENGINE_BIT | node_id;
  
  //  void storeCAS(uint64_t ndb_engine_cas, uint64_t default_engine_cas);
  conf.storeCAS(ndb_eng_cas, def_eng_cas);
  DEBUG_PRINT("Sign On GCI: 0x%llx | Node Id: [%d] 0x%llx | Engine bit: 0x%llx", 
              signon_gci, db.getNodeId(), node_id, ENGINE_BIT);
  DEBUG_PRINT("Initial CAS: %llu 0x%llx ", ndb_eng_cas, ndb_eng_cas);
  
  return;
}


/***************** VERSION 1.0 ****************/
void config_v1_0::minor_version_config() {
  conf.onlineReloadFlag = 0;
  conf.reload_waiter = 0;
}


/***************** VERSION 1.1 ****************/
int server_roles_reload_waiter(Ndb_cluster_connection *, const char *);
void config_v1_1::minor_version_config() {
  conf.onlineReloadFlag = 1;
  conf.reload_waiter = server_roles_reload_waiter;
}


/******** RELOAD WAITER ON ndbmemcache.memcache_server_roles *********/
int create_event(NdbDictionary::Dictionary *dict, const char *event_name) {
  DEBUG_ENTER();
  const NdbDictionary::Table *tab = dict->getTable("memcache_server_roles");
  if(tab == 0) {
    DEBUG_PRINT("getTable(): %s", dict->getNdbError().message);
    return -1;
  }
  
  NdbDictionary::Event event(event_name, *tab);
  event.addTableEvent(NdbDictionary::Event::TE_UPDATE);
  event.addEventColumn("update_timestamp");
  if(dict->createEvent(event) != 0) {
    DEBUG_PRINT("createEvent(): %s", dict->getNdbError().message);
    return -1;
  }

  return 0;
}


int server_roles_reload_waiter(Ndb_cluster_connection *conn,
                               const char *server_role) {
  DEBUG_ENTER();
  const char * event_name = "MEMCACHE$conf_reload_v1.1";
  Ndb db(conn, "ndbmemcache");
  db.init(4);
  NdbDictionary::Dictionary *dict = db.getDictionary();

  const NdbDictionary::Event * stored_event = dict->getEvent(event_name);
  if(stored_event == 0) {
    if(create_event(dict, event_name) != 0) {
      return -1;
    }
  }
    
  NdbEventOperation *wait_op = db.createEventOperation(event_name);
  if(wait_op == 0) { // error
    DEBUG_PRINT("createEventOperation(): %s", db.getNdbError().message);
    return -1;
  }

  /* Create RecAttrs for the PK and the timestamp */
  NdbRecAttr *recattr1 = wait_op->getValue("role_name");
  NdbRecAttr *recattr2 = wait_op->getPreValue("role_name");
  NdbRecAttr *recattr3 = wait_op->getValue("update_timestamp");
  NdbRecAttr *recattr4 = wait_op->getPreValue("update_timestamp");
  assert(recattr1 && recattr2 && recattr3 && recattr4);
  
  if(wait_op->execute() != 0) {
    DEBUG_PRINT("execute(): %s", wait_op->getNdbError().message);
    return -1;
  }

  while(1) {
    // TODO: if conf.shutdown return 0.
    int waiting = db.pollEvents(1000, 0);

    if(waiting < 0) {
      /* error */
      db.dropEventOperation(wait_op);
      return -1;
    }
    else if(waiting > 0) {
      DEBUG_PRINT("%d waiting", waiting);
      if(db.nextEvent()) { 
        if(recattr1->isNULL() == 0) {
          uint role_name_len = *(const unsigned char*) recattr1->aRef();
          char *role_name = recattr1->aRef() + 1;
          if(role_name_len == strlen(server_role) && 
             strcmp(server_role, role_name) == 0) { 
            /* Time to reconfigure! */
            logger->log(LOG_WARNING, 0, "Received update to server role %s", role_name);
            db.dropEventOperation(wait_op);
            return 1;
          }
          else DEBUG_PRINT("Got update event for %s, but that aint me.", role_name);
        }
        else DEBUG_PRINT("isNULL(): %d", recattr1->isNULL());      
      }
      else DEBUG_PRINT("Spurious event");
    }
    else {
      /* 0 = timeout.  just wait again. */
    }
  }
}

/***************** VERSION 1.2 ****************/
void config_v1_2::minor_version_config() {
  conf.onlineReloadFlag = 1;
  conf.reload_waiter = server_roles_reload_waiter;
}


TableSpec * config_v1_2::get_container_record(char *name) {
  TableSpec * cont = config_v1::get_container_record(name);
  if(cont) {
    Ndb db(conf.primary_conn);
    db.init(1);
    TableSpec spec("ndbmemcache.containers", "name", "large_values_table");
    QueryPlan plan(&db, &spec);
    Operation op(&plan, OP_READ);
    
    op.key_buffer = (char *) malloc(op.requiredKeyBuffer());
    op.buffer     = (char *) malloc(op.requiredBuffer());
    NdbTransaction *tx = db.startTransaction();
    
    op.clearKeyNullBits();
    op.setKeyPart(COL_STORE_KEY, name, strlen(name));
    op.readTuple(tx);
    tx->execute(NdbTransaction::Commit);
    
    if(tx->getNdbError().classification == NdbError::NoError) {
      char val[256];
      if(! op.isNull(COL_STORE_VALUE + 0)) {
        op.copyValue(COL_STORE_VALUE + 0, val);
        cont->external_table = ExternalValue::createContainerRecord(val);
      }
    }

    tx->close();
    free(op.key_buffer);
    free(op.buffer);
  }
  return cont;
}  

