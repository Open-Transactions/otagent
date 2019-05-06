// Copyright (c) 2018 The Open-Transactions developers
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <boost/filesystem.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <thread>

#include "Agent.hpp"

#define CONFIG_SECTION "otagent"
#define CONFIG_CLIENTS "clients"
#define CONFIG_SERVERS "servers"
#define CONFIG_SERVER_PRIVKEY "server_privkey"
#define CONFIG_SERVER_PUBKEY "server_pubkey"
#define CONFIG_CLIENT_PRIVKEY "client_privkey"
#define CONFIG_CLIENT_PUBKEY "client_pubkey"
#define RPCPUSH_VERSION 2
#define TASKCOMPLETE_VERSION 1

namespace fs = boost::filesystem;

#define ZAP_DOMAIN "otagent"

#define OT_METHOD "opentxs::Agent::"

namespace opentxs::agent
{
Agent::Agent(
    const api::Native& app,
    const std::int64_t clients,
    const std::int64_t servers,
    const std::string& socket_path,
    const std::vector<std::string>& endpoints,
    const std::string& serverPrivateKey,
    const std::string& serverPublicKey,
    const std::string& clientPrivateKey,
    const std::string& clientPublicKey,
    const std::string& settings_path,
    pt::ptree& config)
    : ot_(app)
    , zmq_(app.ZMQ())
    , clients_(clients)
    , internal_callback_(zmq::ListenCallback::Factory(
          std::bind(&Agent::internal_handler, this, std::placeholders::_1)))
    , internal_(zmq_.DealerSocket(
          internal_callback_,
          zmq::Socket::Direction::Connect))
    , backend_endpoints_(backend_endpoint_generator())
    , backend_callback_(zmq::ReplyCallback::Factory(
          std::bind(&Agent::backend_handler, this, std::placeholders::_1)))
    , backends_(
          create_backend_sockets(zmq_, backend_endpoints_, backend_callback_))
    , frontend_endpoints_(endpoints)
    , frontend_callback_(zmq::ListenCallback::Factory(
          std::bind(&Agent::frontend_handler, this, std::placeholders::_1)))
    , frontend_(
          zmq_.RouterSocket(frontend_callback_, zmq::Socket::Direction::Bind))
    , servers_(servers)
    , settings_path_(settings_path)
    , socket_path_(socket_path)
    , config_lock_()
    , config_(config)
    , server_privkey_(serverPrivateKey)
    , server_pubkey_(serverPublicKey)
    , client_privkey_(clientPrivateKey)
    , client_pubkey_(clientPublicKey)
    , task_lock_()
    , task_connection_map_()
    , nym_lock_()
    , nym_connection_map_()
    , push_callback_(zmq::ListenCallback::Factory(
          std::bind(&Agent::push_handler, this, std::placeholders::_1)))
    , push_subscriber_(zmq_.SubscribeSocket(push_callback_))
{
    {
        Lock lock(config_lock_);
        auto& section = config.get_child(CONFIG_SECTION);
        section.put(
            CONFIG_SERVER_PRIVKEY,
            ot_.Crypto().Encode().DataEncode(server_privkey_));
        section.put(
            CONFIG_SERVER_PUBKEY,
            ot_.Crypto().Encode().DataEncode(server_pubkey_));
        section.put(
            CONFIG_CLIENT_PRIVKEY,
            ot_.Crypto().Encode().DataEncode(client_privkey_));
        section.put(
            CONFIG_CLIENT_PUBKEY,
            ot_.Crypto().Encode().DataEncode(client_pubkey_));
        save_config(lock);
    }

    for (auto i = 0; i < servers_.load(); ++i) {
        ot_.StartServer(ArgList(), i, false);
    }

    for (auto i = 0; i < clients_.load(); ++i) {
        ot_.StartClient(ArgList(), i);
    }

    OT_ASSERT(0 < backend_endpoints_.size())

    auto started{false};

    for (const auto& endpoint : backend_endpoints_) {
        started = internal_->Start(endpoint);

        OT_ASSERT(started)
    }

    OT_ASSERT(false == socket_path_.empty())

    const auto zap = ot_.ZAP().RegisterDomain(
        ZAP_DOMAIN,
        std::bind(&Agent::zap_handler, this, std::placeholders::_1));

    OT_ASSERT(zap)

    const auto domain = frontend_->SetDomain(ZAP_DOMAIN);

    OT_ASSERT(domain)

    const bool set = frontend_->SetPrivateKey(server_privkey_);

    OT_ASSERT(set)

    started = frontend_->Start(socket_path_);

    OT_ASSERT(started)

    for (const auto& endpoint : frontend_endpoints_) {
        started = frontend_->Start(endpoint);

        OT_ASSERT(started)
    }

    OT_ASSERT(0 <= clients_.load())

    for (int i = 1; i <= clients_.load(); ++i) { schedule_refresh(i - 1); }

    started =
        push_subscriber_->Start(ot_.ZMQ().BuildEndpoint("rpc/push", -1, 1));

    OT_ASSERT(started)
}

void Agent::associate_nym(const Data& connection, const std::string& nymID)
{
    if (nymID.empty()) { return; }

    auto it = nym_connection_map_.find(nymID);

    if (nym_connection_map_.end() == it) {
        Lock lock(task_lock_);
        nym_connection_map_.emplace(nymID, connection);
        lock.unlock();
        LogOutput(OT_METHOD)(__FUNCTION__)(": Connection ")(connection.asHex())(
            " is associated with nym ")(nymID)
            .Flush();
    }
}

void Agent::associate_task(
    const Data& connection,
    const std::string& nymID,
    const std::string& task)
{
    OT_ASSERT(false == connection.empty())
    OT_ASSERT(false == nymID.empty())
    OT_ASSERT(false == task.empty())

    LogOutput(OT_METHOD)(__FUNCTION__)(": Connection ")(connection.asHex())(
        " is waiting for task ")(task)
        .Flush();
    Lock lock(task_lock_);
    task_connection_map_.emplace(task, TaskData{connection, nymID});
}

std::vector<std::string> Agent::backend_endpoint_generator()
{
    const unsigned int min_threads{1};
    const auto threads =
        std::max(std::thread::hardware_concurrency(), min_threads);
    LogNormal(OT_METHOD)(__FUNCTION__)(": Starting ")(threads)(
        " handler threads.")
        .Flush();
    std::vector<std::string> output{};
    const auto prefix = std::string("inproc://opentxs/agent/backend/");

    for (unsigned int i{0}; i < threads; ++i) {
        output.emplace_back(prefix + std::to_string(i));
    }

    return output;
}

OTZMQMessage Agent::backend_handler(const zmq::Message& message)
{
    OT_ASSERT(1 < message.Body().size())

    const auto& request = message.Body().at(0);
    const auto data = Data::Factory(request.data(), request.size());
    const auto command =
        opentxs::proto::DataToProto<opentxs::proto::RPCCommand>(data);
    const auto connectionID = Data::Factory(message.Body().at(1));
    for (auto nym : command.associatenym()) {
        associate_nym(connectionID, nym);
    }
    auto response = ot_.RPC(command);
    std::string taskNymID{};

    switch (response.type()) {
        case proto::RPCCOMMAND_ADDCLIENTSESSION: {
            if (0 < response.status_size() &&
                proto::RPCRESPONSE_SUCCESS == response.status(0).code()) {
                update_clients();
            }
        } break;
        case proto::RPCCOMMAND_ADDSERVERSESSION: {
            if (0 < response.status_size() &&
                proto::RPCRESPONSE_SUCCESS == response.status(0).code()) {
                update_servers();
            }
        } break;
        case proto::RPCCOMMAND_CREATENYM: {
            for (const auto& nymid : response.identifier()) {
                associate_nym(connectionID, nymid);
            }
        } break;
        case proto::RPCCOMMAND_REGISTERNYM:
        case proto::RPCCOMMAND_ISSUEUNITDEFINITION:
        case proto::RPCCOMMAND_CREATEACCOUNT:
        case proto::RPCCOMMAND_CREATECOMPATIBLEACCOUNT: {
            taskNymID = command.owner();
        } break;
        case proto::RPCCOMMAND_SENDPAYMENT: {
            if (0 < response.status_size() &&
                proto::RPCRESPONSE_QUEUED == response.status(0).code()) {
                const auto accountID =
                    Identifier::Factory(command.sendpayment().sourceaccount());
                taskNymID =
                    ot_.Client(session_to_client_index(command.session()))
                        .Storage()
                        .AccountOwner(accountID)
                        ->str();
            }
        } break;
        case proto::RPCCOMMAND_ACCEPTPENDINGPAYMENTS: {
            if (0 < response.status_size() &&
                proto::RPCRESPONSE_QUEUED == response.status(0).code()) {
                const auto accountID = Identifier::Factory(
                    command.acceptpendingpayment(0).destinationaccount());
                taskNymID =
                    ot_.Client(session_to_client_index(command.session()))
                        .Storage()
                        .AccountOwner(accountID)
                        ->str();
            }
        } break;
        case proto::RPCCOMMAND_LISTCLIENTSESSIONS:
        case proto::RPCCOMMAND_LISTSERVERSESSIONS:
        case proto::RPCCOMMAND_IMPORTHDSEED:
        case proto::RPCCOMMAND_LISTHDSEEDS:
        case proto::RPCCOMMAND_GETHDSEED:
        case proto::RPCCOMMAND_LISTNYMS:
        case proto::RPCCOMMAND_GETNYM:
        case proto::RPCCOMMAND_ADDCLAIM:
        case proto::RPCCOMMAND_DELETECLAIM:
        case proto::RPCCOMMAND_IMPORTSERVERCONTRACT:
        case proto::RPCCOMMAND_LISTSERVERCONTRACTS:
        case proto::RPCCOMMAND_CREATEUNITDEFINITION:
        case proto::RPCCOMMAND_LISTUNITDEFINITIONS:
        case proto::RPCCOMMAND_LISTACCOUNTS:
        case proto::RPCCOMMAND_GETACCOUNTBALANCE:
        case proto::RPCCOMMAND_GETACCOUNTACTIVITY:
        case proto::RPCCOMMAND_MOVEFUNDS:
        case proto::RPCCOMMAND_ADDCONTACT:
        case proto::RPCCOMMAND_LISTCONTACTS:
        case proto::RPCCOMMAND_GETCONTACT:
        case proto::RPCCOMMAND_ADDCONTACTCLAIM:
        case proto::RPCCOMMAND_DELETECONTACTCLAIM:
        case proto::RPCCOMMAND_VERIFYCLAIM:
        case proto::RPCCOMMAND_ACCEPTVERIFICATION:
        case proto::RPCCOMMAND_SENDCONTACTMESSAGE:
        case proto::RPCCOMMAND_GETCONTACTACTIVITY:
        case proto::RPCCOMMAND_GETSERVERCONTRACT:
        case proto::RPCCOMMAND_GETPENDINGPAYMENTS:
        case proto::RPCCOMMAND_GETCOMPATIBLEACCOUNTS:
        case proto::RPCCOMMAND_GETWORKFLOW:
        case proto::RPCCOMMAND_GETSERVERPASSWORD:
        case proto::RPCCOMMAND_GETADMINNYM:
        case proto::RPCCOMMAND_GETUNITDEFINITION:
        case proto::RPCCOMMAND_GETTRANSACTIONDATA:
        case proto::RPCCOMMAND_LOOKUPACCOUNTID:
        case proto::RPCCOMMAND_RENAMEACCOUNT:
        case proto::RPCCOMMAND_ERROR:
        default: {
        }
    }

    if (0 < response.status_size() &&
        proto::RPCRESPONSE_QUEUED == response.status(0).code()) {

        if (0 < response.task_size()) {
            const auto& taskID = response.task(0).id();
            associate_task(connectionID, taskNymID, taskID);
        }
    }

    auto replymessage = zmq::Message::ReplyFactory(message);
    const auto replydata =
        opentxs::proto::ProtoAsData<opentxs::proto::RPCResponse>(response);
    replymessage->AddFrame(replydata);

    return replymessage;
}

std::vector<OTZMQReplySocket> Agent::create_backend_sockets(
    const zmq::Context& zmq,
    const std::vector<std::string>& endpoints,
    const OTZMQReplyCallback& callback)
{
    bool started{false};
    std::vector<OTZMQReplySocket> output{};

    for (const auto& endpoint : endpoints) {
        output.emplace_back(
            zmq.ReplySocket(callback, zmq::Socket::Direction::Bind));
        auto& socket = *output.rbegin();
        started = socket->Start(endpoint);

        OT_ASSERT(started)

        LogNormal(endpoint).Flush();
    }

    return output;
}

void Agent::frontend_handler(zmq::Message& message)
{
    const auto size = message.Header().size();

    OT_ASSERT(0 < size)

    if (0 == message.Body().size()) {
        LogOutput(OT_METHOD)(__FUNCTION__)(": Empty command.").Flush();

        return;
    }

    // Append connection identity for push notification purposes
    const auto& identity = message.Header_at(size - 1);

    OT_ASSERT(0 < identity.size())

    LogVerbose(OT_METHOD)(__FUNCTION__)(": ConnectionID: ")(
        Data::Factory(identity)->asHex())
        .Flush();
    message.AddFrame(Data::Factory(identity));
    // Forward requests to backend socket(s) via internal socket
    internal_->Send(message);
}

void Agent::increment_config_value(
    const std::string& sectionName,
    const std::string& entryName)
{
    Lock lock(config_lock_);
    pt::ptree& section = config_.get_child(sectionName);
    pt::ptree& entry = section.get_child(entryName);
    auto value = entry.get_value<std::int64_t>();
    entry.put_value<std::int64_t>(++value);
    save_config(lock);
}

OTZMQMessage Agent::instantiate_push(const Data& connectionID)
{
    OT_ASSERT(0 < connectionID.size())

    auto output = zmq::Message::Factory();
    output->AddFrame(connectionID);
    output->AddFrame();
    output->AddFrame("PUSH");

    OT_ASSERT(1 == output->Header().size())
    OT_ASSERT(1 == output->Body().size())

    return output;
}

void Agent::internal_handler(zmq::Message& message)
{
    // Route replies back to original requestor via frontend socket
    frontend_->Send(message);
}

void Agent::process_task_push(const zmq::Message& message)
{
    const auto& payload = message.Body_at(0);
    const auto data = Data::Factory(payload.data(), payload.size());
    const auto push = proto::DataToProto<proto::RPCPush>(data);
    const auto taskcomplete = push.taskcomplete();
    const auto taskID = taskcomplete.id();
    const auto success = taskcomplete.result();

    Lock lock(task_lock_);
    const auto it = task_connection_map_.find(taskID);

    if (task_connection_map_.end() == it) {
        LogDebug(OT_METHOD)(__FUNCTION__)(": We don't care about task ")(taskID)
            .Flush();

        return;
    }

    const OTData connectionID = it->second.first;
    const std::string nymID = it->second.second;
    task_connection_map_.erase(it);
    lock.unlock();

    OT_ASSERT(false == nymID.empty())

    send_task_push(connectionID, taskID, nymID, success);
}

void Agent::push_handler(const zmq::Message& message)
{
    if (1 == message.Body().size()) {
        process_task_push(message);
        return;
    }

    if (3 != message.Body().size()) {
        LogOutput(OT_METHOD)(__FUNCTION__)(": Invalid message").Flush();

        return;
    }

    const std::string nymID{message.Body_at(0)};
    const auto& payload = message.Body_at(1);
    const auto& instance = message.Body_at(2);
    auto connection = Data::Factory();

    try {
        Lock lock(nym_lock_);
        connection = nym_connection_map_.at(nymID);
    } catch (...) {
        LogNormal(OT_METHOD)(__FUNCTION__)(": No connection associated with ")(
            nymID)
            .Flush();

        return;
    }

    auto notification = instantiate_push(connection);
    notification->AddFrame(payload);
    notification->AddFrame(instance);
    const auto sent = frontend_->Send(notification);

    if (sent) {
        LogNormal(OT_METHOD)(__FUNCTION__)(": Push notification delivered to ")(
            nymID)(" via ")(connection->asHex())
            .Flush();
    } else {
        LogOutput(OT_METHOD)(__FUNCTION__)(
            ": Push notification delivery failed")
            .Flush();
    }
}

void Agent::save_config(const Lock& lock)
{
    fs::fstream settingsfile(settings_path_, std::ios::out);
    pt::write_ini(settings_path_, config_);
    settingsfile.close();
}

void Agent::schedule_refresh(const int instance) const
{
    const auto& client = ot_.Client(instance);
    client.OTX().Refresh();
    client.Schedule(
        std::chrono::seconds(30),
        [=]() -> void { this->ot_.Client(instance).OTX().Refresh(); },
        (std::chrono::seconds(std::time(nullptr))));
}

void Agent::send_task_push(
    const Data& connectionID,
    const std::string& taskID,
    const std::string& nymID,
    const bool result)
{
    OT_ASSERT(false == connectionID.empty())
    OT_ASSERT(false == taskID.empty())
    OT_ASSERT(false == nymID.empty())

    auto push = instantiate_push(connectionID);
    proto::RPCPush message{};
    message.set_version(RPCPUSH_VERSION);
    message.set_type(proto::RPCPUSH_TASK);
    message.set_id(nymID);
    auto& task = *message.mutable_taskcomplete();
    task.set_version(TASKCOMPLETE_VERSION);
    task.set_id(taskID);
    task.set_result(result);

    OT_ASSERT(proto::Validate(message, VERBOSE))

    push->AddFrame(proto::ProtoAsData(message));
    frontend_->Send(push);
}

int Agent::session_to_client_index(const std::uint32_t session)
{
    OT_ASSERT(0 == session % 2)

    return session / 2;
}

void Agent::update_clients()
{
    increment_config_value(CONFIG_SECTION, CONFIG_CLIENTS);
    const auto newCount = ++clients_;
    const auto newIndex = static_cast<int>(newCount) - 1;
    schedule_refresh(newIndex);
}

void Agent::update_servers()
{
    increment_config_value(CONFIG_SECTION, CONFIG_SERVERS);
    ++servers_;
}

OTZMQZAPReply Agent::zap_handler(const zap::Request& request) const
{
    auto output = zap::Reply::Factory(request);
    const auto& pubkey = request.Credentials().at(0);

    if (zap::Mechanism::Curve != request.Mechanism()) {
        output->SetCode(zap::Status::AuthFailure);
        output->SetStatus("Unsupported mechanism");
    } else if (client_pubkey_ != ot_.Crypto().Encode().Z85Encode(pubkey)) {
        output->SetCode(zap::Status::AuthFailure);
        output->SetStatus("Incorrect pubkey");
    } else {
        output->SetCode(zap::Status::Success);
        output->SetStatus("OK");
    }

    return output;
}
}  // namespace opentxs::agent
