#include "CServer.h"
#include "HttpConnection.h"
#include "AsioIOServicePool.h"
#include <spdlog/spdlog.h>
//先分配空间再初始化列表再构造函数
CServer::CServer(boost::asio::io_context& ioc, unsigned short& port) :
    _ioc(ioc),//引用绑定
    //_acceptor有参构造内部自动做了 open → bind → listen。
    // 端口已经开放，外部客户端可以发起连接请求，内核完成三次握手放进操作系统内核的全连接队列
    _acceptor(ioc, tcp::endpoint(tcp::v4(), port))
    //_socket(ioc) //仅仅创建一个空 socket 对象，底层还没有操作系统的 fd 文件描述符。
{

}

//发起一次异步接受客户端连接。
void CServer::Start()
{
    //获取当前对象的智能指针，self 持有 CServer 对象的 shared_ptr，引用计数++
    auto self = shared_from_this();

    //从线程池轮询一个 io_context：这条连接以后所有的读写都归它管
    auto& io_context = AsioIOServicePool::GetInstance()->GetIOService();
    std::shared_ptr<HttpConnection> new_con = std::make_shared<HttpConnection>(io_context);

    //acceptor向操作系统注册 IO 事件、向多路复用 (epoll) 注册监听 fd 就绪事件，保存回调。
    //异步等待客户端发来的连接建立请求(SYN包)
    //收到连接后更新socket信息（第一个参数)第一个socket为这次通信
    _acceptor.async_accept(new_con->GetSocket(), [self,new_con](beast::error_code ec) {
        try {
            //出错则放弃这个连接，继续监听新链接
            if (ec) {
                self->Start();
                return;
            }

            //处理新链接，创建HpptConnection类管理新连接
           // std::make_shared<HttpConnection>(std::move(self->_socket))->Start();
            //继续监听_socket已经移动语义转走了现在是空的
            //self->Start();

            new_con->Start();
            // ⚠️ 关键：立刻重新发起监听，等下一个客户端
            self->Start();
        }
        catch (std::exception& exp) {
            spdlog::debug("exception is {}",exp.what());
            self->Start();
        }
        });
}