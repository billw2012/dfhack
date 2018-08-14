#include <ctime>
#include <iostream>
#include <vector>

#include "Core.h"
#include <Console.h>
#include <Export.h>
#include <PluginManager.h>

#include "modules/Units.h"
#include "modules/World.h"
#include "modules/Maps.h"
#include "modules/MapCache.h"
#include "modules/Items.h"
#include "modules/Translation.h"


// DF data structure definition headers
#include "DataDefs.h"

#include <df/world.h>
#include <df/unit.h>
#include <df/unit_soul.h>

#include "LuaTools.h"
#define CURL_STATICLIB
#include "curl/curl.h"
#include "json/json.h"
#include "tinythread.h"

#include "fortmetrics.h"

using namespace DFHack;
using namespace df::enums;

DFHACK_PLUGIN("fortmetrics");
static const char* const fortmetrics_log_id = "[fortmetrics]";

// Any globals a plugin requires (e.g. world) should be listed here.
// For example, this line expands to "using df::global::world" and prevents the
// plugin from being loaded if df::global::world is null (i.e. missing from symbols.xml):
//
///REQUIRE_GLOBAL(world);
using df::global::world;

command_result fortmetrics (color_ostream &out, std::vector <std::string> & parameters);

static std::shared_ptr<CURL> curl;
static std::unique_ptr<tthread::thread> post_thread;

struct Post
{
    Json::Value value;
    std::string url;
};

tthread::mutex post_queue_mutex;
tthread::condition_variable post_queue_items_present;
std::vector<Post> post_queue;
bool terminate_post_thread = false;
bool debug = false;
void post_thread_fn(void*);

// Mandatory init function. If you have some global state, create it here.
DFhackCExport command_result plugin_init (color_ostream &out, std::vector <PluginCommand> &commands)
{
    commands.push_back(PluginCommand(
        "fortmetrics", "Stream various fort related metrics to Elasticsearch.",
        fortmetrics, false, /* true means that the command can't be used from non-interactive user interface */
        "  Stream various fort related metrics to Elasticsearch.\n"
        "  fortmetrics stress <url>\n"
        "    Send stress metrics now to server at url.\n"
        "  fortmetrics focus <url>\n"
        "    Send stress focus now to server at url.\n"
    ));

    out << "Initializing fortmetrics...";
    //auto res = curl_global_init(CURL_GLOBAL_ALL);
//     if (res != CURLE_OK)
//     {
//         out << "Error initializing libcurl: " << curl_easy_strerror(res) << endl;
//         return CR_FAILURE;
//     }
//     else
//     {
//         out << "...done" << endl;
//     }
    auto curl_ptr = curl_easy_init();
    if (curl_ptr == nullptr)
    {
        out << "failed\nError initializing libcurl\n";
        return CR_FAILURE;
    }
    out << "done\n";
    curl = std::shared_ptr<CURL>(curl_ptr, [](CURL* ptr) { curl_easy_cleanup(ptr); });
    post_thread = std::make_unique<tthread::thread>(&post_thread_fn, nullptr);
    return CR_OK;
}


// This is called right before the plugin library is removed from memory.
DFhackCExport command_result plugin_shutdown (color_ostream &out)
{
    terminate_post_thread = true;
    post_queue_items_present.notify_all();
    post_thread.reset();

    curl.reset();

    // You *MUST* kill all threads you created before this returns.
    // If everything fails, just return CR_FAILURE. Your plugin will be
    // in a zombie state, but things won't crash.
    return CR_OK;
}


// Called to notify the plugin about important state changes.
// Invoked with DF suspended, and always before the matching plugin_onupdate.
// More event codes may be added in the future.
/*
DFhackCExport command_result plugin_onstatechange(color_ostream &out, state_change_event event)
{
    switch (event) {
    case SC_GAME_LOADED:
        // initialize from the world just loaded
        break;
    case SC_GAME_UNLOADED:
        // cleanup
        break;
    default:
        break;
    }
    return CR_OK;
}
*/

// Whatever you put here will be done in each game step. Don't abuse it.
// It's optional, so you can just comment it out like this if you don't need it.
/*
DFhackCExport command_result plugin_onupdate ( color_ostream &out )
{
    // whetever. You don't need to suspend DF execution here.
    return CR_OK;
}
*/

std::string get_timestamp()
{
    time_t now;
    time(&now);
    char buf[sizeof "2011-10-08T07:07:09Z"];
    strftime(buf, sizeof buf, "%FT%TZ", gmtime(&now));
    return buf;
}

//std::string get_ticks_timestamp()
//{
//    static const int months_per_year = 12;
//    static const int days_per_month = 28;
//    static const int days_per_year = months_per_year * days_per_month;
//    static const int ticks_per_year = 403200;
//    static const int ticks_per_month = ticks_per_year / months_per_year;
//    static const int ticks_per_day = ticks_per_year / days_per_year;
//    std::stringstream s;
//    if (df::global::cur_year && df::global::cur_year_tick)
//    {
//        char str[256];
//        // 2018-08-11T19:12:24Z
//        int month = *df::global::cur_year_tick / ticks_per_month;
//        int day = *df::global::cur_year_tick / ticks_per_day % days_per_month;
//        snprintf(str, sizeof(str), "%04d-%02d-%02dT%02d:%02d:%02dZ", *df::global::cur_year, month, day);
//        //s << *df::global::cur_year << "-" << month << ;
//    }
//}

