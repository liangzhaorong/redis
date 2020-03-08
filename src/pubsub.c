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

#include "server.h"

/*-----------------------------------------------------------------------------
 * Pubsub low level API
 *----------------------------------------------------------------------------*/

void freePubsubPattern(void *p) {
    pubsubPattern *pat = p;

    decrRefCount(pat->pattern);
    zfree(pat);
}

int listMatchPubsubPattern(void *a, void *b) {
    pubsubPattern *pa = a, *pb = b;

    return (pa->client == pb->client) &&
           (equalStringObjects(pa->pattern,pb->pattern));
}

/* Return the number of channels + patterns a client is subscribed to. */
int clientSubscriptionsCount(client *c) {
    return dictSize(c->pubsub_channels)+
           listLength(c->pubsub_patterns);
}

/* Subscribe a client to a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was already subscribed to that channel. */
int pubsubSubscribeChannel(client *c, robj *channel) {
    dictEntry *de;
    list *clients = NULL;
    int retval = 0;

    // 将订阅的 channel 加入 client 的 pubsub_channels 字典中, 如果存在则直接返回
    if (dictAdd(c->pubsub_channels,channel,NULL) == DICT_OK) {
        retval = 1;
        incrRefCount(channel);
        /* Add the client to the channel -> list of clients hash table */
        de = dictFind(server.pubsub_channels,channel);
        if (de == NULL) {
            clients = listCreate();
            dictAdd(server.pubsub_channels,channel,clients);
            incrRefCount(channel);
        } else {
            clients = dictGetVal(de);
        }
        // 将该 client 加入 server.pubsub_channels 中订阅了该 channel 的值链表
        listAddNodeTail(clients,c);
    }
    /* Notify the client */
    addReply(c,shared.mbulkhdr[3]);
    addReply(c,shared.subscribebulk);
    addReplyBulk(c,channel);
    addReplyLongLong(c,clientSubscriptionsCount(c));
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribeChannel(client *c, robj *channel, int notify) {
    dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;

    /* Remove the channel from the client -> channels hash table */
    incrRefCount(channel); /* channel may be just a pointer to the same object
                            we have in the hash tables. Protect it... */
    if (dictDelete(c->pubsub_channels,channel) == DICT_OK) { // 从 pubsub_channels 字典中删除该 channel
        retval = 1;
        /* Remove the client from the channel -> clients list hash table */
        de = dictFind(server.pubsub_channels,channel);
        serverAssertWithInfo(c,NULL,de != NULL);
        clients = dictGetVal(de);       // 获取订阅该 channel 的 clients 链表
        ln = listSearchKey(clients,c);  // 在 clients 链表中查找是否有该客户端 c
        serverAssertWithInfo(c,NULL,ln != NULL);
        listDelNode(clients,ln);        // 从链表中移除该客户端 c
        if (listLength(clients) == 0) { // 若已经没有客户端订阅该 channel, 则移除该 channel
            /* Free the list and associated hash entry at all if this was
             * the latest client, so that it will be possible to abuse
             * Redis PUBSUB creating millions of channels. */
            dictDelete(server.pubsub_channels,channel);
        }
    }
    /* Notify the client */
    if (notify) {
        addReply(c,shared.mbulkhdr[3]);
        addReply(c,shared.unsubscribebulk);
        addReplyBulk(c,channel);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+
                       listLength(c->pubsub_patterns));

    }
    decrRefCount(channel); /* it is finally safe to release it */
    return retval;
}

/* Subscribe a client to a pattern. Returns 1 if the operation succeeded, or 0 if the client was already subscribed to that pattern. */
int pubsubSubscribePattern(client *c, robj *pattern) {
    int retval = 0;

    // 在 client 的 pubsub_patterns 链表中查找需要订阅的 pattern, 未找到则添加该 pattern
    if (listSearchKey(c->pubsub_patterns,pattern) == NULL) {
        retval = 1;
        pubsubPattern *pat;
        // 将该 pattern 加入 client 的 pubsub_patterns 链表中
        listAddNodeTail(c->pubsub_patterns,pattern);
        incrRefCount(pattern);
        pat = zmalloc(sizeof(*pat));
        pat->pattern = getDecodedObject(pattern);
        pat->client = c;
        // 将该 pattern 加入 server 的 pubsub_patterns 链表中
        listAddNodeTail(server.pubsub_patterns,pat);
    }
    /* Notify the client */
    addReply(c,shared.mbulkhdr[3]);
    addReply(c,shared.psubscribebulk);
    addReplyBulk(c,pattern);
    addReplyLongLong(c,clientSubscriptionsCount(c));
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribePattern(client *c, robj *pattern, int notify) {
    listNode *ln;
    pubsubPattern pat;
    int retval = 0;

    incrRefCount(pattern); /* Protect the object. May be the same we remove */
    if ((ln = listSearchKey(c->pubsub_patterns,pattern)) != NULL) {
        retval = 1;
        listDelNode(c->pubsub_patterns,ln);
        pat.client = c;
        pat.pattern = pattern;
        ln = listSearchKey(server.pubsub_patterns,&pat);
        listDelNode(server.pubsub_patterns,ln);
    }
    /* Notify the client */
    if (notify) {
        addReply(c,shared.mbulkhdr[3]);
        addReply(c,shared.punsubscribebulk);
        addReplyBulk(c,pattern);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+
                       listLength(c->pubsub_patterns));
    }
    decrRefCount(pattern);
    return retval;
}

