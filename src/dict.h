/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdint.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)

// dictEntry 表示 Hash 表中的元素, 用于存储键值对
typedef struct dictEntry {
    void *key;         // 存储键
    union {
        void *val;     // db.dict 中的 val
        uint64_t u64;
        int64_t s64;   // db.expires 中存储过期时间
        double d;
    } v;               // 值, 是个联合体
    struct dictEntry *next; // 当 Hash 冲突时, 指向冲突的元素, 形成单链表
} dictEntry;

// dictType 包含对字典进行操作的函数指针
typedef struct dictType {
    uint64_t (*hashFunction)(const void *key);        // 该字典对应的 Hash 函数
    void *(*keyDup)(void *privdata, const void *key); // 键对应的复制函数
    void *(*valDup)(void *privdata, const void *obj); // 值对应的复制函数
    int (*keyCompare)(void *privdata, const void *key1, const void *key2); // 键的对比函数
    void (*keyDestructor)(void *privdata, void *key); // 键的销毁函数
    void (*valDestructor)(void *privdata, void *obj); // 值的销毁函数
} dictType;

// dictht 哈希表
// Hash 表的结构体整体占用 32 字节, 其中 table 字段是数组, 作用是存储键值对, 该数组中的元素
// 指向的是 dictEntry 的结构体, 每个 dictEntry 里面存有键值对.
/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
typedef struct dictht {
    dictEntry **table;      // 指针数组, 用于存储键值对
    unsigned long size;     // table 数组的大小
    unsigned long sizemask; // 掩码 = size - 1, 用于计算键的索引值(因为索引值是键 Hash 值与数组总容量取余后的值)
    unsigned long used;     // table 数组已存元素个数, 包含 next 单链表的数据
} dictht;

// dict Redis 在两个结构体 Hash 表及 Hash 表节点外, 还在最外面层封装了一个叫做字典 dict 的
// 数据结构, 其主要作用是对散列表再进行一层封装, 当字典需要进行一些特殊操作时需要用到里面
// 的辅助字段.
// dict 结构体占用 96 字节
typedef struct dict {
    dictType *type;  // 该字典对应的特定操作函数
    void *privdata;  // 该字典依赖的数据, 配合 type 字段指向的函数一起使用
    // 通常使用 ht[0], 只有当该字典扩容、缩容需要进行 rehash 时, 才会用到 ht[1]
    dictht ht[2];    // Hash 表, 键值对存储在此
    // rehash 标识. 默认值为 -1, 代表没进行 rehash 操作; 不为 -1 时, 代表正进行
    // rehash 操作, 存储的值表示 Hash 表 ht[0] 的 rehash 操作进行到了哪个索引值
    long rehashidx;
    // 当前运行的安全迭代器数. 当有安全迭代器绑定到该字典时, 会暂停 rehash 操作
    unsigned long iterators;
} dict;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {
    dict *d;         // 迭代的字典
    long index;      // 当前迭代到 Hash 表中哪个索引值
    // table 用于表示当前正在迭代的 Hash 表, 即 ht[0] 与 ht[1];
    // safe 用于表示当前创建的是否为安全迭代器
    int table, safe;
    dictEntry *entry, *nextEntry; // 分别表示 当前节点; 下一个节点
    /* unsafe iterator fingerprint for misuse detection. */
    long long fingerprint; // 字典的指纹, 当字典未发生改变时, 该值不变, 发生改变时则值也随着改变
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
typedef void (dictScanBucketFunction)(void *privdata, dictEntry **bucketref);

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        (entry)->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        (entry)->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do { (entry)->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { (entry)->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \
    do { (entry)->v.d = _val_; } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        (entry)->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        (entry)->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

// dictHashKey 调用该键的 Hash 函数得到键的 Hash 值
#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
#define dictIsRehashing(d) ((d)->rehashidx != -1)

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);
int dictExpand(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);
dictEntry *dictAddOrFind(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
int dictDelete(dict *d, const void *key);
dictEntry *dictUnlink(dict *ht, const void *key);
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictGetStats(char *buf, size_t bufsize, dict *d);
uint64_t dictGenHashFunction(const void *key, int len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void*));
void dictEnableResize(void);
void dictDisableResize(void);
int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(uint8_t *seed);
uint8_t *dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);
uint64_t dictGetHash(dict *d, const void *key);
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
