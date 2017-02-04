/*
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
#include <math.h> /* isnan(), isinf() */

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/

/*
 * 检查size是否超过Redis最大限制
 */
static int checkStringLength(redisClient *c, long long size) {
    if (size > 512*1024*1024) {
        addReplyError(c,"string exceeds maximum allowed size (512MB)");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 * 'flags' changes the behavior of the command (NX or XX, see belove).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */
/*
 * SET / SETEX / PSETEX / SETNX的通用底层处理
 * c		客户端
 * flags	NX / XX ...
 * expire	过期时间
 * ok_reply和abort_reply是函数的返回值
 */

#define REDIS_SET_NO_FLAGS 0
#define REDIS_SET_NX (1<<0)     // key不存在时设置
#define REDIS_SET_XX (1<<1)     // key存在时设置

void setGenericCommand(redisClient *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    long long milliseconds = 0; 

	// 如果带有expire参数，将它从sds转换为long long
    if (expire) {
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != REDIS_OK)
            return;
        if (milliseconds <= 0) {
            addReplyError(c,"invalid expire time in SETEX");
            return;
        }
		// 秒转位毫秒
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }

    if ((flags & REDIS_SET_NX && lookupKeyWrite(c->db,key) != NULL) ||
        (flags & REDIS_SET_XX && lookupKeyWrite(c->db,key) == NULL))
    {
        addReply(c, abort_reply ? abort_reply : shared.nullbulk);
        return;
    }
	// 设置key value
    setKey(c->db,key,val);
    server.dirty++;

    if (expire) setExpire(c->db,key,mstime()+milliseconds);	// 设置超时时间
    addReply(c, ok_reply ? ok_reply : shared.ok);
}

/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
void setCommand(redisClient *c) {
    int j;
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = REDIS_SET_NO_FLAGS;

    for (j = 3; j < c->argc; j++) {
        char *a = c->argv[j]->ptr;
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1];

        if ((a[0] == 'n' || a[0] == 'N') &&
            (a[1] == 'x' || a[1] == 'X') && a[2] == '\0') {
            flags |= REDIS_SET_NX;
        } else if ((a[0] == 'x' || a[0] == 'X') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0') {
            flags |= REDIS_SET_XX;
        } else if ((a[0] == 'e' || a[0] == 'E') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' && next) {
            unit = UNIT_SECONDS;
            expire = next;
            j++;
        } else if ((a[0] == 'p' || a[0] == 'P') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' && next) {
            unit = UNIT_MILLISECONDS;
            expire = next;
            j++;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

	// 尝试压缩value的空间
    c->argv[2] = tryObjectEncoding(c->argv[2]);

    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);
}

/*
 * SETNX KEY VALUE				不存在时设置
 */
void setnxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,REDIS_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

/*
 * SETEX KEY SECONDS VALUE		设置超时时间(s)
 */
void setexCommand(redisClient *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,REDIS_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

/*
 * PSETEX KEY MILISECONDS VALUE	设置超时时间(ms)
 */
void psetexCommand(redisClient *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,REDIS_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}

int getGenericCommand(redisClient *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL)
		// 找到key
        return REDIS_OK;

    if (o->type != REDIS_STRING) {
        addReply(c,shared.wrongtypeerr);
        return REDIS_ERR;
    } else {
        addReplyBulk(c,o);
        return REDIS_OK;
    }
}

/*
 * GET KEY
 */
void getCommand(redisClient *c) {
    getGenericCommand(c);
}

/*
 * GETSET KEY VALUE		设置并返回旧值
 */
void getsetCommand(redisClient *c) {
    if (getGenericCommand(c) == REDIS_ERR) return;

    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setKey(c->db,c->argv[1],c->argv[2]);
    server.dirty++;
}

/*
 * SETRANGE KEY OFFSET VALUE	从offset开始覆盖
 */
void setrangeCommand(redisClient *c) {
    robj *o;
    long offset;
    sds value = c->argv[3]->ptr;

    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != REDIS_OK)
        return;

    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }

	// 检测key是否已经被写入
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
		// key不存在

		// value为空串
        if (sdslen(value) == 0) {
            addReply(c,shared.czero);
            return;
        }

		// value长度过长
        if (checkStringLength(c,offset+sdslen(value)) != REDIS_OK)
            return;

		// 将key设置为空字符串对象
        o = createObject(REDIS_STRING,sdsempty());
        dbAdd(c->db,c->argv[1],o);
    } else {
		// key已经存在
        size_t olen;

		// 检测key是否为字符串类型
        if (checkType(c,o,REDIS_STRING))
            return;

		// 如果value为空字符串，则直接返回原有字符串的长度
        olen = stringObjectLen(o);
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

		// 检测修改后的字符串是否超长
        if (checkStringLength(c,offset+sdslen(value)) != REDIS_OK)
            return;

		// 当o为共享对象或编码对象时，创建副本
        if (o->refcount != 1 || o->encoding != REDIS_ENCODING_RAW) {
            robj *decoded = getDecodedObject(o);
            o = createStringObject(decoded->ptr, sdslen(decoded->ptr));
            decrRefCount(decoded);
            dbOverwrite(c->db,c->argv[1],o);
        }
    }

	// 修改
    if (sdslen(value) > 0) {
		// 先全部填充为0
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));

		// 拷贝
        memcpy((char*)o->ptr+offset,value,sdslen(value));

        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }

	// 将修改后字符串的长度返回给client
    addReplyLongLong(c,sdslen(o->ptr));
}

