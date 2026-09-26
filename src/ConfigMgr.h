#pragma once
#include <map>
#include <string>

//定义一个SectionInfo类管理key和value
struct  SectionInfo
{
	SectionInfo() {}

	~SectionInfo() {
		_section_datas.clear();
	}

	//拷贝构造
	SectionInfo(const SectionInfo& src) {
		_section_datas = src._section_datas;
	}

	//拷贝赋值
	SectionInfo& operator = (const SectionInfo& src) {
		if (&src == this) {
			return *this;
		}
		this->_section_datas = src._section_datas;
		return *this;
	}

	//重载[]key对应value
	std::string  operator[](const std::string& key) {
		if (_section_datas.find(key) == _section_datas.end()) {
			return "";
		}
		// 这里可以添加一些边界检查  
		return _section_datas[key];
	}
	//ini文件中段里面的key和value
	std::map<std::string, std::string> _section_datas;
};


//定义ConfigMgr管理section(其包含key与value)
class ConfigMgr
{
public:
	~ConfigMgr() {
		_config_map.clear();
	}
	SectionInfo operator[](const std::string& section) {
		if (_config_map.find(section) == _config_map.end()) {
			return SectionInfo();
		}
		return _config_map[section];
	}

	ConfigMgr(const ConfigMgr& src) = delete;
	ConfigMgr& operator=(const ConfigMgr& src) = delete;

	static ConfigMgr& Inst() {
		static ConfigMgr cfg_mgr;
		return cfg_mgr;
	}

private:
	//构造函数里实现config读取
	ConfigMgr();
	//根据段名存放对应的段
	std::map<std::string, SectionInfo> _config_map;
};