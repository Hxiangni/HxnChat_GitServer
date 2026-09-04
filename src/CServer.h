#pragma once
#include "const.h"

class CServer :public std::enable_shared_from_this<CServer>
{
public:
    CServer(boost::asio::io_context& ioc, unsigned short& port);
    void Start();//监听新链接
private:

    net::io_context& _ioc;



    /*tcp::acceptor 是 服务端专用的类，作用是监听端口、接受客户端的连接请求然后给这个客户端分配一条新的连接。
     * bind() —— 绑定到某个地址端口
     * listen() —— 开始监听
     * accept() / async_accept() —— 接受连接，生成一个 tcp::socket*/
    tcp::acceptor  _acceptor;

    /*boost::asio::ip::tcp::socket 是 Boost.Asio 库中用于 TCP 网络通信 的核心类。
     *构造时必须传入一个 io_context（或 io_service），所有 I/O 操作都由它调度
     * 封装了一个操作系统底层的 TCP socket（文件描述符），构造时创建，析构时自动关闭，防止资源泄漏
     * 记录这条连接的状态：本地地址、对端地址、是否已连接、协议选项（如 keep-alive、缓冲区大小）
     * 提供你操作这条连接的方法：connect()、read_some()、write_some()、async_read()、async_write()、close() 等
     *tcp::socket 没有无参默认构造，但是它有带 executor / io_context的有参构造函数
     *还可以移动构造：接收同类型右值，转移已有socket资源
     拷贝构造：直接删掉，禁止复制*/
    tcp::socket   _socket;
};