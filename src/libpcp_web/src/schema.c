/*
 * Copyright (c) 2017-2021 Red Hat.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */
#include <assert.h>
#include "pmapi.h"
#include "pmda.h"
#include "search.h"
#include "schema.h"
#include "discover.h"
#include "util.h"
#include "sha1.h"

#define STRINGIFY(s)	#s
#define TO_STRING(s)	STRINGIFY(s)
#define SERIES_VERSION	2
#define SERVER_VERSION	5

extern sds		cursorcount;
static sds		maxstreamlen;
static sds		streamexpire;

typedef struct redisScript {
    sds			hash;
    const char		*text;
} redisScript;

static redisScript	*scripts;
static int		nscripts;

static void
redisScriptsInit(void)
{
    const unsigned char	*text;
    unsigned char	hash[20];
    char		hashbuf[42];
    redisScript		*script;
    SHA1_CTX		shactx;
    int			i;

    for (i = 0; i < nscripts; i++) {
	script = &scripts[i];
	text = (const unsigned char *)script->text;

	/* Calculate unique script identifier from its contents */
	SHA1Init(&shactx);
	SHA1Update(&shactx, text, strlen((char *)text));
	SHA1Final(hash, &shactx);
	pmwebapi_hash_str(hash, hashbuf, sizeof(hashbuf));
	scripts->hash = sdsnew(hashbuf);
    }
}

static void
initRedisSlotsBaton(redisSlotsBaton *baton,
		redisInfoCallBack info, redisDoneCallBack done,
		void *userdata, void *events, void *arg)
{
    initSeriesBatonMagic(baton, MAGIC_SLOTS);
    baton->info = info;
    baton->done = done;
    baton->version = -1;
    baton->userdata = userdata;
    baton->arg = arg;
}

static void
redis_slots_finished(void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_slots_finished");

    if (baton->error == 0)
	baton->done(baton->arg);
    memset(baton, 0, sizeof(*baton));
    free(baton);
}

void
redis_slots_end_phase(void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_slots_end_phase");

    if (baton->error == 0) {
	seriesPassBaton(&baton->current, baton, "redis_slots_end_phase");
    } else {	/* fail after waiting on outstanding I/O */
	if (seriesBatonDereference(baton, "redis_slots_end_phase"))
	    redis_slots_finished(baton);
    }
}

static void
initRedisMapBaton(redisMapBaton *baton, redisSlots *slots,
	redisMap *mapping, sds mapKey, sds mapStr,
	redisDoneCallBack on_done, redisInfoCallBack on_info,
	void *userdata, void *arg)
{
    initSeriesBatonMagic(baton, MAGIC_MAPPING);
    baton->mapping = mapping;
    baton->mapKey = mapKey;
    baton->mapStr = mapStr;
    baton->slots = slots;
    baton->info = on_info;
    baton->mapped = on_done;
    baton->userdata = userdata;
    baton->arg = arg;
}

static void
doneRedisMapBaton(redisMapBaton *baton)
{
    seriesBatonCheckMagic(baton, MAGIC_MAPPING, "doneRedisMapBaton");
    if (baton->mapped)
	baton->mapped(baton->arg);
    sdsfree(baton->mapKey);
    memset(baton, 0, sizeof(*baton));
    free(baton);
}

static void
redis_map_publish_callback(
	redisAsyncContext *redis, redisReply *reply, const sds cmd, void *arg)
{
    redisMapBaton	*baton = (redisMapBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_MAPPING, "redis_map_publish_callback");
    /* no cluster redirection checking is needed for this callback */
    sdsfree(cmd);

    checkIntegerReply(baton->info, baton->userdata, reply,
			"%s: %s", PUBLISH, "new %s mapping",
			redisMapName(baton->mapping));
    doneRedisMapBaton(baton);
}

static void
redis_map_publish(redisMapBaton *baton)
{
    char		hash[42];
    sds			msg, key, cmd;

    pmwebapi_hash_str((unsigned char *)baton->mapKey, hash, sizeof(hash));
    msg = sdscatfmt(sdsempty(), "%S:%S", hash, baton->mapStr);
    key = sdscatfmt(sdsempty(), "pcp:channel:%s", redisMapName(baton->mapping));
    cmd = redis_command(3);
    cmd = redis_param_str(cmd, PUBLISH, PUBLISH_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sds(cmd, msg);
    sdsfree(msg);

    redisSlotsRequest(baton->slots, PUBLISH, key, cmd,
			redis_map_publish_callback, baton);
}

static void
redis_map_request_callback(
	redisAsyncContext *redis, redisReply *reply, const sds cmd, void *arg)
{
    redisMapBaton	*baton = (redisMapBaton *)arg;
    int			newname, sts;

    seriesBatonCheckMagic(baton, MAGIC_MAPPING, "redis_map_request_callback");
    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
			     cmd, redis_map_request_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */

    if (sts == 0)
	newname = checkIntegerReply(baton->info, baton->userdata, reply,
			"%s: %s (%s)", HSET, "string mapping script",
			redisMapName(baton->mapping));
    else
	newname = sts;

    /* publish any newly created name mapping */
    if (newname > 0)
	redis_map_publish(baton);
    else
	doneRedisMapBaton(baton);
}

void
redisMapRequest(redisMapBaton *baton, redisMap *map, sds name, sds value)
{
    sds			cmd, key;

    key = sdscatfmt(sdsempty(), "pcp:map:%s", redisMapName(baton->mapping));
    cmd = redis_command(4);
    cmd = redis_param_str(cmd, HSET, HSET_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sds(cmd, name);
    cmd = redis_param_sds(cmd, value);

    redisSlotsRequest(baton->slots, HSET, key, cmd, redis_map_request_callback, baton);
}

void
redisGetMap(redisSlots *slots, redisMap *mapping, unsigned char *hash, sds mapStr,
		redisDoneCallBack on_done, redisInfoCallBack on_info,
		void *userdata, void *arg)
{
    redisMapBaton	*baton;
    redisMapEntry	*entry;
    sds			mapKey;

    pmwebapi_string_hash(hash, mapStr, sdslen(mapStr));
    mapKey = sdsnewlen(hash, 20);

    if ((entry = redisMapLookup(mapping, mapKey)) != NULL) {
	sdsfree(mapKey);
	on_done(arg);
    } else {
	/*
	 * This string is not cached locally; so we always send it to server;
	 * it may or may not exist there yet, we must just make sure it does.
	 * The caller does not need to wait as we provide the calculated hash
	 * straight away.
	 */
	if ((baton = calloc(1, sizeof(redisMapBaton))) != NULL) {
	    initRedisMapBaton(baton, slots, mapping, mapKey, mapStr,
			    on_done, on_info, userdata, arg);
	    redisMapInsert(mapping, mapKey, sdsdup(mapStr));
	    redisMapRequest(baton, mapping, mapKey, mapStr);
	} else {
	    on_done(arg);
	}
    }
}

static void
redis_source_context_name(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
			     cmd, redis_source_context_name, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkIntegerReply(baton->info, baton->userdata, reply,
		"%s: %s", SADD, "mapping context to source or host name");
    doneSeriesLoadBaton(baton, "redis_source_context_name");
}

static void
redis_source_location(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
			     cmd, redis_source_location, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkIntegerReply(baton->info, baton->userdata, reply,
		"%s: %s", GEOADD, "mapping source location");
    doneSeriesLoadBaton(baton, "redis_source_location");
}

static void
redis_context_name_source(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
			     cmd, redis_context_name_source, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkIntegerReply(baton->info, baton->userdata, reply,
		"%s: %s", SADD, "mapping source names to context");
    doneSeriesLoadBaton(baton, "redis_context_name_source");
}

void
redis_series_source(redisSlots *slots, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    context_t			*context = seriesLoadBatonContext(baton);
    char			hashbuf[42];
    sds				cmd, key, val, val2;

    /* Async recipe:
     * . SADD pcp:source:context.name:<id>
     * . SADD pcp:context.name:source:<hash>
     * . SADD pcp:source:context.name:<hostid>
     * . GEOADD pcp:source:location <lat> <long> <hash>
     */
    seriesBatonReferences(baton, 4, "redis_series_source");

    pmwebapi_hash_str(context->name.id, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "pcp:source:context.name:%s", hashbuf);
    cmd = redis_command(3);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sha(cmd, context->name.hash);
    redisSlotsRequest(slots, SADD, key, cmd, redis_source_context_name, arg);

    pmwebapi_hash_str(context->hostid, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "pcp:source:context.name:%s", hashbuf);
    cmd = redis_command(3);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sha(cmd, context->name.hash);
    redisSlotsRequest(slots, SADD, key, cmd, redis_source_context_name, arg);

    pmwebapi_hash_str(context->name.hash, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "pcp:context.name:source:%s", hashbuf);
    cmd = redis_command(4);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sha(cmd, context->name.id);
    cmd = redis_param_sha(cmd, context->hostid);
    redisSlotsRequest(slots, SADD, key, cmd, redis_context_name_source, arg);

    key = sdsnew("pcp:source:location");
    val = sdscatprintf(sdsempty(), "%.8f", context->location[0]);
    val2 = sdscatprintf(sdsempty(), "%.8f", context->location[1]);
    cmd = redis_command(5);
    cmd = redis_param_str(cmd, GEOADD, GEOADD_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sds(cmd, val2);
    cmd = redis_param_sds(cmd, val);
    cmd = redis_param_sha(cmd, context->name.hash);
    sdsfree(val2);
    sdsfree(val);
    redisSlotsRequest(slots, GEOADD, key, cmd, redis_source_location, arg);
}

static void
redis_series_inst_name_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
			     cmd, redis_series_inst_name_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkIntegerReply(baton->info, baton->userdata, reply,
		"%s: %s", SADD, "mapping series to inst name");
    doneSeriesLoadBaton(baton, "redis_series_inst_name_callback");
}

static void
redis_instances_series_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
			     cmd, redis_instances_series_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkIntegerReply(baton->info, baton->userdata, reply,
		"%s: %s", SADD, "mapping instance to series");
    doneSeriesLoadBaton(baton, "redis_instances_series_callback");
}

static void
redis_series_inst_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
			     cmd, redis_series_inst_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkStatusReplyOK(baton->info, baton->userdata, reply,
		"%s: %s", HMSET, "setting metric inst");
    doneSeriesLoadBaton(baton, "redis_series_inst_callback");
}

