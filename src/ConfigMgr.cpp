#include "ConfigMgr.h"
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>       // ptree 的定义
#include <boost/property_tree/ini_parser.hpp>  // read_ini 在这里
#include <spdlog/spdlog.h>


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * 主要是用boost::property_tree::ptree读取ini配置然后再转换成自己定义的
   std::map<std::string, SectionInfo> _config_map;
   其中string表示ini中的段（section，分组），
   SectionInfo则表示段中的key=value
 * - - - -  - - - - - - - - - - - - - - -- - - - - - - - - - - -
 * boost::property_tree::ptree读取ini了也有自己的结构为什么不直接用还要转换一次？
   这不多次一举吗？
 * 以后可能解析别的文件呢？多一层封装降一层耦合！
 * * * * * * * * * * * * * * * ** * * * * * * * * * * * * * * * * * * * */

ConfigMgr::ConfigMgr() {
    // 获取当前工作目录  
    boost::filesystem::path current_path = boost::filesystem::current_path();
    // 构建config.ini文件的完整路径  
    boost::filesystem::path config_path = current_path / "config.ini";
    spdlog::debug("Config path: {}", config_path.string());

    /* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * 
    * 使用Boost.PropertyTree来读取INI文件 
    * ptree 是 Boost.PropertyTree 库的核心类，是一棵「属性树」，用来存储层级键值数据，专门用来读写配置文件Boost
    * ptree的结构大致如下
      class ptree{
       std::string m_value;                                  // ① 本节点自己的值
       std::list<std::pair<std::string, ptree>> m_children;  // ② 一串有序的子节点
      }
    * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
    boost::property_tree::ptree pt;
    try {
        boost::property_tree::read_ini(config_path.string(), pt);
    }
    catch (const boost::property_tree::ini_parser_error& e) {
        // 读配置失败是致命问题，必须用 error 级别，
        // 否则日志级别一调高（如 info）这条就被过滤掉，出问题无从排查
        spdlog::error("read config.ini failed: {}", e.what());
        return;
    }
   
    /* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
    * 遍历pt的子节点，子节点还是pt
    * 
    * 
    * 
    * * * * * * * * * * * * * * * ** * * * * * * * * * * * * * * * * * * * */
    
    // 遍历INI文件中的所有section  
    for (const auto& section_pair : pt) {
        const std::string& section_name = section_pair.first;
        const boost::property_tree::ptree& section_tree = section_pair.second;

        // 对于每个section，遍历其所有的key-value对  
        std::map<std::string, std::string> section_config;
        for (const auto& key_value_pair : section_tree) {
            const std::string& key = key_value_pair.first;
            const std::string& value = key_value_pair.second.get_value<std::string>();
            section_config[key] = value;
        }
        SectionInfo sectionInfo;
        sectionInfo._section_datas = section_config;
        // 将section的key-value对保存到config_map中  
        _config_map[section_name] = sectionInfo;
    }

    // 输出所有的section和key-value对  
    for (const auto& section_entry : _config_map) {
        const std::string& section_name = section_entry.first;
        const SectionInfo& section_config = section_entry.second;
        spdlog::debug("[{}]", section_name);
        for (const auto& key_value_pair : section_config._section_datas) {
            spdlog::debug("{}={}", key_value_pair.first, key_value_pair.second);
        }
    }

}