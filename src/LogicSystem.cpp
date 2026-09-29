#include "LogicSystem.h"
#include "HttpConnection.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include "VarifyGrpcClient.h"
#include "RedisHolder.h"
#include "RedisConPool.h"
#include "RedisMgr.h"

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




    RegPost("/user_register", [](std::shared_ptr<HttpConnection> connection) {
        auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
        spdlog::info("receive body is {}", body_str); 

        //设置回复报文的格式
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

        nlohmann::json src_root;//放请求的请求体
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

        //同理校验客户端上传的 varifycode（验证码）字段，防止后面 get<std::string>() 抛异常
        if (!src_root.contains("varifycode") || !src_root["varifycode"].is_string()) {
            spdlog::warn("bad request body: {}", body_str);
            send_error(ErrorCodes::Error_Json, http::status::bad_request);
            return;
        }
        //客户端上传的验证码
        std::string varifycode = src_root["varifycode"].get<std::string>();

        //第一步：先查找redis中email对应的验证码是否合理
        //key 的格式必须和发验证码时写入 redis 的保持一致：varifycode_ + email
        std::string varify_key = "varifycode_" + email;
        std::string redis_value;
        bool bvarify = false;
        do {
            //客户端连验证码都没传，直接不通过
            if (varifycode.empty()) {
                break;
            }

            //key 不存在说明验证码已过期（SetEx 到时间自动删除）或没发过
            if (!RedisMgr::GetInstance()->ExistsKey(varify_key)) {
                spdlog::warn("varifycode expired or not exist, email: {}", email);
                break;
            }

            //取出 redis 里保存的验证码，和客户端上传的对比
            if (!RedisMgr::GetInstance()->Get(varify_key, redis_value)) {
                spdlog::error("redis get varifycode failed, email: {}", email);
                break;
            }

            //验证码不一致 → 不通过
            if (varifycode != redis_value) {
                spdlog::warn("varifycode not match, email: {}", email);
                spdlog::debug("varifycode not match, email: {}，用户输入varifycode:{}，redis_value:{}", email, varifycode, redis_value);
                break;
            }

            bvarify = true;
        } while (false);

        if (!bvarify) {
            //验证码错误/失效 → 通知客户端
            send_error(ErrorCodes::varify_code_not_match, http::status::ok);
            return;
        }

        //验证码用完即删，防止重复使用
        RedisMgr::GetInstance()->Del(varify_key);

        //第二步：查找数据库判断用户是否存在，不存在再创建（SQL 先空实现）
        //TODO: 接入 MySQL 后在这里实现：
        //  1.根据 email 查询用户是否已存在
        //  2.不存在则插入一条新用户
        auto check_user_exist = [](const std::string& /*email*/) -> bool {
            spdlog::info("TODO: check user exist in MySQL");
            return false;   //空实现：暂默认用户不存在
            };
        auto create_user = [](const std::string& /*email*/) -> bool {
            spdlog::info("TODO: create user in MySQL");
            return true;    //空实现：暂默认创建成功
            };

        if (check_user_exist(email)) {
            //用户已存在 → 通知客户端
            send_error(ErrorCodes::UserExist, http::status::ok);
            return;
        }
        if (!create_user(email)) {
            //插入失败 → 通知客户端
            send_error(ErrorCodes::SQLFailed, http::status::ok);
            return;
        }

        //============ 第三步：构造成功应答 ============
        //清空旧缓冲区！
        //res.body().consume(res.body().size());

        nlohmann::json root;
        root["error"] = ErrorCodes::Success;
        root["email"] = email;
        root["uid"] = 0;    //TODO: 空实现阶段还没有真实 uid，接入 MySQL 后带出
#ifdef _DEBUG
        beast::ostream(res.body()) << root.dump(4);
#else
        beast::ostream(res.body()) << root.dump();
#endif
        res.result(http::status::ok);
        res.prepare_payload();
        });

}