#include "NetworkCfg.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>

INIReader::INIReader(const std::string &filename) : filename_(filename)
{
    (void)load();
}

INIReader::Proxy &INIReader::Proxy::operator=(double v)
{
    data_[key_] = std::to_string(v);
    return *this;
}

bool INIReader::save() const
{
    std::ofstream file(filename_);
    if (!file.is_open())
        return false;

    std::string cur_section;
    for (const auto &kv : data_)
    {
        const size_t dot = kv.first.find('.');
        if (dot == std::string::npos)
            continue;

        const std::string section = kv.first.substr(0, dot);
        const std::string key = kv.first.substr(dot + 1);
        if (section.empty() || key.empty())
            continue;

        if (section != cur_section)
        {
            if (!cur_section.empty())
                file << "\n";
            file << "[" << section << "]\n";
            cur_section = section;
        }
        file << key << "=" << kv.second << "\n";
    }

    return true;
}

bool INIReader::load()
{
    std::ifstream file(filename_);
    if (!file.is_open())
        return false;

    data_.clear();
    std::string line;
    std::string current_section;

    while (std::getline(file, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        if (line.front() == '[' && line.back() == ']')
        {
            current_section = trim(line.substr(1, line.length() - 2));
            continue;
        }

        if (current_section.empty())
            continue;

        const size_t pos = line.find('=');
        if (pos == std::string::npos)
            continue;

        const std::string key = trim(line.substr(0, pos));
        if (key.empty())
            continue;
        data_[current_section + "." + key] = trim(line.substr(pos + 1));
    }

    return true;
}

std::string INIReader::trim(const std::string &str)
{
    const size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    const size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

template <> std::string INIReader::Proxy::cast<std::string>(std::string dv) const
{
    auto it = data_.find(key_);
    return it != data_.end() ? it->second : dv;
}

template <> int INIReader::Proxy::cast<int>(int dv) const
{
    auto it = data_.find(key_);
    if (it == data_.end())
        return dv;

    try
    {
        size_t idx = 0;
        const int value = std::stoi(it->second, &idx);
        return idx == it->second.size() ? value : dv;
    }
    catch (...)
    {
        return dv;
    }
}

template <> size_t INIReader::Proxy::cast<size_t>(size_t dv) const
{
    auto it = data_.find(key_);
    if (it == data_.end())
        return dv;

    if (!it->second.empty() && it->second[0] == '-')
        return dv;

    try
    {
        size_t idx = 0;
        const unsigned long long value = std::stoull(it->second, &idx);
        if (idx != it->second.size() || value > std::numeric_limits<size_t>::max())
            return dv;
        return static_cast<size_t>(value);
    }
    catch (...)
    {
        return dv;
    }
}

template <> uint32_t INIReader::Proxy::cast<uint32_t>(uint32_t dv) const
{
    auto it = data_.find(key_);
    if (it == data_.end())
        return dv;

    if (!it->second.empty() && it->second[0] == '-')
        return dv;

    try
    {
        size_t idx = 0;
        const unsigned long long value = std::stoull(it->second, &idx);
        if (idx != it->second.size() || value > std::numeric_limits<uint32_t>::max())
            return dv;
        return static_cast<uint32_t>(value);
    }
    catch (...)
    {
        return dv;
    }
}

template <> double INIReader::Proxy::cast<double>(double dv) const
{
    auto it = data_.find(key_);
    if (it == data_.end())
        return dv;

    try
    {
        size_t idx = 0;
        const double value = std::stod(it->second, &idx);
        return idx == it->second.size() ? value : dv;
    }
    catch (...)
    {
        return dv;
    }
}

template <> bool INIReader::Proxy::cast<bool>(bool dv) const
{
    auto it = data_.find(key_);
    if (it == data_.end())
        return dv;

    std::string value = it->second;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (value == "true" || value == "1" || value == "yes" || value == "on")
        return true;
    if (value == "false" || value == "0" || value == "no" || value == "off")
        return false;
    return dv;
}
