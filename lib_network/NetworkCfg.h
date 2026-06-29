#ifndef NETWORK_CFG_H
#define NETWORK_CFG_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

class INIReader
{
  public:
    struct Proxy
    {
        std::map<std::string, std::string> &data_;
        const std::string key_;

        Proxy &operator=(const std::string &v)
        {
            data_[key_] = v;
            return *this;
        }
        Proxy &operator=(const char *v)
        {
            data_[key_] = v ? v : "";
            return *this;
        }
        Proxy &operator=(int v)
        {
            data_[key_] = std::to_string(v);
            return *this;
        }
        Proxy &operator=(size_t v)
        {
            data_[key_] = std::to_string(v);
            return *this;
        }
        Proxy &operator=(uint32_t v)
        {
            data_[key_] = std::to_string(v);
            return *this;
        }
        Proxy &operator=(double v);
        Proxy &operator=(bool v)
        {
            data_[key_] = v ? "true" : "false";
            return *this;
        }

        template <typename T> T cast(T default_val = T{}) const;
    };

    explicit INIReader(const std::string &filename);
    ~INIReader() = default;

    Proxy operator[](const std::string &key)
    {
        return {data_, key};
    }

    bool ensure_default(const std::string &key, const std::string &value);
    bool save() const;

  private:
    bool load();
    static std::string trim(const std::string &str);

    std::string filename_;
    std::map<std::string, std::string> data_;
};

template <> std::string INIReader::Proxy::cast<std::string>(std::string dv) const;
template <> int INIReader::Proxy::cast<int>(int dv) const;
template <> size_t INIReader::Proxy::cast<size_t>(size_t dv) const;
template <> uint32_t INIReader::Proxy::cast<uint32_t>(uint32_t dv) const;
template <> double INIReader::Proxy::cast<double>(double dv) const;
template <> bool INIReader::Proxy::cast<bool>(bool dv) const;

#endif // NETWORK_CFG_H
