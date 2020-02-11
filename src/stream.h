#ifndef STREAM_H
#define STREAM_H

#include "rax.h"
#include "listpack.h"

/* Stream item ID: a 128 bit number composed of a milliseconds time and
 * a sequence counter. IDs generated in the same millisecond (or in a past
 * millisecond if the clock jumped backward) will use the millisecond time
 * of the latest generated ID and an incremented sequence. */
typedef struct streamID {
    uint64_t ms;        /* Unix time in milliseconds. */
    uint64_t seq;       /* Sequence number. */
} streamID;

typedef struct stream {
    // rax 存储消息生产者生产的具体消息, 每个消息有唯一的 ID. 以消息 ID 为键,
    // 消息内容为值存储在 rax 中, 值得注意的是, rax 中的一个节点可能存储多个消息
    rax *rax;               /* The radix tree holding the stream. */
    // 代表当前 stream 中的消息个数(不包括已经删除的消息)
    uint64_t length;        /* Number of elements inside this stream. */
    // 当前 stream 中最后插入的消息的 ID, stream 为空时, 设置为 0
    streamID last_id;       /* Zero if there are yet no items. */
    // 消费组. 消费组是 Stream 中的一个重要的概念, 每个 stream 会有多个消费组, 每个消费组
    // 通过组名称进行唯一标识, 同时关联一个 streamGG 结构
    rax *cgroups;           /* Consumer groups dictionary: name -> streamCG */
} stream;

/* We define an iterator to iterate stream items in an abstract way, without
 * caring about the radix tree + listpack representation. Technically speaking
 * the iterator is only used inside streamReplyWithRange(), so could just
 * be implemented inside the function, but practically there is the AOF
 * rewriting code that also needs to iterate the stream to emit the XADD
 * commands. */
typedef struct streamIterator {
    // 当前迭代器正在遍历的消息流
    stream *stream;         /* The stream we are iterating. */
    // 消息内容实际存储在 listpack 中, 每个 listpack 都有一个 master entry(也就是
    // 第一个插入的消息), master_id 为该消息 id
    streamID master_id;     /* ID of the master entry at listpack head. */
    // master entry 中 field 域的个数
    uint64_t master_fields_count;       /* Master entries # of fields. */
    // master entry field 域存储的首地址
    unsigned char *master_fields_start; /* Master entries start in listpack. */
    // 当 listpack 中消息的 field 域与 master entry 的 field 域完全相同时, 该消息会复用
    // master entry 的 field 域, 在我们遍历该消息时, 需要记录当前所在的 field 域的具体位置,
    // master_fields_ptr 就是实现这个功能
    unsigned char *master_fields_ptr;   /* Master field to emit next. */
    // 当前遍历的消息的标志位
    int entry_flags;                    /* Flags of entry we are emitting. */
    // 代表当前迭代器的方向
    int rev;                /* True if iterating end to start (reverse). */
    // start_key, end_key 为该迭代器处理的消息 ID 的范围
    uint64_t start_key[2];  /* Start key as 128 bit big endian. */
    uint64_t end_key[2];    /* End key as 128 bit big endian. */
    // rax 迭代器, 用于遍历 rax 中所有的 key
    raxIterator ri;         /* Rax iterator. */
    // 当前 listpack 指针
    unsigned char *lp;      /* Current listpack. */
    // 当前正在遍历的 listpack 中的元素
    unsigned char *lp_ele;  /* Current listpack cursor. */
    // 指向当前消息的 flags 域
    unsigned char *lp_flags; /* Current entry flags pointer. */
    /* Buffers used to hold the string of lpGet() when the element is
     * integer encoded, so that there is no string representation of the
     * element inside the listpack itself. */
    // filed_buf, value_buf 用于从 listpack 读取数据时的缓存
    unsigned char field_buf[LP_INTBUF_SIZE];
    unsigned char value_buf[LP_INTBUF_SIZE];
} streamIterator;

