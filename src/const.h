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