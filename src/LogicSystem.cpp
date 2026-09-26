#include "LogicSystem.h"
#include "HttpConnection.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include "VarifyGrpcClient.h"
//前置声明解决互引用问题

//注册传来的路由请求
//RegGet 接收路径和一个可调用对象
//再通过 make_pair 把「路径字符串」和「包装后的函数对象」打包，插入红黑树 map 完成路由绑定。
void LogicSystem::RegGet(std::string url, HttpHandler handler)
{
    //std::make_pair(a, b) 是标准库工具函数，自动根据传入的两个变量类型，生成一个 std::pair<T1, T2> 键值对对象。
    //然后把这个键值对象和handler打包进方法容器
    _get_handlers.insert(make_pair(url, handler));
}

void LogicSystem::RegPost(std::string url, HttpHandler handler)
{
    _post_handlers.insert(make_pair(url, handler));
}

bool LogicSystem::HandlePost(std::string path, std::shared_ptr<HttpConnection> con)
{
    if(_post_handlers.find(path)==_post_handlers.end())
        return false;
    _post_handlers[path](con);
    return true;
}


LogicSystem::~LogicSystem()
{
    ;
}

//路由分发器：根据 path 找到对应的业务函数
bool LogicSystem::HandleGet(std::string path, std::shared_ptr<HttpConnection> con) {
    //在路由表里查找请求路径,找不到就返回.end()
    if (_get_handlers.find(path) == _get_handlers.end()) {
        return false;//如果没找到
    }

    //"查找并立刻执行"key值对应可调用对象
    _get_handlers[path](con);
    return true;
}

//构造函数
LogicSystem::LogicSystem() {

    //在服务器中注册一条 GET 接口路由：地址 /get_test，并绑定处理这个请求的 lambda 业务函数。
    RegGet("/get_test", [](std::shared_ptr<HttpConnection> connection) {

        //beast::ostream() 是 Boost.Beast 库的工具，接收一块文本缓冲区，创建一个临时输出流对象。
        beast::ostream(connection->_response.body()) << "receive get_test req " << std::endl;//把收到请求的消息放入恢复报文中
        int i = 0;
        for (auto& elem : connection->_get_params) {//把 URL 上所有 GET 参数，按顺序编号，一行一行拼到返回给浏览器的页面文字里。
            i++;
            beast::ostream(connection->_response.body()) << "param" << i << " key is " << elem.first;
            beast::ostream(connection->_response.body()) << ", " << " value is " << elem.second << std::endl;
        }
        });

    /*注册一个 POST 接口 `/get_varifycode`，接收 JSON 请求体，解析邮箱，返回 JSON 应答
     *格式请求头GET 请求体*/
    RegPost("/get_varifycode",  [](std::shared_ptr<HttpConnection> connection) {
        auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
        spdlog::info("receive body is {}", body_str);

        auto& res = connection->_response;
        res.set(http::field::content_type, "application/json");

        // 统一错误应答
        auto send_error = [&res](int code, http::status st) {
            //清空旧缓冲区！
            //res.body().consume(res.body().size());

            nlohmann::json root{ {"error", code} };
#ifdef _DEBUG
            beast::ostream(res.body()) << root.dump(4);
#else
            beast::ostream(res.body()) << root.dump();
#endif
            res.result(st);
            res.prepare_payload();
            };

        nlohmann::json src_root;//放的是post请求的请求体
        try
        {
            src_root = nlohmann::json::parse(body_str);
            //判断 json 对象里有没有叫 email 的 key
            //.is_string() 判断这个值是不是字符串类型
            if (!src_root.contains("email") || !src_root["email"].is_string()) {
                spdlog::warn("bad request body: {}", body_str);
                send_error(ErrorCodes::Error_Json, http::status::bad_request);
                return;
            }


        }
        catch (const nlohmann::json::parse_error& e)
        {
            spdlog::error("json error: {}", e.what());
            send_error(ErrorCodes::Error_Json, http::status::bad_request);
            return;
        }
        //把这个 json 节点解析输出成 C++ 的std::string类型
        std::string email = src_root["email"].get<std::string>();

        GetVarifyRsp rsp = VerifyGrpcClient::GetInstance()->GetVarifyCode(email);

        // ✅ 成功分支同样先清空body
        res.body().consume(res.body().size());


        //构造回复报文的json
        nlohmann::json root;
        //向 json 对象增加键值对
        root["error"] = ErrorCodes::Success;
        root["email"] = email;
#ifdef _DEBUG
        beast::ostream(res.body()) << root.dump(4);
#else
        beast::ostream(res.body()) << root.dump();
#endif
        res.result(http::status::ok);
        res.prepare_payload();
        });

}