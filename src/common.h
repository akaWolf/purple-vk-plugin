// Common header, should be included in every file, contains the most basic
// data structures and algorithms.

#pragma once

// It gets set by CMake and is so much trouble to unset, so let's unset it here :-)
// On Linux it is used only for always enabling assert().
#undef NDEBUG

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

// Let's make using most popular names easier.
using std::map;
using std::pair;
using std::set;
using std::shared_ptr;
using std::vector;

// NOTE: Should change string alias to __gnu_cxx::vstring;
using string = std::string;
using string_map = map<string, string>;
using string_pair = pair<string, string>;
using string_set = set<string>;
using string_vec = vector<string>;

using int_vec = vector<int>;
using uint_set = set<uint>;
using int64 = int64_t;
using uint64 = uint64_t;
using uint64_vec = vector<uint64>;
using uint64_set = set<uint64>;

using std::to_string;

using steady_clock = std::chrono::steady_clock;
using steady_time_point = std::chrono::steady_clock::time_point;
using steady_duration = std::chrono::steady_clock::duration;

// This function type is used for signalling success if no other information must be passed.
using SuccessCb = std::function<void()>;
// This function type is used for returning errors via callback.
using ErrorCb = std::function<void()>;

// A very simple object, which calls given function upon termination.
class OnExit
{
public:
    OnExit(std::function<void()> deleter)
        : m_deleter(move(deleter))
    {
    }

    ~OnExit()
    {
        m_deleter();
    }

private:
    std::function<void(void)> m_deleter;
};

// Miscellaneous string functions

// Creates a new string, analogous to sprintf
inline string str_format(const char* fmt, ...)
{
    char tmp[3072];

    va_list arg;
    va_start(arg, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, arg);
    va_end(arg);

    return string(tmp);
}

// Replaces all occurences of from to to in src string. Extremely inefficient, but who cares.
// Probably should've used replace_all or regex from boost.
inline void str_replace(string& s, const char* from, const char* to)
{
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from_len, to);
        pos += to_len;
    }
}

inline void str_replace(string& s, const string& from, const string& to)
{
    str_replace(s, from.data(), to.data());
}

// Concatenates strings into one string, separating them with given separator like "smth".join() in Python.
template<typename Sep, typename It>
inline string str_concat(Sep sep, It first, It last)
{
    string s;
    for (It it = first; it != last; it++) {
        if (!s.empty())
            s += sep;
        s += *it;
    }
    return s;
}

// Version of str_concat, which accepts container.
template<typename Sep, typename C>
inline string str_concat(Sep sep, const C& c)
{
    return str_concat(sep, c.cbegin(), c.cend());
}

// Creates string of integers, separated by sep.
template<typename Sep, typename It>
string str_concat_int(Sep sep, It first, It last)
{
    string s;
    for (It it = first; it != last; it++) {
        if (!s.empty())
            s += sep;
        char buf[128];
        sprintf(buf, "%lld", (long long)*it);
        s += buf;
    }
    return s;
}

// Version of str_concat_int, which accepts container.
template<typename Sep, typename C>
string str_concat_int(Sep sep, const C& c)
{
    return str_concat_int(sep, c.cbegin(), c.cend());
}

// Converts string to upper-case
inline void to_upper(string& s)
{
    std::transform(s.begin(), s.end(), s.begin(), toupper);
}

// Returns the portion of the string after the rightmost sep.
inline string str_rsplit(const string& s, char sep)
{
    string::size_type last = s.find_last_of(sep);
    if (last != string::npos)
        return s.substr(last + 1);
    else
        return s;
}

inline string str_rsplit(const char* s, char sep)
{
    const char* last = strrchr(s, sep);
    if (last)
        return string(last + 1);
    else
        return string(s);
}

// Miscellaneous container functions

// Checks if map already contains key and sets it to new value.
template<typename M, typename K, typename V>
inline bool map_update(M& map, const K& key, const V& value)
{
    typename M::iterator it = map.find(key);
    if (it == map.end())
        return false;
    it->second = value;
    return true;
}

// Returns value for key or default value without inserting into map.
template<typename M, typename K, typename V = typename M::value_type>
inline V map_at_default(const M& map, const K& key, const V& default_value = V())
{
    typename M::iterator it = map.find(key);
    if (it == map.end())
        return default_value;
    else
        return it->second;
}

// Returns true if map or set contains key.
template<typename C, typename K>
inline bool contains_key(const C& cont, const K& key)
{
    return cont.find(key) != cont.end();
}

// Appends one container to another.
template<typename D, typename S>
inline void append(D& dst, const S& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

// Time functions

// Converts the given duration to milliseconds.
template<typename T>
inline std::chrono::milliseconds::rep to_milliseconds(T duration)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}
