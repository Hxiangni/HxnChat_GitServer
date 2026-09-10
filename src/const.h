//创建全局公用的头文件以防多重引用
#pragma once
#include <boost/beast/http.hpp>
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <iostream>
#include <nlohmann/json.hpp>

enum ErrorCodes {
	Success=0,
	Error_Json=1001,//Json解析错误
	RPCFailed = 1002,  //RPC请求错误
};

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

class ConfigMgr;//前置声明
extern ConfigMgr gCfgMgr;//extern 告诉编译器，这个变量存在，但是它的定义在别的翻译单元（.cpp）里。
//等所有.cpp都编译成.（目标文件）之后，链接器才会在所有.o的符号表里，查找符号gCfgMgr的定义实体。