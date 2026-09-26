#include "VarifyGrpcClient.h"
#include "ConfigMgr.h"
#include <spdlog/spdlog.h>
RPCConPool::RPCConPool(size_t poolSize, std::string host, std::string port)
	:poolSize_(poolSize), host_(host), port_(port),b_stop_(false)
{
	for (size_t i = 0; i < poolSize_; i++)
	{
        //建一条"通道"（Channel）后续用返回的Channel构造Stub
        //InsecureChannelCredentials() = 不加密（明文 HTTP/2）只适合本机开发调试。生产环境要换成 SslCredentials(...) 之类。
        //返回值类型是 std::shared_ptr<Channel>
        std::shared_ptr<Channel> channel = grpc::CreateChannel(host+ ":"+ port, grpc::InsecureChannelCredentials());
        //Stub 内部会把 channel 存一份（它得知道往哪发请求），所以传进去的 shared_ptr 被复制了一份，引用计数 +1。
        connections_.push(VarifyService::NewStub(channel));

       
        /* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
         * auto stub = VarifyService::NewStub(channel);
         * connections_.push(stub);
         * 写上面这个会报错为什么
         * connections_.push(VarifyService::NewStub(channel));这就不报错是为啥捏
         * 函数返回「值类型（不带 &、&&）」，返回值是一个纯右值 (prvalue)，属于右值大类。
         * 会调用push的移动构造
         * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

	}
}

RPCConPool::~RPCConPool()
{
    /* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
    * lock_guard是RAII自动锁
    * 构造的时候：lock_guard内部调用 mutex_.lock()，拿到互斥锁(拿不到一直拿）
    * 离开当前作用域（大括号结束、return、异常），自动析构，调用 mutex_.unlock()
    * 多线程环境下，同一时间只有一个线程能进入这块代码操作 `connections_`
    * `lock_guard` 不能手动 unlock，生命周期绑定作用域。
    * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
    std::lock_guard<std::mutex> lock(mutex_);
    Close();
    while (!connections_.empty()) {//队列不为空就继续循环
        connections_.pop();//只删除头部，没有返回值
    }
}

void RPCConPool::Close()
{
    //原子写
    b_stop_ = true;
    //唤醒所有等待在这个 cv 上的线程。适合：服务关闭、清空队列，全部唤醒
    condition_variable_.notify_all();
}

std::unique_ptr<VarifyService::Stub> RPCConPool::getConnection()
{
    std::unique_lock<std::mutex> lock(mutex_);
    //.wait(参数1，访问条件)
    //因为要访问共享变量！所以在.wait前要先获取锁
    //谓词 lambda 是wait 内部帮你反复执行的
    condition_variable_.wait(lock, [this]() {
        if (b_stop_) {
            return true;
        }
        //队列不为空返回false,加个！返回ture，wait条件满足！
        return !connections_.empty();
        });

    if (b_stop_) {//停止服务了
        return nullptr;
    }

    auto context = std::move(connections_.front());
    connections_.pop();
    return context;
}

void RPCConPool::returnConnection(std::unique_ptr<VarifyService::Stub> context)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (b_stop_) {
        return;
    }

    connections_.push(std::move(context));
    //lock.unlock();                 // 先解锁，再唤醒：减少"醒来又抢不到锁"的空转
    condition_variable_.notify_one();
}

GetVarifyRsp VerifyGrpcClient::GetVarifyCode(std::string email)
{
    ClientContext context; //  本次调用的上下文（相当于"这通电话的会话记录"）
    GetVarifyRsp reply;  //  准备一个空信封，等着装对方的回复
    GetVarifyReq request; // 准备请求内容
    request.set_email(email);//  把参数填进去（set_email 是 protobuf 生成的 setter）

    auto stub = pool_->getConnection();
    if (nullptr == stub) {
        reply.set_error(ErrorCodes::RPCFailed);
        return reply;
    }
    //真正发起调用：这一行会【阻塞】，直到服务器返回
    Status status = stub->GetVarifyCode(&context, request, &reply);

     if (!status.ok()) {
        // ① 传输层失败：网络断了 / VarifyServer 没启动 / 超时
        //    status.error_message() 带着具体原因，一定要打出来
        spdlog::error("rpc call failed, email is {}, err msg is {}",
                      email, status.error_message());
        pool_->returnConnection(std::move(stub));
        reply.set_error(ErrorCodes::RPCFailed);
        return reply;
    }

    // ② 传输成功，看业务结果（VarifyServer 塞在回包里的 error 码）
    if (reply.error() != ErrorCodes::Success) {
        spdlog::error("send verify code failed, email is {}, error code is {}",
                      email, reply.error());
    }
    else {
        spdlog::info("send verify code success, email is {}", reply.email());
    }

    pool_->returnConnection(std::move(stub));
    return reply;
}

VerifyGrpcClient::VerifyGrpcClient()
{
    auto& gCfgMgr = ConfigMgr::Inst();
    auto host=gCfgMgr["VarifyServer"]["Host"];
    auto port=gCfgMgr["VarifyServer"]["Port"];
    pool_.reset(new RPCConPool(5, host, port));
    
}
