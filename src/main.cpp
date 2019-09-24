// Copyright (c) 2018 The Open-Transactions developers
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <csignal>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

extern "C" {
#include <pwd.h>
#include <unistd.h>
}

#include "Agent.hpp"

#define OT_STORAGE_GC_SECONDS 3600

#define OPTION_CLIENTS "clients"
#define OPTION_SERVERS "servers"
#define OPTION_SOCKET_PATH "socket-path"
#define OPTION_ENDPOINT "endpoint"
#define OPTION_LOG_ENDPOINT "logendpoint"
#define CONFIG_SERVER_PRIVKEY "server_privkey"
#define CONFIG_SERVER_PUBKEY "server_pubkey"
#define CONFIG_CLIENT_PRIVKEY "client_privkey"
#define CONFIG_CLIENT_PUBKEY "client_pubkey"

namespace po = boost::program_options;
namespace pt = boost::property_tree;
namespace fs = boost::filesystem;

// Prepend the section name (and a dot) to the option name.
std::string config_option_name(const char* name);
std::string config_option_name(const char* name)
{
    return std::string("otagent.") + name;
}

void cleanup_globals();
po::variables_map& variables();
po::options_description& options();         // command line options
po::options_description& config_options();  // config file options

static po::variables_map* variables_{};
static po::options_description* options_{};
static po::options_description* config_options_{};

po::variables_map& variables()
{
    if (nullptr == variables_) { variables_ = new po::variables_map; }

    return *variables_;
}

po::options_description& options()
{
    if (nullptr == options_) {
        options_ = new po::options_description{"otagent"};
        options_->add_options()(
            OPTION_CLIENTS,
            po::value<std::int64_t>(),
            "The number of clients to start.")(
            OPTION_SERVERS,
            po::value<std::int64_t>(),
            "The number of servers to start.")(
            OPTION_SOCKET_PATH,
            po::value<std::string>(),
            "The ipc socket path.")(
            OPTION_ENDPOINT,
            po::value<std::vector<std::string>>()->multitoken(),
            "Tcp endpoint(s).")(
            OPTION_LOG_ENDPOINT, po::value<std::string>(), "Log endpoint.");
    }

    return *options_;
}

po::options_description& config_options()
{
    // When parsing a config file, the parser combines the section name and the
    // option name, e.g. otagent.clients
    if (nullptr == options_) {
        config_options_ = new po::options_description{};
        config_options_->add_options()(
            config_option_name(OPTION_CLIENTS).c_str(),
            po::value<std::int64_t>(),
            "The number of clients to start.")(
            config_option_name(OPTION_SERVERS).c_str(),
            po::value<std::int64_t>(),
            "The number of servers to start.")(
            config_option_name(OPTION_SOCKET_PATH).c_str(),
            po::value<std::string>(),
            "The ipc socket path.")(
            config_option_name(OPTION_ENDPOINT).c_str(),
            po::value<std::string>()->multitoken(),
            "Tcp endpoint(s).")(
            config_option_name(CONFIG_SERVER_PRIVKEY).c_str(),
            po::value<std::string>(),
            "Server private key")(
            config_option_name(CONFIG_SERVER_PUBKEY).c_str(),
            po::value<std::string>(),
            "Server public key")(
            config_option_name(CONFIG_CLIENT_PRIVKEY).c_str(),
            po::value<std::string>(),
            "Client private key")(
            config_option_name(CONFIG_CLIENT_PUBKEY).c_str(),
            po::value<std::string>(),
            "Client public key");
    }

    return *config_options_;
}

void cleanup_globals()
{
    if (nullptr != variables_) {
        delete variables_;
        variables_ = nullptr;
    }

    if (nullptr != options_) {
        delete options_;
        options_ = nullptr;
    }

    if (nullptr != config_options_) {
        delete config_options_;
        config_options_ = nullptr;
    }
}

void read_options(int argc, char** argv);
void read_options(int argc, char** argv)
{
    try {
        po::store(po::parse_command_line(argc, argv, options()), variables());
        po::notify(variables());
    } catch (po::error& e) {
        std::cerr << "ERROR: " << e.what() << "\n\n" << options() << std::endl;
    }
}

void read_config_options(std::string config_file_name);
void read_config_options(std::string config_file_name)
{
    try {
        fs::ifstream config_file(config_file_name);
        po::store(
            po::parse_config_file(config_file, config_options()), variables());
        po::notify(variables());
    } catch (po::error& e) {
        std::cerr << "ERROR: " << e.what() << "\n\n"
                  << config_options() << std::endl;
    }
}

