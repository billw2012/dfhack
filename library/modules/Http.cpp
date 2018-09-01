#include <vector>
#include <map>
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>

#include "modules/Http.h"
#include "LuaTools.h"

#include "json/json.h"
#include "tinythread.h"
#include "lua.h"
#include "lauxlib.h"
#include "happyhttp.h"

namespace DFHack { ;
namespace Http { ;

static const char* const s_module_name = "Http";

namespace { ;
    
enum LUA_VARIANT_TYPE : uint32_t
{
    LUA_VARIANT_DOUBLE,
    LUA_VARIANT_INT,
    LUA_VARIANT_POINTER,
    LUA_VARIANT_STRING,
    LUA_VARIANT_BOOL,
    LUA_VARIANT_FUNCTION,
    LUA_VARIANT_FUNCTION_REF,
    LUA_VARIANT_ITABLE,
    LUA_VARIANT_KTABLE,
    LUA_VARIANT_NIL
};

#define CONST_TO_STRING_SWITCH(c) case c: { return #c; }

static Json::Value FromFunctionRef(LUA_INTEGER ref)
{
    return Json::Value{"<function ptr>"};
}

static Json::Value parse(lua_State *L, int32_t index, bool parseNumbersAsDouble = false, bool pop = true)
{
    Json::Value v;

    auto type = lua_type(L, index);
    if (type == LUA_TUSERDATA || type == LUA_TLIGHTUSERDATA)
    {
        v = Json::Value((void*)lua_touserdata(L, index));
    }
    else if (type == LUA_TNUMBER)
    {
        if (parseNumbersAsDouble)
        {
            v = Json::Value(lua_tonumber(L, index));
        }
        else
        {
            // Use standard int for now.
            v = Json::Value(static_cast<Json::Value::Int>(lua_tointeger(L, index)));
        }
    }
    else if (type == LUA_TSTRING)
    {
        v = Json::Value(lua_tostring(L, index));
    }
    else if (type == LUA_TBOOLEAN)
    {
        v = Json::Value(lua_toboolean(L, index) == 1);
    }
    else if (type == LUA_TFUNCTION)
    {
        // copy the function since we're about to pop it off
        lua_pushvalue(L, index);
        v = FromFunctionRef(luaL_ref(L, LUA_REGISTRYINDEX));
    }
    else if (type == LUA_TTABLE)
    {
        // if the indexds is in the negative, parsing a table
        // will put values "above" it, moving it down by 1 negative index
        if (index < 0) index--;

        static const int32_t KEY_INDEX = -2;
        static const int32_t VALUE_INDEX = -1;
        typedef std::function<void(lua_State*)> WalkCallback;

        // The table parsing loop is conducted by lua_next(), which:
        //    - reads a key from the stack
        //    - read the value of the next key from the table (table index specified by second arg)
        //    - pushes the key (KEY_INDEX) and value (VALUE_INDEX) to the stack
        //    - returns 0 and pops all extraneous stuff on completion
        // Starting with a key of nil will allow us to process the entire table.
        // Leaving the last key on the stack each iteration will go to the next item
        auto walk = [](lua_State *L, int32_t index, WalkCallback callback) -> void
        {
            lua_pushnil(L);
            while (lua_next(L, index))
            {
                callback(L);
                lua_pop(L, 1);
            }
        };

        // we're going to walk the table one time to see if we can use an std::vector<Json::Value>,
        // or if it we need a std::map<Json::Value, Json::Value>.
        bool requiresStringKeys = false;
        walk(L, index, [&requiresStringKeys](lua_State *L) -> void
        {
            if (lua_type(L, KEY_INDEX) == LUA_TSTRING)
                requiresStringKeys = true;
        });

        // the below are variations, since the table might have only numeric indices (iTable)
        // or it might also have string indices (kTable)
        auto parseKTableItem =
            [parseNumbersAsDouble, &v](lua_State *L) -> void
        {
            // TODO: probably type-check the key here and have some error handling mechanism
            std::string key = lua_tostring(L, KEY_INDEX);
            int32_t valueIndex = (lua_type(L, VALUE_INDEX) == LUA_TTABLE) ? lua_gettop(L) : VALUE_INDEX;
            v[key] = parse(L, valueIndex, parseNumbersAsDouble, false);
        };
        auto parseITableItem =
            [parseNumbersAsDouble, &v](lua_State *L) -> void
        {
            // TODO: probably type-check the key here and have some error handling mechanism
            auto key = lua_tonumber(L, KEY_INDEX) - 1; // -1 because lua is 1-indexed and we are 0-indexed
            int32_t valueIndex = (lua_type(L, VALUE_INDEX) == LUA_TTABLE) ? lua_gettop(L) : VALUE_INDEX;

            v[static_cast<int>(key)] = parse(L, valueIndex, parseNumbersAsDouble, false);
        };

        if (requiresStringKeys)
        {
            v = Json::Value(Json::objectValue);
            walk(L, index, parseKTableItem);
        }
        else
        {
            v = Json::Value(Json::arrayValue);
            walk(L, index, parseITableItem);
        }
    }

    return v;
}

}


/* Original code: http://stackoverflow.com/questions/2616011/easy-way-to-parse-a-url-in-c-cross-platform */
bool parse_url(const std::string& url_s, std::string& protocol_, std::string& host_, std::string& path_, std::string& query_)
{
    using namespace std;
    const string prot_end("://");

    string::const_iterator prot_i = search(url_s.begin(), url_s.end(), prot_end.begin(), prot_end.end());
    protocol_.reserve(distance(url_s.begin(), prot_i));

    transform(url_s.begin(), prot_i, back_inserter(protocol_), ptr_fun<int, int>(tolower)); // protocol is icase

    if (prot_i == url_s.end())
        return false;

    advance(prot_i, prot_end.length());
    string::const_iterator path_i = find(prot_i, url_s.end(), '/');
    host_.reserve(distance(prot_i, path_i));
    transform(prot_i, path_i, back_inserter(host_), ptr_fun<int, int>(tolower)); // host is icase
    string::const_iterator query_i = find(path_i, url_s.end(), '?');
    path_.assign(path_i, query_i);
    if (query_i != url_s.end())
        ++query_i;

    query_.assign(query_i, url_s.end());
    return true;
}

struct SendThread
{
    struct ConnectionWrapper
    {
        ConnectionWrapper(const char* host, int port, happyhttp::ResponseBegin_CB begincb, happyhttp::ResponseData_CB datacb, happyhttp::ResponseComplete_CB completecb, void* userdata)
            : conn(host, port) 
        {
            conn.setcallbacks(begincb, datacb, completecb, userdata);
        }