/*
 * GET RANGE START END
 */
void getrangeCommand(redisClient *c) {
    robj *o;
    long start, end;
    char *str, llbuf[32];
    size_t strlen;

	// 获取start
    if (getLongFromObjectOrReply(c,c->argv[2],&start,NULL) != REDIS_OK)
        return;

	// 获取end
    if (getLongFromObjectOrReply(c,c->argv[3],&end,NULL) != REDIS_OK)
        return;

	// key不存在或者不为string类型，直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,REDIS_STRING)) return;

	// 获取字符串以及长度
    if (o->encoding == REDIS_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }

	// 对负数进行转换
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((unsigned)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end) {
        addReply(c,shared.emptybulk);
    } else {
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

/*
 * MSET KEY [KEY ...]
 */
void mgetCommand(redisClient *c) {
    int j;

    addReplyMultiBulkLen(c,c->argc-1);
    for (j = 1; j < c->argc; j++) {
		// 读取
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
			// key不存在
            addReply(c,shared.nullbulk);
        } else {
            if (o->type != REDIS_STRING) {
                addReply(c,shared.nullbulk);
            } else {
                addReplyBulk(c,o);
            }
        }
    }
}

void msetGenericCommand(redisClient *c, int nx) {
    int j, busykeys = 0;

    if ((c->argc % 2) == 0) {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }

	// nx情况下，检查给定的key是否已经存在
	// 只要任何一个key存在，那么就不进行修改，直接返回0
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                busykeys++;
            }
        }
		// 如果有已存在的key
        if (busykeys) {
            addReply(c, shared.czero);
            return;
        }
    }

	// 写入
    for (j = 1; j < c->argc; j += 2) {
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
        setKey(c->db,c->argv[j],c->argv[j+1]);
    }
    server.dirty += (c->argc-1)/2;
    addReply(c, nx ? shared.cone : shared.ok);
}

/*
 * MSET KEY VALUE [ KEY VALUE ...]
 */
void msetCommand(redisClient *c) {
    msetGenericCommand(c,0);
}

/*
 * MSETNX KEY VALUE [ KEY VALUE ...]
 */
void msetnxCommand(redisClient *c) {
    msetGenericCommand(c,1);
}

void incrDecrCommand(redisClient *c, long long incr) {
    long long value, oldvalue;
    robj *o, *new;

	// 查找key
    o = lookupKeyWrite(c->db,c->argv[1]);

	// 如果key存在并且为string类型，返回
    if (o != NULL && checkType(c,o,REDIS_STRING)) return;

	// 获取redisObject的long long值
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != REDIS_OK) return;

	// 溢出检测
    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }

	// 处理
    value += incr;

	// 创建string对象
    new = createStringObjectFromLongLong(value);

	// 重写 ／ 新建
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);

	// 修改提示
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;

    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}

/*
 * INCR KEY
 */
void incrCommand(redisClient *c) {
    incrDecrCommand(c,1);
}

/*
 * DECR KEY
 */
void decrCommand(redisClient *c) {
    incrDecrCommand(c,-1);
}

/*
 * INCRBY KEY INCREMENT
 */
void incrbyCommand(redisClient *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c,incr);
}

/*
 * DECRBY KEY DECREMENT
 */
void decrbyCommand(redisClient *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c,-incr);
}

/*
 * INCRBYFLOAT KEY INCREMENT
 */
void incrbyfloatCommand(redisClient *c) {
    long double incr, value;
    robj *o, *new, *aux;

	// 检测key是否存在
    o = lookupKeyWrite(c->db,c->argv[1]);

	// key为string类型，返回
    if (o != NULL && checkType(c,o,REDIS_STRING)) return;

	// 获取浮点型的value和增量
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != REDIS_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != REDIS_OK)
        return;

    value += incr;
	// 溢出检测
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }

	// 保存
    new = createStringObjectFromLongDouble(value);
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    aux = createStringObject("SET",3);
    rewriteClientCommandArgument(c,0,aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c,2,new);
}

/*
 * APPEND KEY VALUE
 */
void appendCommand(redisClient *c) {
    size_t totlen;
    robj *o, *append;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
		// key还未被插入db字典
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
		// 必须为string类型
        if (checkType(c,o,REDIS_STRING))
            return;

        append = c->argv[2];
		// append之后的长度
        totlen = stringObjectLen(o)+sdslen(append->ptr);
		// 长度检测
        if (checkStringLength(c,totlen) != REDIS_OK)
            return;

		// 如果是被共享的对象 ／ 编码对象，生成其string类型的拷贝
        if (o->refcount != 1 || o->encoding != REDIS_ENCODING_RAW) {
            robj *decoded = getDecodedObject(o);
            o = createStringObject(decoded->ptr, sdslen(decoded->ptr));
            decrRefCount(decoded);
            dbOverwrite(c->db,c->argv[1],o);
        }

		// 添加
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    addReplyLongLong(c,totlen);
}

/*
 * STRLEN key
 */
void strlenCommand(redisClient *c) {
    robj *o;
	// key必须存在 && string类型
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_STRING)) return;
    addReplyLongLong(c,stringObjectLen(o));
}
