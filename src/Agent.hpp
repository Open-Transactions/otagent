// Copyright (c) 2018 The Open-Transactions developers
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <opentxs/opentxs.hpp>

#include <atomic>
#include <mutex>
#include <string>

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;
namespace ot = opentxs;
namespace zmq = ot::network::zeromq;
namespace zap = zmq::zap;

namespace opentxs::agent
{
class Agent
{
public:
    Agent(
        const api::Context& app,
        const std::int64_t clients,
        const std::int64_t servers,
        const std::string& socket_path,
        const std::vector<std::string>& endpoints,
        const std::string& serverPrivateKey,
        const std::string& serverPublicKey,
        const std::string& clientPrivateKey,
        const std::string& clientPublicKey,
        const std::string& settings_path,
        pt::ptree& config);

    ~Agent() = default;

private:
    // connection id, nym id
    using TaskData = std::pair<OTData, std::string>;
    // task id, task data
    using TaskMap = std::map<std::string, TaskData>;
    // nym id, connection id
    using NymMap = std::map<std::string, OTData>;

    const api::Context& ot_;
    const zmq::Context& zmq_;
    std::atomic<std::int64_t> clients_;
    const OTZMQListenCallback internal_callback_;
    const OTZMQDealerSocket internal_;
    const std::vector<std::string> backend_endpoints_;
    const OTZMQReplyCallback backend_callback_;
    const std::vector<OTZMQReplySocket> backends_;
    const std::vector<std::string>& frontend_endpoints_;
    const OTZMQListenCallback frontend_callback_;
    const OTZMQRouterSocket frontend_;
    std::atomic<std::int64_t> servers_;
    const std::string& settings_path_;
    const std::string& socket_path_;
    mutable std::mutex config_lock_;
    pt::ptree& config_;
    const std::string server_privkey_;
    const std::string server_pubkey_;
    const std::string client_privkey_;
    const std::string client_pubkey_;
    mutable std::mutex task_lock_;
    TaskMap task_connection_map_;
    mutable std::mutex nym_lock_;
    NymMap nym_connection_map_;
    const OTZMQListenCallback push_callback_;
    const OTZMQSubscribeSocket push_subscriber_;

    static std::vector<std::string> backend_endpoint_generator();
    static std::vector<OTZMQReplySocket> create_backend_sockets(
        const zmq::Context& zmq,
        const std::vector<std::string>& endpoints,
        const OTZMQReplyCallback& callback);
    static int session_to_client_index(const std::uint32_t session);

    void schedule_refresh(const int instance) const;
    OTZMQZAPReply zap_handler(const zap::Request& request) const;

    void associate_nym(const Data& connection, const std::string& nymID);
    void associate_task(
        const Data& connection,
        const std::string& nymID,
        const std::string& task);
    OTZMQMessage backend_handler(const zmq::Message& message);
    void internal_handler(zmq::Message& message);
    void increment_config_value(
        const std::string& section,
        const std::string& entry);
    OTZMQMessage instantiate_push(const Data& connectionID);
    void frontend_handler(zmq::Message& message);
    void process_task_push(const zmq::Message& message);
    void push_handler(const zmq::Message& message);
    void save_config(const Lock& lock);
    void send_task_push(
        const Data& connectionID,
        const std::string& taskID,
        const std::string& nymID,
        const bool result);
    void update_clients();
    void update_servers();

    Agent() = delete;
    Agent(const Agent&) = delete;
    Agent(Agent&&) = delete;
    Agent& operator=(const Agent&) = delete;
    Agent& operator=(Agent&&) = delete;
};
}  // namespace opentxs::agent