/* Unsubscribe from all the channels. Return the number of channels the
 * client was subscribed to. */
int pubsubUnsubscribeAllChannels(client *c, int notify) {
    dictIterator *di = dictGetSafeIterator(c->pubsub_channels); // 创建安全迭代器(即遍历期间禁止 rehash 操作)
    dictEntry *de;
    int count = 0;

    while((de = dictNext(di)) != NULL) {
        robj *channel = dictGetKey(de);

        count += pubsubUnsubscribeChannel(c,channel,notify);
    }
    /* We were subscribed to nothing? Still reply to the client. */
    if (notify && count == 0) {
        addReply(c,shared.mbulkhdr[3]);
        addReply(c,shared.unsubscribebulk);
        addReply(c,shared.nullbulk);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+
                       listLength(c->pubsub_patterns));
    }
    dictReleaseIterator(di);
    return count;
}

/* Unsubscribe from all the patterns. Return the number of patterns the
 * client was subscribed from. */
int pubsubUnsubscribeAllPatterns(client *c, int notify) {
    listNode *ln;
    listIter li;
    int count = 0;

    listRewind(c->pubsub_patterns,&li);
    while ((ln = listNext(&li)) != NULL) {
        robj *pattern = ln->value;

        count += pubsubUnsubscribePattern(c,pattern,notify);
    }
    if (notify && count == 0) {
        /* We were subscribed to nothing? Still reply to the client. */
        addReply(c,shared.mbulkhdr[3]);
        addReply(c,shared.punsubscribebulk);
        addReply(c,shared.nullbulk);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+
                       listLength(c->pubsub_patterns));
    }
    return count;
}

/* Publish a message */
int pubsubPublishMessage(robj *channel, robj *message) {
    int receivers = 0;
    dictEntry *de;
    listNode *ln;
    listIter li;

    /* Send to clients listening for that channel */
    // 从 pubsub_channels 字典中取出订阅该 channel 的客户端
    de = dictFind(server.pubsub_channels,channel);
    // 如果有订阅该 channel 的客户端, 依次向客户端发送该条消息
    if (de) {
        list *list = dictGetVal(de);
        listNode *ln;
        listIter li;

        listRewind(list,&li); // 初始化链表的迭代器(从头开始遍历)
        while ((ln = listNext(&li)) != NULL) {
            client *c = ln->value;

            addReply(c,shared.mbulkhdr[3]);
            addReply(c,shared.messagebulk);
            addReplyBulk(c,channel);
            addReplyBulk(c,message);
            receivers++;
        }
    }
    /* Send to clients listening to matching channels */
    // 遍历链表, 将推送的 channel 和每个模式比较, 如果匹配, 就将推送的消息发送给订阅该模式的客户端
    if (listLength(server.pubsub_patterns)) {
        listRewind(server.pubsub_patterns,&li);
        channel = getDecodedObject(channel);
        while ((ln = listNext(&li)) != NULL) {
            pubsubPattern *pat = ln->value;

            if (stringmatchlen((char*)pat->pattern->ptr,
                                sdslen(pat->pattern->ptr),
                                (char*)channel->ptr,
                                sdslen(channel->ptr),0)) {
                addReply(pat->client,shared.mbulkhdr[4]);
                addReply(pat->client,shared.pmessagebulk);
                addReplyBulk(pat->client,pat->pattern);
                addReplyBulk(pat->client,channel);
                addReplyBulk(pat->client,message);
                receivers++;
            }
        }
        decrRefCount(channel);
    }
    return receivers;
}

/*-----------------------------------------------------------------------------
 * Pubsub commands implementation
 *----------------------------------------------------------------------------*/

// 格式: subscribe channel [channel ...]
// 说明: 订阅命令(subscribe) 的作用是订阅指定的通道.
void subscribeCommand(client *c) {
    int j;

    // 依次对每个订阅的 channel 执行订阅操作
    for (j = 1; j < c->argc; j++)
        pubsubSubscribeChannel(c,c->argv[j]);
    c->flags |= CLIENT_PUBSUB; // 为该客户端添加 CLIENT_PUBSUB 标志, 表示进入 pub/sub 模式
}