void
redis_series_instance(redisSlots *slots, metric_t *metric, instance_t *instance, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    char			mhashbuf[42], hashbuf[42];
    sds				cmd, key, val;
    int				i;

    seriesBatonCheckMagic(baton, MAGIC_LOAD, "redis_series_instance");
    seriesBatonReferences(baton, 2, "redis_series_instance");

    assert(instance->name.sds);
    pmwebapi_hash_str(instance->name.id, hashbuf, sizeof(hashbuf));

    if (pmDebugOptions.series) {
	pmwebapi_hash_str(metric->names[0].id, mhashbuf, sizeof(mhashbuf));
	fprintf(stderr, "%s: loading inst name %s [%s] for metric %s [%s]\n",
		"redis_series_instance", instance->name.sds, hashbuf,
			metric->names[0].sds, mhashbuf);
    }

    key = sdscatfmt(sdsempty(), "pcp:series:inst.name:%s", hashbuf);
    cmd = redis_command(2 + metric->numnames);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    for (i = 0; i < metric->numnames; i++)
	cmd = redis_param_sha(cmd, metric->names[i].hash);
    redisSlotsRequest(slots, SADD, key, cmd, redis_series_inst_name_callback, arg);

    for (i = 0; i < metric->numnames; i++) {
	seriesBatonReference(baton, "redis_series_instance");
	pmwebapi_hash_str(metric->names[i].hash, hashbuf, sizeof(hashbuf));
	key = sdscatfmt(sdsempty(), "pcp:instances:series:%s", hashbuf);
	cmd = redis_command(3);
	cmd = redis_param_str(cmd, SADD, SADD_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sha(cmd, instance->name.hash);
	redisSlotsRequest(slots, SADD, key, cmd, redis_instances_series_callback, arg);
    }

    pmwebapi_hash_str(instance->name.hash, hashbuf, sizeof(hashbuf));
    val = sdscatfmt(sdsempty(), "%i", instance->inst);
    key = sdscatfmt(sdsempty(), "pcp:inst:series:%s", hashbuf);
    cmd = redis_command(8);
    cmd = redis_param_str(cmd, HMSET, HMSET_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_str(cmd, "inst", sizeof("inst")-1);
    cmd = redis_param_sds(cmd, val);
    cmd = redis_param_str(cmd, "name", sizeof("name")-1);
    cmd = redis_param_sha(cmd, instance->name.id);
    cmd = redis_param_str(cmd, "source", sizeof("series")-1);
    cmd = redis_param_sha(cmd, metric->indom->domain->context->name.hash);
    sdsfree(val);
    redisSlotsRequest(slots, HMSET, key, cmd, redis_series_inst_callback, arg);
}

static void
label_value_mapping_callback(void *arg)
{
    labellist_t			*list = (labellist_t *)arg;
    seriesLoadBaton		*baton = (seriesLoadBaton *)list->arg;

    seriesBatonCheckMagic(baton, MAGIC_LOAD, "label_value_mapping_callback");
    redisMapRelease(list->valuemap);
    list->valuemap = NULL;
    doneSeriesLoadBaton(baton, "label_value_mapping_callback");
}

static void
label_name_mapping_callback(void *arg)
{
    labellist_t			*list = (labellist_t *)arg;
    seriesLoadBaton		*baton = (seriesLoadBaton *)list->arg;

    seriesBatonCheckMagic(baton, MAGIC_LOAD, "label_name_mapping_callback");
    doneSeriesLoadBaton(baton, "label_name_mapping_callback");
}

typedef struct seriesAnnotateClosure {
    struct seriesLoadBaton	*load;
    metric_t			*metric;
    instance_t			*instance;
} seriesAnnotateClosure;

static int
annotate_metric(const pmLabel *label, const char *json, void *arg)
{
    seriesAnnotateClosure	*closure = (seriesAnnotateClosure *)arg;
    seriesLoadBaton		*baton = closure->load;
    labellist_t			*list;
    instance_t			*instance = closure->instance;
    metric_t			*metric = closure->metric;
    char			hashbuf[42];
    sds				key;

    seriesBatonCheckMagic(baton, MAGIC_LOAD, "annotate_metric");

    /* check if this label is already in the list */
    list = instance ? instance->labellist : metric->labellist;
    while (list) {
	if (label->namelen == sdslen(list->name) &&
	    strncmp(list->name, json + label->name, label->namelen) == 0)
	    return 0;	/* short-circuit */
	list = list->next;
    }

    /*
     * TODO: decode complex values ('{...}' and '[...]'),
     * using a dot-separated name for these maps, and names
     * with explicit array index suffix for array entries.
     */

    if ((list = (labellist_t *)calloc(1, sizeof(labellist_t))) == NULL)
	return -ENOMEM;

    list->arg = baton;
    list->name = sdsnewlen(json + label->name, label->namelen);
    list->value = sdsnewlen(json + label->value, label->valuelen);
    list->flags = label->flags;

    if (pmDebugOptions.series) {
	fprintf(stderr, "Annotate metric %s", metric->names[0].sds);
	if (instance)
	    fprintf(stderr, "[%s]", instance->name.sds);
	fprintf(stderr, " label %s=%s (flags=0x%x)\n",
			list->name, list->value, list->flags);
    }

    /* prepend map onto the list for this metric or instance */
    if (instance) {
	if (instance->labellist)
	    list->next = instance->labellist;
	instance->labellist = list;
    } else {
	if (metric->labellist)
	    list->next = metric->labellist;
	metric->labellist = list;
    }

    seriesBatonReferences(baton, 2, "annotate_metric");

    redisGetMap(baton->slots,
		labelsmap, list->nameid, list->name,
		label_name_mapping_callback,
		baton->info, baton->userdata, (void *)list);

    pmwebapi_hash_str(list->nameid, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "label.%s.value", hashbuf);
    list->valuemap = redisMapCreate(key);

    redisGetMap(baton->slots,
		list->valuemap, list->valueid, list->value,
		label_value_mapping_callback,
		baton->info, baton->userdata, (void *)list);

    return 0;
}

static void
redis_series_labelvalue_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(load->slots, reply, load->info, load->userdata,
			     cmd, redis_series_labelvalue_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkStatusReplyOK(load->info, load->userdata, reply,
		"%s: %s", HMSET, "setting series label value");
    doneSeriesLoadBaton(arg, "redis_series_labelvalue_callback");
}

static void
redis_series_maplabelvalue_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(load->slots, reply, load->info, load->userdata,
			     cmd, redis_series_maplabelvalue_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkStatusReplyOK(load->info, load->userdata, reply,
		"%s: %s", HMSET, "setting series map label value");
    doneSeriesLoadBaton(arg, "redis_series_maplabelvalue_callback");
}


static void
redis_series_labelflags_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(load->slots, reply, load->info, load->userdata,
			     cmd, redis_series_labelflags_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkStatusReplyOK(load->info, load->userdata, reply,
		"%s: %s", HMSET, "setting series label flags");
    doneSeriesLoadBaton(arg, "redis_series_labelflags_callback");
}

static void
redis_series_label_set_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(load->slots, reply, load->info, load->userdata,
			     cmd, redis_series_label_set_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkIntegerReply(load->info, load->userdata, reply,
		"%s %s", SADD, "pcp:series:label.X.value:Y");
    doneSeriesLoadBaton(arg, "redis_series_label_set_callback");
}

static void
redis_series_label(redisSlots *slots, metric_t *metric, char *hash,
		labellist_t *list, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    char			namehash[42], valhash[42];
    sds				cmd, key, val;
    int				i;

    seriesBatonReferences(baton, 3, "redis_series_label");

    if (list->flags != PM_LABEL_CONTEXT) {
	seriesBatonReference(baton, "redis_series_label");

	val = sdscatfmt(sdsempty(), "%I", list->flags);
	key = sdscatfmt(sdsempty(), "pcp:labelflags:series:%s", hash);
	cmd = redis_command(4);
	cmd = redis_param_str(cmd, HMSET, HMSET_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sha(cmd, list->nameid);
	cmd = redis_param_sds(cmd, val);
	sdsfree(val);
	redisSlotsRequest(slots, HMSET, key, cmd,
				redis_series_labelflags_callback, arg);
    }

    key = sdscatfmt(sdsempty(), "pcp:labelvalue:series:%s", hash);
    cmd = redis_command(4);
    cmd = redis_param_str(cmd, HMSET, HMSET_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sha(cmd, list->nameid);
    cmd = redis_param_sha(cmd, list->valueid);
    redisSlotsRequest(slots, HMSET, key, cmd,
			redis_series_labelvalue_callback, arg);

    pmwebapi_hash_str(list->nameid, namehash, sizeof(namehash));
    pmwebapi_hash_str(list->valueid, valhash, sizeof(valhash));

    key = sdscatfmt(sdsempty(), "pcp:map:label.%s.value", namehash);
    cmd = redis_command(4);
    cmd = redis_param_str(cmd, HMSET, HMSET_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sha(cmd, list->valueid);
    cmd = redis_param_sds(cmd, list->value);
    redisSlotsRequest(slots, HMSET, key, cmd,
			redis_series_maplabelvalue_callback, arg);

    key = sdscatfmt(sdsempty(), "pcp:series:label.%s.value:%s",
		    namehash, valhash);
    cmd = redis_command(2 + metric->numnames);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    for (i = 0; i < metric->numnames; i++)
	cmd = redis_param_sha(cmd, metric->names[i].hash);
    redisSlotsRequest(slots, SADD, key, cmd,
			redis_series_label_set_callback, arg);
}

static void
redis_series_labelset(redisSlots *slots, metric_t *metric, instance_t *instance, void *arg)
{
    labellist_t			*list;
    char			hashbuf[42];
    int				i;

    if (instance != NULL) {
	pmwebapi_hash_str(instance->name.hash, hashbuf, sizeof(hashbuf));
	list = instance->labellist;
	do {
	    redis_series_label(slots, metric, hashbuf, list, arg);
	} while ((list = list->next) != NULL);
    } else {
	for (i = 0; i < metric->numnames; i++) {
	    pmwebapi_hash_str(metric->names[i].hash, hashbuf, sizeof(hashbuf));
	    list = metric->labellist;
	    do {
		redis_series_label(slots, metric, hashbuf, list, arg);
	    } while ((list = list->next) != NULL);
	}
    }
}

static void
series_label_mapping_fail(seriesname_t *series, int sts, seriesLoadBaton *baton)
{
    char			pmmsg[PM_MAXERRMSGLEN];
    char			hashbuf[42];
    sds				msg;

    pmwebapi_hash_str(series->hash, hashbuf, sizeof(hashbuf));
    infofmt(msg, "Cannot merge metric %s [%s] label set: %s", hashbuf,
		series->sds, pmErrStr_r(sts, pmmsg, sizeof(pmmsg)));
    batoninfo(baton, PMLOG_ERROR, msg);
}

void
series_metric_label_mapping(metric_t *metric, seriesLoadBaton *baton)
{
    seriesAnnotateClosure	closure = { baton, metric, NULL };
    char			buf[PM_MAXLABELJSONLEN];
    int				sts;

    if ((sts = metric_labelsets(metric, buf, sizeof(buf),
				    annotate_metric, &closure)) < 0)
	series_label_mapping_fail(&metric->names[0], sts, baton);
}

void
series_instance_label_mapping(metric_t *metric, instance_t *instance,
				seriesLoadBaton *baton)
{
    seriesAnnotateClosure	closure = { baton, metric, instance };
    char			buf[PM_MAXLABELJSONLEN];
    int				sts;

    if ((sts = instance_labelsets(metric->indom, instance, buf, sizeof(buf),
				  annotate_metric, &closure)) < 0)
	series_label_mapping_fail(&instance->name, sts, baton);
}

static void
series_name_mapping_callback(void *arg)
{
    seriesBatonCheckMagic(arg, MAGIC_LOAD, "series_name_mapping_callback");
    doneSeriesLoadBaton(arg, "series_name_mapping_callback");
}

static void redis_series_metadata(context_t *, metric_t *, void *);
static void redis_series_streamed(sds, metric_t *, void *);

void
redis_series_metric(redisSlots *slots, metric_t *metric,
		sds timestamp, int meta, int data, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    instance_t			*instance;
    value_t			*value;
    int				i;

    /*
     * First satisfy any/all mappings for metric name, instance
     * names, label names and values.  This may issue updates to
     * cache (new) strings.  Then we can issue all (new) metadata
     * and data simultaneously afterward.
     */

    /* ensure all metric name strings are mapped */
    for (i = 0; metric->cached == 0 && i < metric->numnames; i++) {
	assert(metric->names[i].sds != NULL);
	seriesBatonReference(baton, "redis_series_metric");
	redisGetMap(slots,
		    namesmap, metric->names[i].id, metric->names[i].sds,
		    series_name_mapping_callback,
		    baton->info, baton->userdata, baton);
    }

    /* ensure all metric or instance label strings are mapped */
    if (metric->desc.indom == PM_INDOM_NULL || metric->u.vlist == NULL) {
	if (metric->cached == 0)
	    series_metric_label_mapping(metric, baton);
    } else {
	for (i = 0; i < metric->u.vlist->listcount; i++) {
	    value = &metric->u.vlist->value[i];
	    if ((instance = dictFetchValue(metric->indom->insts, &value->inst)) == NULL) {
		if (pmDebugOptions.series)
		    fprintf(stderr, "indom lookup failure for %s instance %u",
				pmInDomStr(metric->indom->indom), value->inst);
		continue;
	    }
	    assert(instance->name.sds != NULL);
	    seriesBatonReference(baton, "redis_series_metric");
	    redisGetMap(slots,
			instmap, instance->name.id, instance->name.sds,
			series_name_mapping_callback,
			baton->info, baton->userdata, baton);

	    if (instance->cached == 0)
		series_instance_label_mapping(metric, instance, baton);
	}
    }

    /* push the metric, instances and any label metadata into the cache */
    if (meta || data)
	redis_series_metadata(&baton->pmapi.context, metric, baton);

    /* push values for all instances, no-value or errors into the cache */
    if (data)
	redis_series_streamed(timestamp, metric, baton);
}

static void
redis_metric_name_series_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(load->slots, reply, load->info, load->userdata,
			     cmd, redis_metric_name_series_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkIntegerReply(load->info, load->userdata, reply,
			"%s %s", SADD, "map metric name to series");
    doneSeriesLoadBaton(arg, "redis_metric_name_series_callback");
}

static void
redis_series_metric_name_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(load->slots, reply, load->info, load->userdata,
			     cmd, redis_series_metric_name_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkIntegerReply(load->info, load->userdata, reply,
			"%s: %s", SADD, "map series to metric name");
    doneSeriesLoadBaton(arg, "redis_series_metric_name_callback");
}

static void
redis_desc_series_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(load->slots, reply, load->info, load->userdata,
			     cmd, redis_desc_series_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkStatusReplyOK(load->info, load->userdata, reply,
			"%s: %s", HMSET, "setting metric desc");
    doneSeriesLoadBaton(arg, "redis_desc_series_callback");
}

static void
redis_series_source_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;
    int				sts;

    sts = redisSlotsRedirect(load->slots, reply, load->info, load->userdata,
			     cmd, redis_series_source_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkIntegerReply(load->info, load->userdata, reply,
			"%s: %s", SADD, "mapping series to context");
    doneSeriesLoadBaton(arg, "redis_series_source_callback");
}

static void
redis_series_metadata(context_t *context, metric_t *metric, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    redisSlots			*slots = baton->slots;
    instance_t			*instance;
    value_t			*value;
    const char			*units, *indom = NULL, *pmid, *sem, *type;
    char			ibuf[32], pbuf[32], sbuf[20], tbuf[20], ubuf[60];
    char			hashbuf[42];
    sds				cmd, key;
    int				i;

    if (metric->cached)
	goto check_instances;

    indom = pmwebapi_indom_str(metric, ibuf, sizeof(ibuf));
    pmid = pmwebapi_pmid_str(metric, pbuf, sizeof(pbuf));
    sem = pmwebapi_semantics_str(metric, sbuf, sizeof(sbuf));
    type = pmwebapi_type_str(metric, tbuf, sizeof(tbuf));
    units = pmwebapi_units_str(metric, ubuf, sizeof(ubuf));

    for (i = 0; i < metric->numnames; i++) {
	assert(metric->names[i].sds != NULL);

	seriesBatonReferences(baton, 3, "redis_series_metadata names");

	pmwebapi_hash_str(metric->names[i].id, hashbuf, sizeof(hashbuf));
	key = sdscatfmt(sdsempty(), "pcp:series:metric.name:%s", hashbuf);
	cmd = redis_command(3);
	cmd = redis_param_str(cmd, SADD, SADD_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sha(cmd, metric->names[i].hash);
	redisSlotsRequest(slots, SADD, key, cmd,
			redis_series_metric_name_callback, arg);

	pmwebapi_hash_str(metric->names[i].hash, hashbuf, sizeof(hashbuf));
	key = sdscatfmt(sdsempty(), "pcp:metric.name:series:%s", hashbuf);
	cmd = redis_command(3);
	cmd = redis_param_str(cmd, SADD, SADD_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sha(cmd, metric->names[i].id);
	redisSlotsRequest(slots, SADD, key, cmd,
			redis_metric_name_series_callback, arg);

	key = sdscatfmt(sdsempty(), "pcp:desc:series:%s", hashbuf);
	cmd = redis_command(14);
	cmd = redis_param_str(cmd, HMSET, HMSET_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_str(cmd, "indom", sizeof("indom")-1);
	cmd = redis_param_str(cmd, indom, strlen(indom));
	cmd = redis_param_str(cmd, "pmid", sizeof("pmid")-1);
	cmd = redis_param_str(cmd, pmid, strlen(pmid));
	cmd = redis_param_str(cmd, "semantics", sizeof("semantics")-1);
	cmd = redis_param_str(cmd, sem, strlen(sem));
	cmd = redis_param_str(cmd, "source", sizeof("source")-1);
	cmd = redis_param_sha(cmd, context->name.hash);
	cmd = redis_param_str(cmd, "type", sizeof("type")-1);
	cmd = redis_param_str(cmd, type, strlen(type));
	cmd = redis_param_str(cmd, "units", sizeof("units")-1);
	cmd = redis_param_str(cmd, units, strlen(units));
	redisSlotsRequest(slots, HMSET, key, cmd, redis_desc_series_callback, arg);

	if ((baton->flags & PM_SERIES_FLAG_TEXT) && slots->search)
	    redis_search_text_add(slots, PM_SEARCH_TYPE_METRIC,
				metric->names[i].sds, indom,
				metric->oneline, metric->helptext, baton);
    }

    seriesBatonReference(baton, "redis_series_metadata");

    pmwebapi_hash_str(context->name.id, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "pcp:series:context.name:%s", hashbuf);
    cmd = redis_command(2 + metric->numnames);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    for (i = 0; i < metric->numnames; i++)
	cmd = redis_param_sha(cmd, metric->names[i].hash);
    redisSlotsRequest(slots, SADD, key, cmd, redis_series_source_callback, arg);

check_instances:
    if (metric->desc.indom != PM_INDOM_NULL &&
        (baton->flags & PM_SERIES_FLAG_TEXT) && slots->search) {
	if (indom == NULL)
	    indom = pmwebapi_indom_str(metric, ibuf, sizeof(ibuf));
	redis_search_text_add(slots, PM_SEARCH_TYPE_INDOM, indom, indom,
			metric->indom->oneline, metric->indom->helptext, baton);
    }

    if (metric->desc.indom == PM_INDOM_NULL || metric->u.vlist == NULL) {
	if (metric->cached == 0) {
	    redis_series_labelset(slots, metric, NULL, baton);
	    metric->cached = 1;
	}
    } else {
	for (i = 0; i < metric->u.vlist->listcount; i++) {
	    value = &metric->u.vlist->value[i];
	    if ((instance = dictFetchValue(metric->indom->insts, &value->inst)) == NULL)
		continue;
	    if (instance->cached == 0 || metric->cached == 0) {
		redis_series_instance(slots, metric, instance, baton);
		redis_series_labelset(slots, metric, instance, baton);

		if ((baton->flags & PM_SERIES_FLAG_TEXT) && slots->search) {
		    if (indom == NULL)
			indom = pmwebapi_indom_str(metric, ibuf, sizeof(ibuf));
		    redis_search_text_add(slots, PM_SEARCH_TYPE_INST,
				instance->name.sds, indom, NULL, NULL, baton);
		}
	    }
	    instance->cached = 1;
	}
	metric->cached = 1;
    }
}

typedef struct redisStreamBaton {
    seriesBatonMagic	header;
    redisSlots		*slots;
    sds			stamp;
    char		hash[40+1];
    redisInfoCallBack   info;
    void		*userdata;
    void		*arg;
} redisStreamBaton;

static void
initRedisStreamBaton(redisStreamBaton *baton, redisSlots *slots,
		sds stamp, const char *hash, seriesLoadBaton *load)
{
    initSeriesBatonMagic(baton, MAGIC_STREAM);
    baton->slots = slots;
    baton->stamp = sdsdup(stamp);
    memcpy(baton->hash, hash, sizeof(baton->hash));
    baton->info = load->info;
    baton->userdata = load->userdata;
    baton->arg = load;
}

static void
doneRedisStreamBaton(redisStreamBaton *baton)
{
    void		*load = baton->arg;

    seriesBatonCheckMagic(baton, MAGIC_STREAM, "doneRedisStreamBaton");
    seriesBatonCheckMagic(load, MAGIC_LOAD, "doneRedisStreamBaton");
    sdsfree(baton->stamp);
    memset(baton, 0, sizeof(*baton));
    free(baton);

    doneSeriesLoadBaton(load, "doneRedisStreamBaton");
}

static sds
series_stream_append(sds cmd, sds name, sds value)
{
    unsigned int	nlen = sdslen(name);
    unsigned int	vlen = sdslen(value);

    cmd = sdscatfmt(cmd, "$%u\r\n%S\r\n$%u\r\n%S\r\n", nlen, name, vlen, value);
    sdsfree(value); /* NOTE: value free'd here, but caller frees the name parameter */
    return cmd;
}

static sds
series_stream_value(sds cmd, sds name, int type, pmAtomValue *avp)
{
    sds			value;

    if (!avp) {
	value = sdsnewlen("0", 1);
	goto append;
    }

    switch (type) {
    case PM_TYPE_32:
	value = sdscatfmt(sdsempty(), "%i", avp->l);
	break;
    case PM_TYPE_U32:
	value = sdscatfmt(sdsempty(), "%u", avp->ul);
	break;
    case PM_TYPE_64:
	value = sdscatfmt(sdsempty(), "%I", avp->ll);
	break;
    case PM_TYPE_U64:
	value = sdscatfmt(sdsempty(), "%U", avp->ull);
	break;

    case PM_TYPE_FLOAT:
	value = sdscatprintf(sdsempty(), "%e", (double)avp->f);
	break;
    case PM_TYPE_DOUBLE:
	value = sdscatprintf(sdsempty(), "%e", (double)avp->d);
	break;

    case PM_TYPE_STRING:
    case PM_TYPE_AGGREGATE:
    case PM_TYPE_AGGREGATE_STATIC:
	value = sdsdup(avp->cp);
	break;

    default:
	value = sdscatfmt(sdsempty(), "%i", PM_ERR_NYI);
	break;
    }

append:
    return series_stream_append(cmd, name, value);
}

static void
redis_series_stream_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    redisStreamBaton	*baton = (redisStreamBaton *)arg;
    sds			msg;
    int			sts;

    seriesBatonCheckMagic(baton, MAGIC_STREAM, "redis_series_stream_callback");
    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
			     cmd, redis_series_stream_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0) {
	if (testReplyError(reply, REDIS_ESTREAMXADD)) {
	    infofmt(msg, "duplicate or early stream %s insert at time %s",
		baton->hash, baton->stamp);
	    batoninfo(baton, PMLOG_WARNING, msg);
	}
	else {
	    checkStreamReplyString(baton->info, baton->userdata, reply,
		baton->stamp, "stream %s status mismatch at time %s",
		baton->hash, baton->stamp);
	}
    }
    doneRedisStreamBaton(baton);
}

static void
redis_series_timer_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    seriesLoadBaton	*baton = (seriesLoadBaton *)arg;
    int			sts;

    seriesBatonCheckMagic(baton, MAGIC_LOAD, "redis_series_timer_callback");
    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
			     cmd, redis_series_timer_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    doneSeriesLoadBaton(baton, "redis_series_timer_callback");
}

static void
redis_series_stream(redisSlots *slots, sds stamp, metric_t *metric,
		const char *hash, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;
    redisStreamBaton		*baton;
    unsigned int		count;
    int				i, sts, type;
    sds				cmd, key, name, stream = sdsempty();

    if ((baton = malloc(sizeof(redisStreamBaton))) == NULL) {
	stream = sdscatfmt(stream, "OOM creating stream baton");
	batoninfo(load, PMLOG_ERROR, stream);
	return;
    }
    initRedisStreamBaton(baton, slots, stamp, hash, load);
    seriesBatonReferences(load, 2, "redis_series_stream");

    count = 6;	/* XADD key MAXLEN ~ len stamp */
    key = sdscatfmt(sdsempty(), "pcp:values:series:%s", hash);

    if ((sts = metric->error) < 0) {
	sds minus1 = sdsnewlen("-1", 2);
	stream = series_stream_append(stream,
			minus1, sdscatfmt(sdsempty(), "%i", sts));
	sdsfree(minus1);
	count += 2;
    } else {
	name = sdsempty();
	type = metric->desc.type;
	if (metric->desc.indom == PM_INDOM_NULL || metric->u.vlist == NULL) {
	    stream = series_stream_value(stream, name, type, &metric->u.atom);
	    count += 2;
	} else if (metric->u.vlist->listcount <= 0) {
	    sds zero = sdsnew("0");
	    stream = series_stream_append(stream, zero, sdsnew("0"));
	    sdsfree(zero);
	    count += 2;
	} else {
	    for (i = 0; i < metric->u.vlist->listcount; i++) {
		instance_t	*inst;
		value_t		*v = &metric->u.vlist->value[i];

		if ((inst = dictFetchValue(metric->indom->insts, &v->inst)) == NULL)
		    continue;
		name = sdscpylen(name, (const char *)inst->name.hash, sizeof(inst->name.hash));
		stream = series_stream_value(stream, name, type, &v->atom);
		count += 2;
	    }
	}
	sdsfree(name);
    }

    cmd = redis_command(count);
    cmd = redis_param_str(cmd, XADD, XADD_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_str(cmd, "MAXLEN", sizeof("MAXLEN")-1);
    cmd = redis_param_str(cmd, "~", 1);
    cmd = redis_param_sds(cmd, maxstreamlen);
    cmd = redis_param_sds(cmd, stamp);
    cmd = redis_param_raw(cmd, stream);
    sdsfree(stream);

    redisSlotsRequest(slots, XADD, key, cmd, redis_series_stream_callback, baton);

    key = sdscatfmt(sdsempty(), "pcp:values:series:%s", hash);
    cmd = redis_command(3);	/* EXPIRE key timer */
    cmd = redis_param_str(cmd, EXPIRE, EXPIRE_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sds(cmd, streamexpire);

    redisSlotsRequest(slots, EXPIRE, key, cmd, redis_series_timer_callback, load);
}

static void
redis_series_streamed(sds stamp, metric_t *metric, void *arg)
{
    seriesLoadBaton		*baton= (seriesLoadBaton *)arg;
    redisSlots			*slots = baton->slots;
    char			hashbuf[42];
    int				i;

    for (i = 0; i < metric->numnames; i++) {
	pmwebapi_hash_str(metric->names[i].hash, hashbuf, sizeof(hashbuf));
	redis_series_stream(slots, stamp, metric, hashbuf, arg);
    }
}

void
redis_series_mark(redisSlots *redis, sds timestamp, int data, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    seriesGetContext		*context = &baton->pmapi;

    /* TODO: cache mark records in Redis series, then in done callback... */
    doneSeriesGetContext(context, "redis_series_mark");
}

static void
redis_update_version_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    int			sts;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_update_version_callback");

    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
			     cmd, redis_update_version_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */
    if (sts == 0)
	checkStatusReplyOK(baton->info, baton->userdata, reply,
			"%s setup", "pcp:version:schema");
    redis_slots_end_phase(baton);
}

static void
redis_update_version(redisSlotsBaton *baton)
{
    sds			cmd, key;
    const char		ver[] = TO_STRING(SERIES_VERSION);

    seriesBatonReference(baton, "redis_update_version");

    key = sdsnew("pcp:version:schema");
    cmd = redis_command(3);
    cmd = redis_param_str(cmd, SETS, SETS_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_str(cmd, ver, sizeof(ver)-1);
    redisSlotsRequest(baton->slots, SETS, key, cmd, redis_update_version_callback, baton);
}

static void
redis_load_series_version_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    unsigned int	version = 0;
    int			sts;
    sds			msg;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_load_series_version_callback");
    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
				cmd, redis_load_series_version_callback, arg);
    if (sts > 0)
	return;	/* short-circuit as command was re-submitted */

    if (!reply) {
	baton->version = 0;	/* NIL - no version key yet */
    } else if (reply->type == REDIS_REPLY_STRING) {
	version = (unsigned int)atoi(reply->str);
	if (version == 0 || version == SERIES_VERSION) {
	    baton->version = version;
	} else {
	    infofmt(msg, "unsupported series schema (got v%u, expected v%u)",
			version, SERIES_VERSION);
	    batoninfo(baton, PMLOG_ERROR, msg);
	}
    } else if (reply->type == REDIS_REPLY_ERROR) {
	if (sts < 0) {
	    infofmt(msg, "version check error: %s", reply->str);
	    batoninfo(baton, PMLOG_REQUEST, msg);
	}
    } else if (reply->type != REDIS_REPLY_NIL) {
	infofmt(msg, "unexpected schema version reply type (%s)",
		redis_reply_type(reply));
	batoninfo(baton, PMLOG_ERROR, msg);
    } else {
	baton->version = 0;	/* NIL - no version key yet */
    }

    /* set the version when none found (first time through) */
    if (version != SERIES_VERSION && baton->version != -1) {
	/* drop reference from schema version request */
	seriesBatonDereference(baton, "redis_load_series_version_callback");
	redis_update_version(arg);
    } else {
	redis_slots_end_phase(baton);
    }
}

static void
redis_load_series_version(void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    sds			cmd, key;

    seriesBatonReference(baton, "redis_load_series_version");

    key = sdsnew("pcp:version:schema");
    cmd = redis_command(2);
    cmd = redis_param_str(cmd, GETS, GETS_LEN);
    cmd = redis_param_sds(cmd, key);
    redisSlotsRequest(baton->slots, GETS, key, cmd, redis_load_series_version_callback, baton);
}

static void
redis_load_version_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    unsigned int	server_version = 0;
    size_t		l;
    char		*endnum;
    int			sts;
    sds			msg;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_load_version_callback");
    sts = redisSlotsRedirect(baton->slots, reply, baton->info, baton->userdata,
				cmd, redis_load_version_callback, arg);
    if (sts > 0)
	return;
	
    if (!reply) {
	/* This situation should not happen, since we can always get server info from redis */
	infofmt(msg, "no redis version reply");
	batoninfo(baton, PMLOG_ERROR, msg);
    } else if (reply->type == REDIS_REPLY_STRING) {
	for (l = 0; l < reply->len; l++) {
	    if (strncmp("redis_version:", reply->str+l, sizeof("redis_version:")-1) == 0) {
		l += sizeof("redis_version:")-1;
	    	server_version = (unsigned int)strtoul(reply->str+l, &endnum, 10);
	    	if (*endnum != '.') {
		    infofmt(msg, "redis server version parse error");
		    batoninfo(baton, PMLOG_ERROR, msg);
	    	} else if (server_version < SERVER_VERSION) {
		    infofmt(msg, "unsupported redis server (got v%u, expected v%u or above)", 
				server_version, SERVER_VERSION);
	    	    batoninfo(baton, PMLOG_ERROR, msg);
	    	}
	    	break;
	    }
	    /* move to the end of this line within the reply string */
	    while (++l < reply->len) {
		if (reply->str[l] == '\n' || reply->str[l] == '\0')
		    break;
	    }
	}
    } else if (reply->type == REDIS_REPLY_ERROR) {
	if (sts < 0) {
	    infofmt(msg, "redis server version check error: %s", reply->str);
	    batoninfo(baton, PMLOG_REQUEST, msg);
	}
    } else {
	infofmt(msg, "unexpected redis server version reply type (%s)", redis_reply_type(reply));
	batoninfo(baton, PMLOG_ERROR, msg);
    }
    redis_slots_end_phase(baton);
}

static void
redis_load_version(void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    sds			cmd;

    seriesBatonReference(baton, "redis_load_version");

    cmd = redis_command(2);
    cmd = redis_param_str(cmd, INFO, INFO_LEN);
    cmd = redis_param_str(cmd, "SERVER", sizeof("SERVER")-1);
    redisSlotsRequest(baton->slots, INFO, NULL, cmd, redis_load_version_callback, baton);
}

static int
decodeCommandKey(redisSlotsBaton *baton, int index, redisReply *reply)
{
    redisSlots		*slots = baton->slots;
    redisReply		*node;
    dictEntry		*entry;
    long long		position;
    sds			msg, cmd;

    /*
     * Each element contains:
     * - command name
     * - command arity specification
     * - nested array reply of command flags
     * - position of first key in argument list
     * - position of last key in argument list
     * - step count for locating repeating keys
     *
     * We care primarily about the command name and position of
     * the first key, as that key is the one used when selecting
     * the Redis server to communicate with for each command, in
     * a setup with more than one server (cluster or otherwise).
     */
    if (reply->elements < 6) {
	infofmt(msg, "bad reply %s[%d] response (%lld elements)",
			COMMAND, index, (long long)reply->elements);
	batoninfo(baton, PMLOG_RESPONSE, msg);
	return -EPROTO;
    }

    node = reply->element[3];
    if ((position = checkIntegerReply(baton->info, baton->userdata, node,
			"KEY position for %s element %d", COMMAND, index)) < 0)
	return -EINVAL;
    node = reply->element[0];
    if ((cmd = checkStringReply(baton->info, baton->userdata, node,
			"NAME for %s element %d", COMMAND, index)) == NULL)
	return -EINVAL;

    if ((entry = dictAddRaw(slots->keymap, cmd, NULL)) != NULL) {
	dictSetSignedIntegerVal(entry, position);
	sdsfree(cmd);
	return 0;
    }
    sdsfree(cmd);
    return -ENOMEM;
}

static void
redis_load_keymap_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    redisReply		*command;
    sds			msg;
    int			i;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_load_keymap_callback");

    /* no cluster redirection checking is needed for this callback */
    sdsfree(cmd);

    if (reply->type == REDIS_REPLY_ARRAY) {
	for (i = 0; i < reply->elements; i++) {
	    command = reply->element[i];
	    if (checkArrayReply(baton->info, baton->userdata,
			command, "%s entry %d", COMMAND, i) == 0)
		decodeCommandKey(baton, i, command);
	}
    } else if (reply->type == REDIS_REPLY_ERROR) {
	infofmt(msg, "command key mapping error: %s", reply->str);
	batoninfo(baton, PMLOG_REQUEST, msg);
    } else if (reply->type != REDIS_REPLY_NIL) {
	infofmt(msg, "unexpected command reply type (%s)",
		redis_reply_type(reply));
	batoninfo(baton, PMLOG_ERROR, msg);
    }
    redis_slots_end_phase(baton);
}

static void
redis_load_keymap(void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    sds			cmd;

    seriesBatonReference(baton, "redis_load_keymap");

    cmd = redis_command(1);
    cmd = redis_param_str(cmd, COMMAND, COMMAND_LEN);
    redisSlotsRequest(baton->slots, GETS, NULL, cmd, redis_load_keymap_callback, baton);
}

static int
decodeRedisNode(redisSlotsBaton *baton, redisReply *reply, redisSlotServer *server)
{
    redisReply		*value;
    unsigned int	port;
    sds			msg;

    /* expecting IP address and port (integer), ignore optional node ID */
    if (reply->elements < 2) {
	infofmt(msg, "insufficient elements in cluster NODE reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }

    value = reply->element[1];
    if (value->type != REDIS_REPLY_INTEGER) {
	infofmt(msg, "expected integer port in cluster NODE reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }
    port = (unsigned int)value->integer;

    value = reply->element[0];
    if (value->type != REDIS_REPLY_STRING) {
	infofmt(msg, "expected string hostspec in cluster NODE reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }

    server->hostspec = sdscatfmt(sdsempty(), "%s:%u", value->str, port);
    return server->hostspec ? 0 : -ENOMEM;
}

static int
decodeRedisSlot(redisSlotsBaton *baton, redisReply *reply)
{
    redisSlotServer	*servers = NULL;
    redisSlotRange	slots, *sp;
    redisReply		*node;
    long long		slot;
    int			i, n;
    sds			msg;

    /* expecting start and end slot range integers, then node arrays */
    if (reply->elements < 3) {
	infofmt(msg, "insufficient elements in cluster SLOT reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }
    memset(&slots, 0, sizeof(slots));

    node = reply->element[0];
    if ((slot = checkIntegerReply(baton->info, baton->userdata,
				node, "%s start", "SLOT")) < 0) {
	infofmt(msg, "expected integer start in cluster SLOT reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }
    slots.start = (__uint32_t)slot;
    node = reply->element[1];
    if ((slot = checkIntegerReply(baton->info, baton->userdata,
				node, "%s end", "SLOT")) < 0) {
	infofmt(msg, "expected integer end in cluster SLOT reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }
    slots.end = (__uint32_t)slot;
    node = reply->element[2];
    if ((decodeRedisNode(baton, node, &slots.primary)) < 0)
	return -EINVAL;

    if ((sp = calloc(1, sizeof(redisSlotRange))) == NULL)
	return -ENOMEM;
    *sp = slots;    /* struct copy */

    if ((n = reply->elements - 3) > 0)
	if ((servers = calloc(n, sizeof(redisSlotServer))) == NULL)
	    n = 0;
    sp->nreplicas = n;
    sp->replicas = servers;

    for (i = 0; i < n; i++) {
	node = reply->element[i + 3];
	if (checkArrayReply(baton->info, baton->userdata,
				node, "%s range %u-%u replica %d",
				"SLOTS", sp->start, sp->end, i) == 0)
	    decodeRedisNode(baton, node, &sp->replicas[i]);
    }

    return redisSlotRangeInsert(baton->slots, sp);
}

static void
decodeRedisSlots(redisSlotsBaton *baton, redisReply *reply)
{
    redisReply		*slot;
    int			i;

    for (i = 0; i < reply->elements; i++) {
	slot = reply->element[i];
	if (checkArrayReply(baton->info, baton->userdata,
			slot, "%s %s entry %d", CLUSTER, "SLOTS", i) == 0)
	    decodeRedisSlot(baton, slot);
    }
}

static void
redis_load_slots_callback(
	redisAsyncContext *c, redisReply *reply, const sds cmd, void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    redisSlots		*slots = baton->slots;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_load_slots_callback");

    slots->setup = 1;	/* we've received initial response from Redis */
    slots->refresh = 0;	/* we're processing CLUSTER SLOTS command now */

    /* no cluster redirection checking is needed for this callback */
    sdsfree(cmd);

    if (reply && testReplyError(reply, REDIS_ENOCLUSTER) == 0) {
	/* cluster of Redis instances, following the cluster spec */
	if (checkArrayReply(baton->info, baton->userdata,
				reply, "%s %s", CLUSTER, "SLOTS") == 0) {
	    redisSlotsClear(slots);
	    decodeRedisSlots(baton, reply);
	}
    }
    redis_slots_end_phase(baton);
}

static void
redis_load_slots(void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    sds			cmd;

    seriesBatonReference(baton, "redis_load_slots");

    cmd = redis_command(2);
    cmd = redis_param_str(cmd, CLUSTER, CLUSTER_LEN);
    cmd = redis_param_str(cmd, "SLOTS", sizeof("SLOTS")-1);
    redisSlotsRequest(baton->slots, CLUSTER, NULL, cmd, redis_load_slots_callback, baton);
}

redisSlots *
redisSlotsConnect(dict *config, redisSlotsFlags flags,
		redisInfoCallBack info, redisDoneCallBack done,
		void *userdata, void *events, void *arg)
{
    redisSlotsBaton		*baton;
    redisSlots			*slots;
    sds				msg;
    unsigned int		i = 0;

    if ((baton = (redisSlotsBaton *)calloc(1, sizeof(redisSlotsBaton))) != NULL) {
	if ((slots = redisSlotsInit(config, events)) != NULL) {
	    initRedisSlotsBaton(baton, info, done, userdata, events, arg);
	    baton->slots = slots;
	    baton->current = &baton->phases[0];
	    baton->phases[i++].func = redis_load_slots;

	    /* Prepare mapping of commands to key positions if needed */
	    if (flags & SLOTS_KEYMAP)
		baton->phases[i++].func = redis_load_keymap;
	    /* Verify pmseries schema version and create it if needed */
	    if (flags & SLOTS_VERSION) {
		baton->phases[i++].func = redis_load_version; /* Redis v5 */
		baton->phases[i++].func = redis_load_series_version;
	    }
	    /* Register the pmsearch schema with RediSearch if needed */
	    if (flags & SLOTS_SEARCH)
		baton->phases[i++].func = redis_load_search_schema;
	    baton->phases[i++].func = redis_slots_finished;
	    assert(i <= SLOTS_PHASES);
	    seriesBatonPhases(baton->current, i, baton);
	    return slots;
	}
	redis_slots_end_phase(baton);
    }
    infofmt(msg, "Failed to allocate memory for Redis slots");
    info(PMLOG_ERROR, msg, arg);
    sdsfree(msg);
    return NULL;
}

seriesModuleData *
getSeriesModuleData(pmSeriesModule *module)
{
    if (module->privdata == NULL)
	module->privdata = calloc(1, sizeof(seriesModuleData));
    return module->privdata;
}

int
pmSeriesSetSlots(pmSeriesModule *module, void *slots)
{
    seriesModuleData	*data = getSeriesModuleData(module);

    if (data) {
	data->slots = (redisSlots *)slots;
	data->shareslots = 1;
	return 0;
    }
    return -ENOMEM;
}

int
pmSeriesSetHostSpec(pmSeriesModule *module, sds hostspec)
{
    (void)module;
    (void)hostspec;
    return -ENOTSUP;	/* deprecated, use pmSeriesSetConfiguration */
}

int
pmSeriesSetConfiguration(pmSeriesModule *module, dict *config)
{
    seriesModuleData	*data = getSeriesModuleData(module);

    if (data) {
	data->config = config;
	return 0;
    }
    return -ENOMEM;
}

int
pmSeriesSetEventLoop(pmSeriesModule *module, void *events)
{
    seriesModuleData	*data = getSeriesModuleData(module);

    if (data) {
	data->events = (uv_loop_t *)events;
	return 0;
    }
    return -ENOMEM;
}

int
pmSeriesSetMetricRegistry(pmSeriesModule *module, mmv_registry_t *registry)
{
    seriesModuleData	*data = getSeriesModuleData(module);

    if (data) {
	data->metrics = registry;
	return 0;
    }
    return -ENOMEM;
}

static void
redisSeriesInit(struct dict *config)
{
    sds		option;

    if (!cursorcount) {
	if ((option = pmIniFileLookup(config, "pmseries", "cursor.count")))
	    cursorcount = option;
	else
	    cursorcount = sdsnew("256");
    }

    if (!maxstreamlen) {
	if ((option = pmIniFileLookup(config, "pmseries", "stream.maxlen")))
	    maxstreamlen = option;
	else
	    maxstreamlen = sdsnew("8640");	/* 1 day, ~10 second delta */
    }

    if (!streamexpire) {
	if ((option = pmIniFileLookup(config, "pmseries", "stream.expire")))
	    streamexpire = option;
	else
	    streamexpire = sdsnew("86400");	/* 1 day (without changes) */
    }
}

void
redisGlobalsInit(struct dict *config)
{
    redisSeriesInit(config);
    redisSearchInit(config);
    redisScriptsInit();
    redisMapsInit();
}

static void
pmSeriesSetupMetrics(pmSeriesModule *module)
{
    seriesModuleData	*data = getSeriesModuleData(module);
    pmUnits		nounits = MMV_UNITS(0,0,0,0,0,0);
    pmInDom		noindom = MMV_INDOM_NULL;

    if (data == NULL || data->metrics == NULL)
    	return; /* no metric registry has been set up */

    /*
     * various RESTAPI request call counters
     */
    mmv_stats_add_metric(data->metrics, "query.calls", 1,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total RESTAPI calls to /series/values", NULL);

    mmv_stats_add_metric(data->metrics, "descs.calls", 2,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total RESTAPI calls to /series/descs", NULL);

    mmv_stats_add_metric(data->metrics, "instances.calls", 3,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total RESTAPI calls to /series/instances", NULL);

    mmv_stats_add_metric(data->metrics, "sources.calls", 4,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total RESTAPI calls to /series/sources", NULL);

    mmv_stats_add_metric(data->metrics, "metrics.calls", 5,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total RESTAPI calls to /series/metrics", NULL);

    mmv_stats_add_metric(data->metrics, "values.calls", 6,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total RESTAPI calls to /series/values", NULL);

    mmv_stats_add_metric(data->metrics, "labels.calls", 7,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total RESTAPI calls to /series/labels", NULL);

    mmv_stats_add_metric(data->metrics, "labelvalues.calls", 8,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total RESTAPI calls to /series/labelvalues", NULL);

    mmv_stats_add_metric(data->metrics, "load.calls", 9,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total RESTAPI calls to /series/load", NULL);

    data->metrics_handle = mmv_stats_start(data->metrics);
}

void
pmSeriesStatsAdd(pmSeriesModule *module, const char *name, const char *inst, double count)
{
    seriesModuleData   *data;

    if (module != NULL && (data = getSeriesModuleData(module)) != NULL)
	mmv_stats_add(data->metrics_handle, name, inst, count);
}

void
pmSeriesStatsSet(pmSeriesModule *module, const char *name, const char *inst, double value)
{
    seriesModuleData   *data;

    if (module != NULL && (data = getSeriesModuleData(module)) != NULL)
	mmv_stats_set(data->metrics_handle, name, inst, value);
}

int
pmSeriesSetup(pmSeriesModule *module, void *arg)
{
    seriesModuleData	*data = getSeriesModuleData(module);
    redisSlotsFlags	flags;

    if (data == NULL)
	return -ENOMEM;

    /* create global EVAL hashes and string map caches */
    redisGlobalsInit(data->config);

    /* fast path for when Redis has been setup already */
    if (data->slots) {
	module->on_setup(arg);
	data->shareslots = 1;
    } else {
	/* establish an initial connection to Redis instance(s) */
	flags = SLOTS_VERSION | SLOTS_SEARCH;
	data->slots = redisSlotsConnect(
			data->config, flags, module->on_info,
			module->on_setup, arg, data->events, arg);
	data->shareslots = 0;
    }

    pmSeriesSetupMetrics(module);

    return 0;
}

void
pmSeriesClose(pmSeriesModule *module)
{
    seriesModuleData	*data = (seriesModuleData *)module->privdata;

    if (data) {
	if (!data->shareslots)
	    redisSlotsFree(data->slots);
	memset(data, 0, sizeof(seriesModuleData));
	free(data);
    }
}

discoverModuleData *
getDiscoverModuleData(pmDiscoverModule *module)
{
    if (module->privdata == NULL)
	module->privdata = calloc(1, sizeof(discoverModuleData));
    return module->privdata;
}

int
pmDiscoverSetSlots(pmDiscoverModule *module, void *slots)
{
    discoverModuleData	*data = getDiscoverModuleData(module);

    if (data) {
	data->slots = (redisSlots *)slots;
	data->shareslots = 1;
	return 0;
    }
    return -ENOMEM;
}

int
pmDiscoverSetHostSpec(pmDiscoverModule *module, sds hostspec)
{
    (void)module;
    (void)hostspec;
    return -ENOTSUP;	/* deprecated, use pmDiscoverSetConfiguration */
}

int
pmDiscoverSetConfiguration(pmDiscoverModule *module, dict *config)
{
    discoverModuleData	*data = getDiscoverModuleData(module);

    if (data) {
	data->config = config;
	return 0;
    }
    return -ENOMEM;
}

int
pmDiscoverSetEventLoop(pmDiscoverModule *module, void *events)
{
    discoverModuleData	*data = getDiscoverModuleData(module);

    if (data) {
	data->events = (uv_loop_t *)events;
	return 0;
    }
    return -ENOMEM;
}

void
pmDiscoverSetupMetrics(pmDiscoverModule *module)
{
    discoverModuleData	*data = getDiscoverModuleData(module);
    pmUnits		nounits = MMV_UNITS(0,0,0,0,0,0);
    pmInDom		noindom = MMV_INDOM_NULL;

    if (data == NULL || data->metrics == NULL)
    	return; /* no metric registry has been set up */

    /*
     * redis and archive discovery metrics
     */
    mmv_stats_add_metric(data->metrics, "discover.monitored", 1,
	MMV_TYPE_U64, MMV_SEM_DISCRETE, nounits, noindom,
	"number of directories, sub-directories and archives currently being monitored", NULL);

    mmv_stats_add_metric(data->metrics, "discover.purged", 2,
	MMV_TYPE_U64, MMV_SEM_DISCRETE, nounits, noindom,
	"number of directories, sub-directories and archives purged (no longer being monitored)", NULL);

    mmv_stats_add_metric(data->metrics, "discover.metadata.callbacks", 3,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total calls to process metadata for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.metadata.loops", 4,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total loops processing metadata for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.metadata.decode.desc", 5,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total desc records decoded processing metadata for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.metadata.decode.indom", 6,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total indom records decoded processing metadata for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.metadata.decode.label", 7,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total label records decoded processing metadata for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.metadata.decode.helptext", 8,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total help text records decoded processing metadata for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.logvol.callbacks", 9,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total calls to process logvol data for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.logvol.loops", 10,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total loops processing logvol data for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.logvol.change_vol", 11,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total log vol values callbacks made for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.logvol.decode.result", 12,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total result records decoded for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.logvol.decode.result_pmids", 13,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total metric pmids in decoded result records for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.logvol.decode.mark_record", 14,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total mark record result records decoded for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.logvol.new_contexts", 15,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total successful new context calls made for all monitored archives", NULL);

    mmv_stats_add_metric(data->metrics, "discover.logvol.get_archive_end_failed", 16,
	MMV_TYPE_U64, MMV_SEM_COUNTER, nounits, noindom,
	"total failed pmGetArchiveEnd calls after successfully creating a new context for all monitored archives", NULL);

    data->metrics_handle = mmv_stats_start(data->metrics);
}

int
pmDiscoverSetMetricRegistry(pmDiscoverModule *module, mmv_registry_t *registry)
{
    discoverModuleData	*data = getDiscoverModuleData(module);

    if (data) {
	data->metrics = registry;
	return 0;
    }
    return -ENOMEM;
}

int
pmDiscoverSetup(pmDiscoverModule *module, pmDiscoverCallBacks *cbs, void *arg)
{
    discoverModuleData	*data = getDiscoverModuleData(module);
    const char		fallback[] = "/var/log/pcp/pmlogger";
    const char		*logdir = pmGetOptionalConfig("PCP_ARCHIVE_DIR");
    struct dict		*config;
    unsigned int	domain, serial;
    pmInDom		indom;
    sds			option, *ids;
    int			i, sts, nids;

    if (data == NULL)
	return -ENOMEM;
    config = data->config;

    /* double-check that we are supposed to be in here */
    if ((option = pmIniFileLookup(config, "discover", "enabled"))) {
	if (strcasecmp(option, "false") == 0)
	    return 0;
    }

    /* prepare for optional metric and indom exclusion */
    if ((option = pmIniFileLookup(config, "discover", "exclude.metrics"))) {
	if ((data->pmids = dictCreate(&intKeyDictCallBacks, NULL)) == NULL)
	    return -ENOMEM;
	/* parse comma-separated metric name glob patterns, in 'option' */
	if ((ids = sdssplitlen(option, sdslen(option), ",", 1, &nids))) {
	    data->exclude_names = nids;
	    for (i = 0; i < nids; i++)
		ids[i] = sdstrim(ids[i], " ");
	    data->patterns = ids;
	}
    }
    if ((option = pmIniFileLookup(config, "discover", "exclude.indoms"))) {
	if ((data->indoms = dictCreate(&intKeyDictCallBacks, NULL)) == NULL)
	    return -ENOMEM;
	/* parse comma-separated indoms in 'option', convert to pmInDom */
	if ((ids = sdssplitlen(option, sdslen(option), ",", 1, &nids))) {
	    data->exclude_indoms = nids;
	    for (i = 0; i < nids; i++) {
		if (sscanf(ids[i], "%u.%u", &domain, &serial) == 2) {
		    indom = pmInDom_build(domain, serial);
		    dictAdd(data->indoms, &indom, NULL);
		}
		sdsfree(ids[i]);
	    }
	    free(ids);
	}
    }

    /* create global EVAL hashes and string map caches */
    redisSearchInit(data->config);
    redisSeriesInit(data->config);
    redisScriptsInit();
    redisMapsInit();

    if (!logdir)
	logdir = fallback;

    pmDiscoverSetupMetrics(module);

    if (access(logdir, F_OK) == 0) {
	sts = pmDiscoverRegister(logdir, module, cbs, arg);
	if (sts >= 0) {
	    data->handle = sts;
	    return 0;
	}
    }
    return -ESRCH;
}

void
pmDiscoverClose(pmDiscoverModule *module)
{
    discoverModuleData	*discover = (discoverModuleData *)module->privdata;
    unsigned int	i;

    if (discover) {
	pmDiscoverUnregister(discover->handle);
	if (!discover->shareslots)
	    redisSlotsFree(discover->slots);
	for (i = 0; i < discover->exclude_names; i++)
	    sdsfree(discover->patterns[i]);
	if (discover->patterns)
	    free(discover->patterns);
	if (discover->pmids)
	    dictRelease(discover->pmids);
	if (discover->indoms)
	    dictRelease(discover->indoms);
	memset(discover, 0, sizeof(*discover));
	free(discover);
    }
}