        std::time_t last_used;
        happyhttp::Connection conn;
        tthread::mutex mutex;
    };

    tthread::mutex connections_mutex;
    std::unordered_map<std::string, std::shared_ptr<ConnectionWrapper>> connections;
    std::unique_ptr<tthread::thread> conn_thread;

    struct Post
    {
        std::string url;
        Json::Value value;
    };

    tthread::mutex post_queue_mutex;
    tthread::condition_variable post_queue_items_present;
    std::vector<Post> post_queue;
    std::unique_ptr<tthread::thread> post_thread;

    bool terminate_threads = false;
    bool debug = false;

    // Make it a singleton for now.
    static SendThread& get()
    {
        static std::shared_ptr<SendThread> instance = std::make_shared<SendThread>();
        return *instance;
    }
    
    SendThread()
    {
        happyhttp::init();

        conn_thread.reset(new tthread::thread(&conn_thread_fn, this));
        post_thread.reset(new tthread::thread(&post_thread_fn, this));
    }

    ~SendThread()
    {
        // Use the validity of the conn_thread as a flag to indicate we were successfully initialized and should shut down
        // cleanly
        if (conn_thread)
        {
            terminate_threads = true;
            post_queue_items_present.notify_all();
            conn_thread.reset();
            post_thread.reset();

            happyhttp::shutdown();
        }
    }

    void send(const std::string& url, const Json::Value& value)
    {
        tthread::lock_guard<tthread::mutex> guard(post_queue_mutex);
        post_queue.push_back({ url, value });
        post_queue_items_present.notify_all();
    }

private:
    static void conn_thread_fn(void* this_raw)
    {
        auto this_ptr = static_cast<SendThread*>(this_raw);
        std::vector<std::shared_ptr<ConnectionWrapper>> connections_to_pump;

        while (!this_ptr->terminate_threads)
        {
            {
                auto t = std::time(nullptr);
                tthread::lock_guard<tthread::mutex> guard(this_ptr->connections_mutex);
                connections_to_pump.clear();
                for (auto& conn : this_ptr->connections)
                {
                    connections_to_pump.push_back(conn.second);
                    // TODO: cull old/dead connections
                    //if (std::difftime(std::time(nullptr), conn.second->last_used) > 30)
                    //{

                    //}
                }
            }

            for (auto& conn : connections_to_pump)
            {
                tthread::lock_guard<tthread::mutex> guard(conn->mutex);
                conn->conn.pump();
            }

            // Is this a reasonable interval?
            tthread::this_thread::sleep_for(tthread::chrono::milliseconds(100));
        }
    }

    static void ResponseBegin_CB(const happyhttp::Response* /*r*/, void* /*userdata*/)
    {
        // Unused
        //auto this_ptr = static_cast<SendThread*>(userdata);
    }

    static void ResponseData_CB(const happyhttp::Response* /*r*/, void* /*userdata*/, const unsigned char* /*data*/, int /*numbytes*/)
    {
        // Unused
        //auto this_ptr = static_cast<SendThread*>(userdata);
    }

    static void ResponseComplete_CB(const happyhttp::Response* /*r*/, void* /*userdata*/)
    {
        // Unused
        //auto this_ptr = static_cast<SendThread*>(userdata);
    }

