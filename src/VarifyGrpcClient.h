#pragma once
#include <grpcpp/grpcpp.h>
#include "../message.pb.h"          // ← 先：消息类 GetVarifyReq / GetVarifyRsp
#include "../message.grpc.pb.h"     // ← 后：服务类 VarifyService / Stub（依赖上面的消息）
#include "const.h"
#include "Singleton.h"

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
    GetVarifyRsp GetVarifyCode(std::string email) {
        ClientContext context; //  本次调用的上下文（相当于"这通电话的会话记录"）
        GetVarifyRsp reply;  //  准备一个空信封，等着装对方的回复
        GetVarifyReq request; // 准备请求内容
        request.set_email(email);//  把参数填进去（set_email 是 protobuf 生成的 setter）

        //真正发起调用：这一行会【阻塞】，直到服务器返回
        Status status = stub_->GetVarifyCode(&context, request, &reply);

        if (status.ok()) {//status.ok() 只代表"传输成功"，不代表"业务成功"。

            return reply;// 成功把服务器返回的数据交给调用方
        }
        else {
            reply.set_error(ErrorCodes::RPCFailed);// 塞一个业务错误码
            return reply;
        }
    }

private:
    VerifyGrpcClient() {
        //建一条"通道"（Channel）
        //InsecureChannelCredentials() = 不加密（明文 HTTP/2）只适合本机开发调试。生产环境要换成 SslCredentials(...) 之类。
        //返回值类型是 std::shared_ptr<Channel>
        std::shared_ptr<Channel> channel = grpc::CreateChannel("127.0.0.1:50051", grpc::InsecureChannelCredentials());
        //用通道造一个"存根"（Stub）
        //Stub 内部会把 channel 存一份（它得知道往哪发请求），所以传进去的 shared_ptr 被复制了一份，引用计数 +1。
        stub_ = VarifyService::NewStub(channel);
    }

    //Stub 是"远程函数的本地替身"，让你像调本地函数一样调远程函数（设计模式里叫 Proxy / 代理模式）。
    std::unique_ptr<VarifyService::Stub> stub_;
};