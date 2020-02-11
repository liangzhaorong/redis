/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
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

#ifndef __AE_H__
#define __AE_H__

#include <time.h>

#define AE_OK 0
#define AE_ERR -1

#define AE_NONE 0       /* No events registered. */
#define AE_READABLE 1   /* Fire when descriptor is readable. */
#define AE_WRITABLE 2   /* Fire when descriptor is writable. */
#define AE_BARRIER 4    /* With WRITABLE, never fire the event if the
                           READABLE event already fired in the same event
                           loop iteration. Useful when you want to persist
                           things to disk before sending replies, and want
                           to do that in a group fashion. */

#define AE_FILE_EVENTS 1
#define AE_TIME_EVENTS 2
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT 4
#define AE_CALL_AFTER_SLEEP 8

#define AE_NOMORE -1
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure: 文件事件结构体 */
typedef struct aeFileEvent {
    // 存储监控的文件事件类型, 如 AE_(READABLE|WRITABLE|BARRIER)
    int mask; /* one of AE_(READABLE|WRITABLE|BARRIER) */
    aeFileProc *rfileProc; // 指向读事件处理函数
    aeFileProc *wfileProc; // 指向写事件处理函数
    void *clientData;      // 指向对应的客户端对象
} aeFileEvent;

/* Time event structure: 时间事件结构体 */
typedef struct aeTimeEvent {
    long long id;   // 时间事件唯一 ID, 通过字段 eventLoop->timeEventNextId 实现
    long when_sec;  // 时间事件触发的秒数
    long when_ms;   // 时间事件触发的毫秒数
    aeTimeProc *timeProc;                // 时间事件处理函数
    aeEventFinalizerProc *finalizerProc; // 函数指针, 删除时间事件节点前会调用此函数
    void *clientData;          // 指向对应的客户端对象
    struct aeTimeEvent *prev;  // 时间事件表采用链表维护
    struct aeTimeEvent *next;
} aeTimeEvent;

/* A fired event: 触发事件 */
typedef struct aeFiredEvent {
    int fd;   // 发生事件的 socket 文件描述符
    int mask; // 发生的事件类型, 如 AE_READABLE 可读事件和 AE_WRITEABLE 可写事件
} aeFiredEvent;

/* State of an event based program: 事件循环结构体 */
typedef struct aeEventLoop {
    // 当前已注册的最大文件描述符
    int maxfd;   /* highest file descriptor currently registered */
    // 事件循环结构体中可跟踪的文件描述符的最大数量
    int setsize; /* max number of file descriptors tracked */
    // 记录最大的定时事件 id+1
    long long timeEventNextId;
    // 用于系统时间的矫正
    time_t lastTime;     /* Used to detect system clock skew */
    // 文件事件表, 存储已注册的文件事件(socket 的可读可写事件)
    aeFileEvent *events; /* Registered events */
    // 存储被触发的文件事件
    aeFiredEvent *fired; /* Fired events */
    // 定时事件表
    aeTimeEvent *timeEventHead;
    // 事件循环结束标志
    int stop;
    // Redis 底层可以使用 4 种 I/O 多路复用模型(kqueu, epoll 等), apidata 是对这 
    // 4 种模型的进一步封装.
    void *apidata; /* This is used for polling API specific data */
    // Redis 服务器需要阻塞等待文件事件的发生, 进程阻塞之前会调用 beforesleep 函数,
    // 进程因为某种原因被唤醒后会调用 aftersleep 函数
    aeBeforeSleepProc *beforesleep;
    // 新的循环后需要执行的操作
    aeBeforeSleepProc *aftersleep;
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);

#endif