// 格式: unsubscribe [channel [channel ...]]
// 说明: 取消订阅命令(unsubscribe) 的作用是取消订阅某个通道, 如果不指定, 则该客户端所有的订阅都会取消
void unsubscribeCommand(client *c) {
    if (c->argc == 1) { // 未指定 channel, 则取消该客户端所有 channel 的订阅
        pubsubUnsubscribeAllChannels(c,1);
    } else {
        int j;

        // 依次取消订阅的 channel
        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribeChannel(c,c->argv[j],1);
    }
    // 如果客户端不再订阅任何 channel, 则退出 pub/sub 模式
    if (clientSubscriptionsCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
}

// 格式: psubscribe pattern [pattern ...]
// 说明: 订阅指定模式通道命令(psubscribe) 的作用是订阅由指定模式表示的所有通道, 匹配该模式的
//       所有通道发送的消息都会被接收.
// psubscribe 执行流程:
// 1. 在 client 的 pubsub_patterns 链表中查找该 pattern, 如果找到说明已经订阅, 则直接返回;
// 2. 否则将 pattern 加入 client 的 pubsub_patterns 和 server 的 pubsub_patterns 链表中
void psubscribeCommand(client *c) {
    int j;

    for (j = 1; j < c->argc; j++)
        pubsubSubscribePattern(c,c->argv[j]);
    c->flags |= CLIENT_PUBSUB;
}

// 格式: punsubscribe [pattern [pattern ...]]
// 说明: 取消订阅指定通道命令(punsubscribe) 用于取消订阅指定的模式, 如果没有指定, 则
//       取消所有该客户端订阅的模式.
void punsubscribeCommand(client *c) {
    if (c->argc == 1) { // 未指定, 则取消该客户端订阅的所有模式
        pubsubUnsubscribeAllPatterns(c,1);
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribePattern(c,c->argv[j],1);
    }
    if (clientSubscriptionsCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
}

// 格式: publish channel message
// 说明: 发布命令(publish) 的作用是向指定 channel 发送消息
// publish 执行流程:
// 1. 从 pubsub_channels 字典中以推送到的 channel 为 key, 取出所有订阅了该 channel 的客户端,
//    依次向每个客户端返回推送的数据.
// 2. 依次遍历 pubsub_patterns, 将链表中每个节点的模式字段 pattern 和推送的 channel 进行比较,
//    如果能够匹配, 则向节点中订阅了该 pattern 的客户端返回推送的数据.
void publishCommand(client *c) {
    int receivers = pubsubPublishMessage(c->argv[1],c->argv[2]);
    // 集群下, 将 publish 消息同步到从服务器中. 这样, 如果一个客户端订阅了从服务器的 channel,
    // 在主服务器中向该 channel 推送消息时, 该客户端也能收到推送的消息.
    if (server.cluster_enabled)
        clusterPropagatePublish(c->argv[1],c->argv[2]);
    else
        forceCommandPropagation(c,PROPAGATE_REPL);
    addReplyLongLong(c,receivers);
}

// 格式: pubsub subcommand [argument [argument ...]]
// 说明: 查看订阅状态命令(pubsub) 用来查看所有发布订阅的相关状态.
/* PUBSUB command for Pub/Sub introspection. */
void pubsubCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"CHANNELS [<pattern>] -- Return the currently active channels matching a pattern (default: all).",
"NUMPAT -- Return number of subscriptions to patterns.",
"NUMSUB [channel-1 .. channel-N] -- Returns the number of subscribers for the specified channels (excluding patterns, default: none).",
NULL
        };
        addReplyHelp(c, help);

    // CHANNELS 子命令: 返回该客户端订阅的所有 channels 列表(未指定 pattern 时), 
    // 否则只返回匹配该 pattern 的 channels 列表
    } else if (!strcasecmp(c->argv[1]->ptr,"channels") &&
        (c->argc == 2 || c->argc == 3))
    {
        /* PUBSUB CHANNELS [<pattern>] */
        sds pat = (c->argc == 2) ? NULL : c->argv[2]->ptr;
        dictIterator *di = dictGetIterator(server.pubsub_channels); // 创建迭代器 
        dictEntry *de;
        long mblen = 0;
        void *replylen;

        replylen = addDeferredMultiBulkLength(c);
        while((de = dictNext(di)) != NULL) {
            robj *cobj = dictGetKey(de);
            sds channel = cobj->ptr;

            // 根据是否指定模式, 返回所有订阅的 channels 或匹配 pattern 的 channels
            if (!pat || stringmatchlen(pat, sdslen(pat),
                                       channel, sdslen(channel),0))
            {
                addReplyBulk(c,cobj);
                mblen++;
            }
        }
        dictReleaseIterator(di);
        setDeferredMultiBulkLength(c,replylen,mblen);

    // NUMSUB 子命令: 返回所有订阅了指定 channel 的客户端个数
    } else if (!strcasecmp(c->argv[1]->ptr,"numsub") && c->argc >= 2) {
        /* PUBSUB NUMSUB [Channel_1 ... Channel_N] */
        int j;

        addReplyMultiBulkLen(c,(c->argc-2)*2);
        for (j = 2; j < c->argc; j++) {
            list *l = dictFetchValue(server.pubsub_channels,c->argv[j]);

            addReplyBulk(c,c->argv[j]);
            addReplyLongLong(c,l ? listLength(l) : 0);
        }

    // NUMPAT 子命令: 返回模式订阅的个数
    } else if (!strcasecmp(c->argv[1]->ptr,"numpat") && c->argc == 2) {
        /* PUBSUB NUMPAT */
        addReplyLongLong(c,listLength(server.pubsub_patterns));
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
