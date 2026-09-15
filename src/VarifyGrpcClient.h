#pragma once
#include "const.h"
#include <grpcpp/grpcpp.h>
#include "../message.pb.h"          // ← 先：消息类 GetVarifyReq / GetVarifyRsp
#include "../message.grpc.pb.h"     // ← 后：服务类 VarifyService / Stub（依赖上面的消息）
#include "Singleton.h"
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
* message.pb.h 负责"数据"——请求长什么样、响应长什么样；
* message.grpc.pb.h 负责"通信"——怎么把数据发出去、收回来
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
using grpc::Channel;//到服务器的"通道"
using grpc::Status; // 一次 RPC 调用的结果状态
using grpc::ClientContext;  // 一次调用的上下文（可放超时、元数据等）

using message::GetVarifyReq;// 请求类型
using message::GetVarifyRsp;// 响应类型
using message::VarifyService;// 服务类型


class RPCConPool {
public:
    RPCConPool(size_t poolSize, std::string host, std::string port);
    ~RPCConPool();
    void Close();
    std::unique_ptr<VarifyService::Stub> getConnection();
    void returnConnection(std::unique_ptr<VarifyService::Stub> context);
private:
    size_t poolSize_;
    std::string host_;
    std::string port_;
    //std::atomic<bool>保证bool本身读写是原子操作
    std::atomic<bool> b_stop_;//标记是否回收
    //用队列管理，线程不安全，可以用互斥锁实现线程安全
    //队列管理指向Stub的智能指针
    std::queue<std::unique_ptr<VarifyService::Stub>> connections_;
    std::mutex mutex_;
    //条件变量
    std::condition_variable condition_variable_;
    
};





/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * 
* CRTP单例全程序只有一个客户端实例（一个通道复用比每次新建连接高效）
* 
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
class VerifyGrpcClient :public Singleton<VerifyGrpcClient>
{
    //CRTP经典设置父类为友元，父类调用子类方法
    friend class Singleton<VerifyGrpcClient>;
public:

    //在约定中 GetVarifyReq里 string email = 1;protobuf 规定每个字段必须有个编号
    //grpc 在每次发送的时候还要三个参数 context;reply;request;
    //我写的包装函数把GetVarifyReq需要的email放到了request
    //然后用stub_->GetVarifyCode(&context, request, &reply);调用函数
    //status：是函数的返回值表示状态
    //gRPC 只是在收到服务器的响应后，把字节反序列化写进reply
    GetVarifyRsp GetVarifyCode(std::string email);

private:
    VerifyGrpcClient();

    std::unique_ptr<RPCConPool> pool_;
};
    /* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
    * 1.Stub 是"远程函数的本地替身"，让你像调本地函数一样调远程函数（设计模式里叫 Proxy / 代理模式）。
    * 2.因为 VerifyGrpcClient是单例所以stub_也只有一个，现在我们改成了多线程
    *   但是我们只有一个stub_，多线程访问这唯一一个stub_会发生错误
    * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */