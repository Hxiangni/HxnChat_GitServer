#pragma once
// hiredis 裸 C API：redisConnect / redisCommand / redisReply / freeReplyObject 全在这
#include <hiredis/hiredis.h>
#include <cstdio>     // printf
#include <cstring>    // strcmp
#include <string>     // std::string

// 头文件里的非模板函数必须加 inline：
// 否则一旦有两个 .cpp 都 include 了本文件，TestRedis 会被定义两份 → 链接报 LNK2005 重定义
// inline 的语义是"允许多重定义，链接时合并为一份"，正是头文件函数需要的
inline void TestRedis() {
    // 连接 redis：需要 redis-server 已经启动才可以连接
    // redis 默认监听端口为 6379，可在 redis.conf 中修改
    // 本机服务跑在默认 6379 上（redis-cli -p 6379 -a 123456 ping 已验证返回 PONG）
    redisContext* c = redisConnect("127.0.0.1", 6379);
    // 注意：redisConnect 在【内存分配失败】时会返回 NULL（此时连 c->err 都不能访问）
    //      在【连接失败】时返回非空对象，但 err 字段被置位 —— 两种失败要分开判断
    if (c == nullptr || c->err)
    {
        printf("Connect to redisServer faile:%s\n", c ? c->errstr : "alloc redisContext failed");
        redisFree(c);
        return;
    }
    printf("Connect to redisServer Success\n");

    // redis 配置了 requirepass，所以先 AUTH 认证，密码 123456
    std::string redis_password = "123456";
    // ⚠️ 必须传 .c_str()：redisCommand 是 printf 风格的可变参数函数，
    // %s 期望的是 const char*（指向字符数组的指针）。
    // 直接传 std::string 对象是未定义行为——尤其 MSVC 的 Debug STL 里，
    // string 对象开头是迭代器调试的隐藏指针，%s 会把一串二进制垃圾当密码发出去 → WRONGPASS
    redisReply* r = (redisReply*)redisCommand(c, "AUTH %s", redis_password.c_str());
    if (r->type == REDIS_REPLY_ERROR) {
        // 打印服务器返回的具体错误原因（WRONGPASS / NOAUTH 等），别把原因吞掉
        printf("Redis认证失败：%s\n", r->str);
    }
    else {
        printf("Redis认证成功！\n");
    }
    // 铁律：每一次 redisCommand 的返回 reply 用完都必须 freeReplyObject，
    // 否则内存泄漏（AUTH 这次的 reply 之前漏掉了）
    freeReplyObject(r);

    // 为redis设置key
    const char* command1 = "set stest1 value1";

    //执行redis命令行
    r = (redisReply*)redisCommand(c, command1);

    //如果返回NULL则说明执行失败（连接断开等底层错误；注意和业务错误的区别）
    if (NULL == r)
    {
        printf("Execut command1 failure\n");
        redisFree(c);        return;
    }

    //如果执行失败则释放连接（打印服务器返回的错误原因，如 NOAUTH Authentication required）
    if (!(r->type == REDIS_REPLY_STATUS && (strcmp(r->str, "OK") == 0 || strcmp(r->str, "ok") == 0)))
    {
        printf("Failed to execute command[%s], reason: %s\n", command1, r->str);
        freeReplyObject(r);
        redisFree(c);        return;
    }

    //执行成功 释放redisCommand执行后返回的redisReply所占用的内存
    freeReplyObject(r);
    printf("Succeed to execute command[%s]\n", command1);

    const char* command2 = "strlen stest1";
    r = (redisReply*)redisCommand(c, command2);

    //如果返回类型不是整形 则释放连接
    if (r->type != REDIS_REPLY_INTEGER)
    {
        printf("Failed to execute command[%s]\n", command2);
        freeReplyObject(r);
        redisFree(c);        return;
    }

    //获取字符串长度
    int length = r->integer;
    freeReplyObject(r);
    printf("The length of 'stest1' is %d.\n", length);
    printf("Succeed to execute command[%s]\n", command2);

    //获取redis键值对信息
    const char* command3 = "get stest1";
    r = (redisReply*)redisCommand(c, command3);
    if (r->type != REDIS_REPLY_STRING)
    {
        printf("Failed to execute command[%s]\n", command3);
        freeReplyObject(r);
        redisFree(c);        return;
    }
    printf("The value of 'stest1' is %s\n", r->str);
    freeReplyObject(r);
    printf("Succeed to execute command[%s]\n", command3);

    //查询一个不存在的 key：redis 正常返回 NIL 类型（不是错误！）
    //所以这里判断的是 REDIS_REPLY_NIL，走到了才算测试通过
    const char* command4 = "get stest2";
    r = (redisReply*)redisCommand(c, command4);
    if (r->type != REDIS_REPLY_NIL)
    {
        printf("Failed to execute command[%s]\n", command4);
        freeReplyObject(r);
        redisFree(c);        return;
    }
    freeReplyObject(r);
    printf("Succeed to execute command[%s]\n", command4);

    //释放连接资源
    redisFree(c);
}