std::int64_t max_option_value(std::string name);
std::int64_t max_option_value(std::string name)
{
    std::int64_t command_line_value = 0;
    std::int64_t config_file_value = 0;

    if (!variables()[name].empty()) {
        command_line_value = variables()[name].as<std::int64_t>();
    }

    std::string config_name = config_option_name(name.c_str());
    if (!variables()[config_name].empty()) {
        config_file_value = variables()[config_name].as<std::int64_t>();
    }

    return std::max(command_line_value, config_file_value);
}

// Converts a string containing multiple items separated by spaces to a vector.
std::vector<std::string> string_to_vector(std::string s);
std::vector<std::string> string_to_vector(std::string s)
{
    std::vector<std::string> v;

    std::size_t start = 0;
    std::size_t end = std::string::npos;

    // static_cast gets rid of a clang compiler warning
    while (static_cast<void>(end = s.find(' ', start)),
           end != std::string::npos) {
        v.emplace_back(s.substr(start, end - start));
        while (s[++end] == ' ')  // In case there are multiple spaces.
            ;
        start = end;
    }

    if (0 < start) { v.emplace_back(s.substr(start)); }

    return v;
}

std::string find_home();
std::string find_home()
{
    std::string home_directory;
#ifdef __APPLE__
    home_directory = ot::OTPaths::AppDataFolder().Get();
#else
    std::string environment;
    const char* env = getenv("HOME");

    if (nullptr != env) { environment.assign(env); }

    if (!environment.empty()) {
        home_directory = environment;
    } else {
        passwd* entry = getpwuid(getuid());
        const char* password = entry->pw_dir;
        home_directory.assign(password);
    }

    if (home_directory.empty()) {
        ot::LogOutput(__FUNCTION__)(": Unable to determine the home directory.")
            .Flush();
    }
#endif

    return home_directory;
}

