#pragma once
#include "Singleton.h"
#include <functional>
#include <map>
#include "const.h"

class HttpConnection;//前置声明，代表一条HTTP客户端连接对象

/*std::function<T> 是 C++ 标准库的函数包装器
* void(std::shared_ptr<HttpConnection>)是函数类型描述符,返回值类型(参数类型列表)
  只能包装，接收 1 个 std::shared_ptr<HttpConnection> 类型参数，无返回值的，可调用对象。
  std::function<void(std::shared_ptr<HttpConnection>)> 是 STL 提供的完整实例化类，
  底层逻辑全部封装好，你不能改它内部实现，只能调用它对外暴露的公有接口；
  这个类有硬性约束：只能存储签名匹配的可调用对象
*typedef 原类型 别名; */
typedef std::function<void(std::shared_ptr<HttpConnection>)> HttpHandler;

class LogicSystem :public Singleton<LogicSystem>
{
    /** ** ** *
     * 为什么要设置成友元？
       因为Singleton的构造函数是private，Singleton<LogicSystem> 的构造函数是 protected：
       GetInstance() 属于 Singleton<LogicSystem> 这个类 → 它对 Singleton<LogicSystem>
       自己的 private/protected 有完全访问权 ✅
       但它要执行 new T = new LogicSystem() → 这是在访问 LogicSystem 的 private 构造函数 ❌
     *一个类的成员函数，无权访问另一个类的 private。 哪怕它是"爹"（基类）也一样
     ********/
    friend class Singleton<LogicSystem>;
public:
    ~LogicSystem();
    bool HandleGet(std::string, std::shared_ptr<HttpConnection>);
    void RegGet(std::string, HttpHandler handler);
    void RgePost(std::string, HttpHandler handler);
private:
    LogicSystem();
    //HttpHandler是一个指定类型的可调用对象
    std::map<std::string, HttpHandler> _post_handlers;
    std::map<std::string, HttpHandler> _get_handlers;
};