void post_value(const Json::Value& value, const std::string& url)
{
    CURLcode res;
    std::string str = value.toStyledString();
    //char *postFields = ;
    curl_easy_setopt(curl.get(), CURLOPT_VERBOSE, 1);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "charsets: utf-8");
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, str.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, str.length());
    if (debug)
    {
        Core::print("%s Sending to %s...\n%s\n", fortmetrics_log_id, url.c_str(), str.c_str());
    }
    res = curl_easy_perform(curl.get());
    if (res != CURLE_OK)
    {
        Core::print("%s Error sending to %s: %s\n", fortmetrics_log_id, url.c_str(), curl_easy_strerror(res));
    }
    if (debug)
    {
        Core::print("%s ...done\n", fortmetrics_log_id);
    }
}

void post_thread_fn(void*)
{
    std::vector<Post> curr_post_queue;
    while (!terminate_post_thread)
    {
        {
            tthread::lock_guard<tthread::mutex> guard(post_queue_mutex);
            if (post_queue.empty())
            {
                post_queue_items_present.wait(post_queue_mutex);
            }
            if (debug) 
            {
                Core::print("%s %d items to send on post thread\n", fortmetrics_log_id, post_queue.size());
            }
            curr_post_queue.swap(post_queue);
        }
        for (auto&& post : curr_post_queue)
        {
            post_value(post.value, post.url);
        }
        curr_post_queue.clear();
    }
}

void log_metric(const Json::Value& metric, const std::string& url/*, color_ostream& out, bool debug*/)
{
    tthread::lock_guard<tthread::mutex> guard(post_queue_mutex);
    post_queue.push_back({ metric, url });
    post_queue_items_present.notify_all();
}

template <class MetricFn_>
void log_dwarf_metrics(MetricFn_&& metric_callback, const std::string& url)
{
    static const int months_per_year = 12;
    static const int days_per_month = 28;
    static const int days_per_year = months_per_year * days_per_month;
    static const int ticks_per_year = 403200;
    static const int ticks_per_month = ticks_per_year / months_per_year;
    static const int ticks_per_day = ticks_per_year / days_per_year;

    for (auto&& unit : world->units.active)
    {
        if (Units::isCitizen(unit))
        {
            Json::Value log_entry(Json::objectValue);
            log_entry["dwarf"] = DF2UTF(DFHack::Translation::TranslateName(&(unit->status.current_soul->name), false));

            // real time stamp
            log_entry["timestamp"] = get_timestamp();

            // df time
            log_entry["year"] = *df::global::cur_year;
            log_entry["season"] = *df::global::cur_season;
            log_entry["month"] = *df::global::cur_year_tick / ticks_per_month;
            log_entry["day"] = *df::global::cur_year_tick / ticks_per_day % days_per_month;
            log_entry["year_tick"] = *df::global::cur_year_tick;
            log_entry["total_tick"] = *df::global::cur_year * ticks_per_year + *df::global::cur_year_tick;

            metric_callback(unit, log_entry);


            log_metric(log_entry, url);
            //val["stress"] = unit->status.current_soul->personality.stress_level;
            //val["timestamp"] = getISOCurrentTimestamp<std::chrono::milliseconds>();
        }
    }
}

std::map<int32_t, int32_t> last_stress;

void log_metrics_stress(const std::string& url)
{
    log_dwarf_metrics([](df::unit* unit, Json::Value& log_entry) {
        auto stress = unit->status.current_soul->personality.stress_level;
        auto existing_stress = last_stress.find(unit->id);
        if (existing_stress != last_stress.end())
        {
            log_entry["stress_change"] = stress - existing_stress->second;
        }
        else
        {
            log_entry["stress_change"] = 0;
        }
        log_entry["stress"] = stress;
        last_stress[unit->id] = stress;
    }, url);
}

command_result fortmetrics (color_ostream& out, std::vector<std::string>& parameters)
{
    if (parameters.empty())
        return CR_WRONG_USAGE;

    // Commands are called from threads other than the DF one.
    // Suspend this thread until DF has time for us. If you
    // use CoreSuspender, it'll automatically resume DF when
    // execution leaves the current scope.
    CoreSuspender suspend;

    if (!Core::getInstance().isWorldLoaded())
    {
        out.printerr("World is not loaded: please load a game first.\n");
        return CR_FAILURE;
    }

    if (parameters.size() >= 1 && parameters[0] == "debug")
    {
        debug = !debug;
        out.print("%s toggled debug %s\n", fortmetrics_log_id, (debug ? "on" : "off"));
    }
    else if (parameters.size() >= 2 && parameters[0] == "stress")
    {
        if (debug)
        {
            out.print("%s Logging stress to %s\n", fortmetrics_log_id, parameters[1].c_str());
        }
        log_metrics_stress(parameters[1]);
    }
    else
    {
        return CR_WRONG_USAGE;
    }

    return CR_OK;
}