    static void post_thread_fn(void* this_raw)
    {
        auto this_ptr = static_cast<SendThread*>(this_raw);

        std::vector<Post> curr_post_queue;
        while (!this_ptr->terminate_threads)
        {
            {
                tthread::lock_guard<tthread::mutex> guard(this_ptr->post_queue_mutex);
                if (this_ptr->post_queue.empty())
                {
                    this_ptr->post_queue_items_present.wait(this_ptr->post_queue_mutex);
                }
                if (this_ptr->debug)
                {
                    Core::print("%s %d items to send on post thread\n", s_module_name, (int)this_ptr->post_queue.size());
                }
                curr_post_queue.swap(this_ptr->post_queue);
            }
            for (auto&& post : curr_post_queue)
            {
                this_ptr->post_value(post.url, post.value);
            }
            curr_post_queue.clear();
        }
    }

    void post_value(std::string url, const Json::Value& value)
    {
        static const char* headers[] = {
            "Accept: application/json",
            "Content-Type: application/json",
            "charsets: utf-8"
        };

        std::transform(url.begin(), url.end(), url.begin(), ::tolower);

        std::string protocol, host_port, path, query;
        parse_url(url, protocol, host_port, path, query);
        std::string connection_name = protocol + host_port;

        std::string host = host_port;
        // default to http default port
        int port = 80;
        auto port_idx = host_port.find(':');
        if (port_idx != std::string::npos)
        {
            host = host_port.substr(0, port_idx);
            port = std::atoi(host_port.substr(port_idx + 1).c_str());
        }

        std::shared_ptr<ConnectionWrapper> conn;
        // Find an existing connection or create one
        {
            tthread::lock_guard<tthread::mutex> guard(connections_mutex);
            auto existing_conn = connections.find(connection_name);
            if (existing_conn == connections.end())
            {
                conn = std::make_shared<ConnectionWrapper>(host.c_str(), port, ResponseBegin_CB, ResponseData_CB, ResponseComplete_CB, this);
                connections[connection_name] = conn;
            }
            else
            {
                conn = existing_conn->second;
            }
        }

        std::string str = value.toStyledString();

        if (debug)
        {
            Core::print("%s Sending to %s...\n%s\n", s_module_name, url.c_str(), str.c_str());
        }

        {
            tthread::lock_guard<tthread::mutex> guard(conn->mutex);
            try
            {
                conn->conn.request("POST", url.c_str(), headers, reinterpret_cast<const unsigned char*>(str.c_str()), str.length());
            }
            catch (const happyhttp::happyhttp_exception& ex)
            {
                Core::print("%s Error sending to %s: %s\n", s_module_name, url.c_str(), ex.what());
            }
        }

        if (debug)
        {
            Core::print("%s ...done\n", s_module_name);
        }

        //CURLcode res;
        //std::string str = value.toStyledString();
        //curl_easy_setopt(curl.get(), CURLOPT_VERBOSE, 1);
        //curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        //curl_easy_setopt(curl.get(), CURLOPT_POST, 1);
        //struct curl_slist *headers = NULL;
        //headers = curl_slist_append(headers, "Accept: application/json");
        //headers = curl_slist_append(headers, "Content-Type: application/json");
        //headers = curl_slist_append(headers, "charsets: utf-8");
        //curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
        //curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, str.c_str());
        //curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, str.length());
        //res = curl_easy_perform(curl.get());
        //if (res != CURLE_OK)
        //{
        //    Core::print("%s Error sending to %s: %s\n", s_module_name, url.c_str(), curl_easy_strerror(res));
        //}
        //if (debug)
        //{
        //    Core::print("%s ...done\n", s_module_name);
        //}
    }
};

int post_as_json(lua_State* state)
{
    if (lua_gettop(state) != 2)
    {
        return luaL_error(state, "Invalid number of parameters. Usage: post_as_json(url, object)");
    }
    auto url = lua_tostring(state, 1);
    auto val = parse(state, 2);
    lua_pop(state, 2);
    try
    {
        SendThread::get().send(url, val);
    }
    catch (const happyhttp::happyhttp_exception& ex)
    {
        return luaL_error(state, ex.what());
    }

    return 0;
}

int to_json_string(lua_State* state)
{
    if (lua_gettop(state) != 1)
    {
        return luaL_error(state, "Invalid number of parameters. Usage: to_json_string(object)");
    }
    auto val = parse(state, 1);
    lua_pop(state, 1);
    lua_pushstring(state, val.toStyledString().c_str());
    return 1;
}

std::string get_timestamp()
{
    time_t now;
    time(&now);
    char buf[sizeof "2011-10-08T07:07:09Z"];
    strftime(buf, sizeof buf, "%FT%TZ", gmtime(&now));
    return buf;
}

int get_iso8601_timestamp(lua_State* state)
{
    if (lua_gettop(state) != 0)
    {
        return luaL_error(state, "Function does not expect parameters. Usage: get_iso8601_timestamp()");
    }
    lua_pushstring(state, get_timestamp().c_str());
    return 1;
}

}

}
