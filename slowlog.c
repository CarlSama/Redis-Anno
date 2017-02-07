/* Slowlog implements a system that is able to remember the latest N
 * queries that took more than M microseconds to execute.
 *
 * The execution time to reach to be logged in the slow log is set
 * using the 'slowlog-log-slower-than' config directive, that is also
 * readable and writable using the CONFIG SET/GET command.
 *
 * The slow queries log is actually not "logged" in the Redis log file
 * but is accessible thanks to the SLOWLOG command.
 *
 * ----------------------------------------------------------------------------
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


#include "redis.h"
#include "slowlog.h"

/*
 * 创建slow log节点
 * 增加相应数据的引用技术
 */
slowlogEntry *slowlogCreateEntry(robj **argv, int argc, long long duration) {
    slowlogEntry *se = zmalloc(sizeof(*se));
    int j, slargc = argc;

    if (slargc > SLOWLOG_ENTRY_MAX_ARGC) slargc = SLOWLOG_ENTRY_MAX_ARGC;
    se->argc = slargc;
    se->argv = zmalloc(sizeof(robj*)*slargc);
    for (j = 0; j < slargc; j++) {
		// 避免记录太多
        if (slargc != argc && j == slargc-1) {
            se->argv[j] = createObject(REDIS_STRING,
                sdscatprintf(sdsempty(),"... (%d more arguments)",
                argc-slargc+1));
        } else {
			// 避免过长记录
            if (argv[j]->type == REDIS_STRING &&
                argv[j]->encoding == REDIS_ENCODING_RAW &&
                sdslen(argv[j]->ptr) > SLOWLOG_ENTRY_MAX_STRING)
            {
                sds s = sdsnewlen(argv[j]->ptr, SLOWLOG_ENTRY_MAX_STRING);

                s = sdscatprintf(s,"... (%lu more bytes)",
                    (unsigned long)
                    sdslen(argv[j]->ptr) - SLOWLOG_ENTRY_MAX_STRING);
                se->argv[j] = createObject(REDIS_STRING,s);
            } else {
                se->argv[j] = argv[j];
                incrRefCount(argv[j]);
            }
        }
    }
    se->time = time(NULL);
    se->duration = duration;
    se->id = server.slowlog_entry_id++;
    return se;
}

/*
 *  释放slow log节点
 */
void slowlogFreeEntry(void *septr) {
    slowlogEntry *se = septr;
    int j;

    for (j = 0; j < se->argc; j++)
        decrRefCount(se->argv[j]);
    zfree(se->argv);
    zfree(se);
}

/*
 * slow log初始化, server初始化时被调用
 */
void slowlogInit(void) {
    server.slowlog = listCreate();
    server.slowlog_entry_id = 0;			// 初始id
    listSetFreeMethod(server.slowlog,slowlogFreeEntry);
}

/*
 * 向slow log中插入新的记录
 */
void slowlogPushEntryIfNeeded(robj **argv, int argc, long long duration) {
	// 未打开慢查询记录
    if (server.slowlog_log_slower_than < 0) return;

	// 需要记录
    if (duration >= server.slowlog_log_slower_than)
        listAddNodeHead(server.slowlog,slowlogCreateEntry(argv,argc,duration));

	// 记录个数限制
    while (listLength(server.slowlog) > server.slowlog_max_len)
        listDelNode(server.slowlog,listLast(server.slowlog));
}

/* Remove all the entries from the current slow log. */
void slowlogReset(void) {
    while (listLength(server.slowlog) > 0)
        listDelNode(server.slowlog,listLast(server.slowlog));
}

/* The SLOWLOG command. Implements all the subcommands needed to handle the
 * Redis slow log. */
void slowlogCommand(redisClient *c) {
	// reset
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"reset")) {
        slowlogReset();
        addReply(c,shared.ok);
	// len
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"len")) {
        addReplyLongLong(c,listLength(server.slowlog));
	// get
    } else if ((c->argc == 2 || c->argc == 3) &&
               !strcasecmp(c->argv[1]->ptr,"get"))
    {
        long count = 10, sent = 0;
        listIter li;
        void *totentries;
        listNode *ln;
        slowlogEntry *se;

        if (c->argc == 3 &&
            getLongFromObjectOrReply(c,c->argv[2],&count,NULL) != REDIS_OK)
            return;

        listRewind(server.slowlog,&li);
        totentries = addDeferredMultiBulkLength(c);
        while(count-- && (ln = listNext(&li))) {
            int j;

            se = ln->value;
            addReplyMultiBulkLen(c,4);
            addReplyLongLong(c,se->id);
            addReplyLongLong(c,se->time);
            addReplyLongLong(c,se->duration);
            addReplyMultiBulkLen(c,se->argc);
            for (j = 0; j < se->argc; j++)
                addReplyBulk(c,se->argv[j]);
            sent++;
        }
        setDeferredMultiBulkLength(c,totentries,sent);
    } else {
        addReplyError(c,
            "Unknown SLOWLOG subcommand or wrong # of args. Try GET, RESET, LEN.");
    }
}