/* Consumer group. */
typedef struct streamCG {
    // 为该消费组已经确认的最后一个消息的 ID
    streamID last_id;       /* Last delivered (not acknowledged) ID for this
                               group. Consumers that will just ask for more
                               messages will served with IDs > than this. */
    // 为该消费组尚未确认的消息, 并以消息 ID 为键, streamNACK(代表一个尚未确认的消息)为值
    rax *pel;               /* Pending entries list. This is a radix tree that
                               has every message delivered to consumers (without
                               the NOACK option) that was yet not acknowledged
                               as processed. The key of the radix tree is the
                               ID as a 64 bit big endian number, while the
                               associated value is a streamNACK structure.*/
    // 为该消费组中所有的消费者, 并以消费者的名称为键, streamConsumer(代表一个消费者)为值
    rax *consumers;         /* A radix tree representing the consumers by name
                               and their associated representation in the form
                               of streamConsumer structures. */
} streamCG;

/* A specific consumer in a consumer group.  */
typedef struct streamConsumer {
    // 为该消费者最后一次活跃的时间
    mstime_t seen_time;         /* Last time this consumer was active. */
    // 消费者的名称
    sds name;                   /* Consumer name. This is how the consumer
                                   will be identified in the consumer group
                                   protocol. Case sensitive. */
    // 为该消费者尚未确认的消息, 以消息 ID 为键, streamNACK 为值
    rax *pel;                   /* Consumer specific pending entries list: all
                                   the pending messages delivered to this
                                   consumer not yet acknowledged. Keys are
                                   big endian message IDs, while values are
                                   the same streamNACK structure referenced
                                   in the "pel" of the conumser group structure
                                   itself, so the value is shared. */
} streamConsumer;

/* Pending (yet not acknowledged) message in a consumer group. */
// streamNACK 维护了消费组或者消费者中尚未确认的消息, 值得注意的是, 消费组中的 pel 的元素
// 与每个消费者的 pel 中的元素是共享的, 即该消费组消费了某个消息, 这个消息会同时放到消费组
// 以及该消费者的 pel 队列中, 并且二者是同一个 streamNACK 结构
typedef struct streamNACK {
    // 为该消息最后发送给消费方的时间
    mstime_t delivery_time;     /* Last time this message was delivered. */
    // 为该消息已经发送的次数(组内的成员可以通过 xclaim 命令获取某个消息的处理权, 
    // 该消息已经分给组内另一个消费者当其并没有确认该消息)
    uint64_t delivery_count;    /* Number of times this message was delivered.*/
    // 该消息当前归属的消费者
    streamConsumer *consumer;   /* The consumer this message was delivered to
                                   in the last delivery. */
} streamNACK;

/* Stream propagation informations, passed to functions in order to propagate
 * XCLAIM commands to AOF and slaves. */
typedef struct sreamPropInfo {
    robj *keyname;
    robj *groupname;
} streamPropInfo;

/* Prototypes of exported APIs. */
struct client;

stream *streamNew(void);
void freeStream(stream *s);
size_t streamReplyWithRange(client *c, stream *s, streamID *start, streamID *end, size_t count, int rev, streamCG *group, streamConsumer *consumer, int flags, streamPropInfo *spi);
void streamIteratorStart(streamIterator *si, stream *s, streamID *start, streamID *end, int rev);
int streamIteratorGetID(streamIterator *si, streamID *id, int64_t *numfields);
void streamIteratorGetField(streamIterator *si, unsigned char **fieldptr, unsigned char **valueptr, int64_t *fieldlen, int64_t *valuelen);
void streamIteratorStop(streamIterator *si);
streamCG *streamLookupCG(stream *s, sds groupname);
streamConsumer *streamLookupConsumer(streamCG *cg, sds name, int create);
streamCG *streamCreateCG(stream *s, char *name, size_t namelen, streamID *id);
streamNACK *streamCreateNACK(streamConsumer *consumer);
void streamDecodeID(void *buf, streamID *id);
int streamCompareID(streamID *a, streamID *b);

#endif