int main(int argc, char** argv)
{
    ot::Signals::Block();
    auto settings_path = find_home() + "/.otagent";
    read_config_options(settings_path);
    read_options(argc, argv);
    auto opts = variables();

    ot::ArgList args;
    if (!variables()[OPTION_LOG_ENDPOINT].empty()) {
        args[OPTION_LOG_ENDPOINT].emplace(
            variables()[OPTION_LOG_ENDPOINT].as<std::string>());
    }

    const auto& ot =
        ot::InitContext(args, std::chrono::seconds(OT_STORAGE_GC_SECONDS));

    // Use the max of the values from the command line and the config file.
    std::int64_t clients = max_option_value(OPTION_CLIENTS);
    std::int64_t servers = max_option_value(OPTION_SERVERS);
    // Once the socket_path is saved to the config file, don't change the value
    // in the file.
    std::string config_socket_path;

    if (!variables()[config_option_name(OPTION_SOCKET_PATH)].empty()) {
        config_socket_path = variables()[config_option_name(OPTION_SOCKET_PATH)]
                                 .as<std::string>();
    }

    std::string socket_path = config_socket_path;

    // Use the socket_path from the command line, if it exists.
    if (!variables()[OPTION_SOCKET_PATH].empty()) {
        socket_path = variables()[OPTION_SOCKET_PATH].as<std::string>();
    }

    // Use a default socket path.
    if (socket_path.empty()) {
        std::string uid = std::to_string(getuid());
        std::string dir = "/run/user/" + uid;
        fs::path path(dir);
        fs::file_status status = fs::status(path);
        if (0 != (status.permissions() & fs::owner_write)) {
            socket_path = dir + "/otagent.sock";
        } else {
            dir = "/tmp/user/" + uid;
            path = dir;
            status = fs::status(path);
            if (0 != (status.permissions() & fs::owner_write)) {
                socket_path = dir + "/otagent.sock";
            }
        }
    }

    // Combine the endpoints from the command line and the config file.
    std::vector<std::string> endpoints;

    if (!variables()[config_option_name(OPTION_ENDPOINT)].empty()) {
        auto config_endpoints_string =
            variables()[config_option_name(OPTION_ENDPOINT)].as<std::string>();
        auto config_endpoints = string_to_vector(config_endpoints_string);
        for (auto& ep : config_endpoints) {
            if (std::find(endpoints.begin(), endpoints.end(), ep) ==
                endpoints.end()) {
                endpoints.emplace_back(ep);
            }
        }
    }

    if (!variables()[OPTION_ENDPOINT].empty()) {
        auto command_endoints =
            variables()[OPTION_ENDPOINT].as<std::vector<std::string>>();
        for (auto& ep : command_endoints) {
            if (std::find(endpoints.begin(), endpoints.end(), ep) ==
                endpoints.end()) {
                endpoints.emplace_back(ep);
            }
        }
    }

    std::string server_private_key{};
    std::string server_public_key{};
    auto& serverPrivkey =
        variables()[config_option_name(CONFIG_SERVER_PRIVKEY)];
    auto& serverPubkey = variables()[config_option_name(CONFIG_SERVER_PUBKEY)];
    const bool needServerKeys = serverPrivkey.empty() || serverPubkey.empty();

    if (needServerKeys) {
        std::cout << "Generating new server keypair." << std::endl;
        auto [generatedSecret, generatedPublic] =
            ot::network::zeromq::curve::Client::RandomKeypair();

        OT_ASSERT(false == generatedSecret.empty())
        OT_ASSERT(false == generatedPublic.empty())

        server_private_key = generatedSecret;
        server_public_key = generatedPublic;
    } else {
        server_private_key = ot.Crypto().Encode().DataDecode(
            boost::any_cast<std::string>(serverPrivkey.value()));
        server_public_key = ot.Crypto().Encode().DataDecode(
            boost::any_cast<std::string>(serverPubkey.value()));
    }

    std::string client_private_key{};
    std::string client_public_key{};
    auto& clientPrivkey =
        variables()[config_option_name(CONFIG_CLIENT_PRIVKEY)];
    auto& clientPubkey = variables()[config_option_name(CONFIG_CLIENT_PUBKEY)];
    const bool needClientKeys = clientPrivkey.empty() || clientPubkey.empty();

    if (needClientKeys) {
        std::cout << "Generating new client keypair." << std::endl;
        auto [generatedSecret, generatedPublic] =
            ot::network::zeromq::curve::Client::RandomKeypair();

        OT_ASSERT(false == generatedSecret.empty())
        OT_ASSERT(false == generatedPublic.empty())

        client_private_key = generatedSecret;
        client_public_key = generatedPublic;
    } else {
        client_private_key = ot.Crypto().Encode().DataDecode(
            boost::any_cast<std::string>(clientPrivkey.value()));
        client_public_key = ot.Crypto().Encode().DataDecode(
            boost::any_cast<std::string>(clientPubkey.value()));
    }

    {
        std::stringstream ss{};
        ss << R"~({)~"
           << "\n";
        ss << R"~(  "otagent": {)~"
           << "\n";
        ss << R"~(    ")~" << CONFIG_SERVER_PUBKEY;
        ss << R"~(": ")~";
        ss << server_public_key;
        ss << R"~(",)~"
           << "\n";
        ss << R"~(    ")~" << CONFIG_CLIENT_PRIVKEY;
        ss << R"~(": ")~";
        ss << client_private_key;
        ss << R"~(",)~"
           << "\n";
        ss << R"~(    ")~" << CONFIG_CLIENT_PUBKEY;
        ss << R"~(": ")~";
        ss << client_public_key;
        ss << R"~(")~"
           << "\n";
        ss << R"~(  })~"
           << "\n";
        ss << R"~(})~"
           << "\n";
        std::ofstream settingsfile(find_home() + "/otagent.key");
        settingsfile << ss.str();
        settingsfile.close();
    }

    pt::ptree root;
    pt::ptree section;
    section.put(OPTION_CLIENTS, clients);
    section.put(OPTION_SERVERS, servers);
    // Only save the socket_path from the command line if it hasn't been
    // saved before.
    section.put(
        OPTION_SOCKET_PATH,
        config_socket_path.empty() ? socket_path : config_socket_path);

    // Save the endpoints as a single entry in the config file, with the
    // endpoints separated by spaces.
    if (0 < endpoints.size()) {
        std::string endpoints_string = endpoints[0];
        for (auto& ep :
             std::vector<std::string>(++endpoints.begin(), endpoints.end()))
            endpoints_string += ' ' + ep;
        section.put(OPTION_ENDPOINT, endpoints_string);
    }

    root.push_front(pt::ptree::value_type("otagent", section));
    fs::fstream settingsfile(settings_path, std::ios::out);
    pt::write_ini(settingsfile, root);
    settingsfile.close();
    std::unique_ptr<ot::agent::Agent> otagent;
    otagent.reset(new ot::agent::Agent(
        ot,
        clients,
        servers,
        std::string("ipc://") + socket_path,
        endpoints,
        server_private_key,
        server_public_key,
        client_private_key,
        client_public_key,
        settings_path,
        root));
    std::function<void()> shutdowncallback = [&otagent]() -> void {
        ot::LogNormal(__FUNCTION__)(": Shutting down...").Flush();
        otagent.reset();
    };
    ot::Context().HandleSignals(&shutdowncallback);
    ot::Join();
    cleanup_globals();

    return 0;
}
