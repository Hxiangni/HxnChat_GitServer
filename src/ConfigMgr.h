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
	//成员变量
	std::map<std::string, std::string> _section_datas;
};


//定义ConfigMgr管理section和其包含的key与value

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


	ConfigMgr& operator=(const ConfigMgr& src) {
		if (&src == this) {
			return *this;
		}

		this->_config_map = src._config_map;
		return *this;
	};

	ConfigMgr(const ConfigMgr& src) {
		this->_config_map = src._config_map;
	}

	static ConfigMgr& Inst() {
		static ConfigMgr cfg_mgr;
		return cfg_mgr;
	}

private:
	//构造函数里实现config读取
	ConfigMgr();

	//从指定目录读取配置文件的构造函数
	//ConfigMgr(const std::string file_path);
private:
	std::map<std::string, SectionInfo> _config_map;
};