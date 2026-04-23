#include "asos/Orchestrator.hpp"
#include "asos/Blackboard.hpp"
#include "buffer.hpp"
#include <iostream>
#include <chrono>

namespace asos {

Orchestrator::~Orchestrator() {
    stop();
}

void Orchestrator::start(Blackboard* bb, const std::string& location_id, int pub_port, int sub_port) {
    blackboard_ = bb;
    location_id_ = location_id;
    running_ = true;

    heartbeat_thread_ = std::thread(&Orchestrator::heartbeat_loop, this);
    listen_thread_ = std::thread(&Orchestrator::listen_loop, this);
    
    std::cout << "[Orchestrator] ASOS Distributed Blackboard Active (ZMQ port " << pub_port << ")" << std::endl;
}

void Orchestrator::stop() {
    running_ = false;
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (listen_thread_.joinable()) listen_thread_.join();
}

void Orchestrator::broadcast_atom(const CpbEntry& entry) {
    if (!running_) return;

    try {
        zmq::socket_t pub(ctx_, ZMQ_PUB);
        pub.connect("tcp://127.0.0.1:8090"); // Connect to our own PUB socket's "loopback" or use a shared one.
        // Actually, in a multi-thread ZMQ app, we should share a socket or use a separate context.
        // For MVP, we'll open a dedicated PUB socket and connect to the local relay.
        // Or better: just use the existing pub in heartbeat_loop? No, that's private to that thread.
        
        // Proper way: Heartbeat loop owns the PUB socket, we send to it via inproc.
        // But for this sandbox, we'll just open a new one.
        zmq::socket_t data_pub(ctx_, ZMQ_PUB);
        data_pub.connect("tcp://127.0.0.1:8090");
        
        std::string topic = "ATOM:" + entry.header.uuid;
        lite3cpp::Buffer buf;
        entry.serialize(buf);

        data_pub.send(zmq::message_t(topic.data(), topic.size()), zmq::send_flags::sndmore);
        data_pub.send(zmq::message_t(buf.data(), buf.size()), zmq::send_flags::none);
    } catch (...) {}
}

void Orchestrator::dispatch_nucleus_command(const std::string& json_payload) {
    if (!running_) return;

    try {
        // Nucleus MCP Control Plane is on port 5556
        zmq::socket_t req(ctx_, ZMQ_REQ);
        req.set(zmq::sockopt::linger, 100);
        req.set(zmq::sockopt::rcvtimeo, 500); // Don't block forever if Nucleus is down
        req.connect("tcp://127.0.0.1:5556");

        std::cout << "[Orchestrator] Dispatching command to Nucleus: " << json_payload << std::endl;
        req.send(zmq::message_t(json_payload.data(), json_payload.size()), zmq::send_flags::none);
        
        zmq::message_t reply;
        auto res = req.recv(reply, zmq::recv_flags::none);
        if (res) {
            std::string reply_str(static_cast<const char*>(reply.data()), reply.size());
            std::cout << "[Orchestrator] Nucleus Response: " << reply_str << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Orchestrator] Failed to dispatch to Nucleus: " << e.what() << std::endl;
    }
}

void Orchestrator::heartbeat_loop() {
    zmq::socket_t pub(ctx_, ZMQ_PUB);
    pub.bind("tcp://*:8090"); 

    while (running_) {
        NodeHeartbeat hb;
        hb.location_id = location_id_;
        hb.connectivity_state = (state_ == State::CONNECTED) ? "CONNECTED" : "ISOLATED";
        hb.sync_epoch = std::chrono::steady_clock::now().time_since_epoch().count();

        lite3cpp::Buffer buf;
        hb.serialize(buf);
        
        std::string topic = "HB:" + location_id_;
        pub.send(zmq::message_t(topic.data(), topic.size()), zmq::send_flags::sndmore);
        pub.send(zmq::message_t(buf.data(), buf.size()), zmq::send_flags::none);

        {
            std::lock_guard<std::mutex> lock(nodes_mutex_);
            auto now = std::chrono::steady_clock::now().time_since_epoch().count() / 1000000; // ms
            
            for (auto it = remote_nodes_.begin(); it != remote_nodes_.end(); ) {
                if (now - it->second.last_seen > 5000) { // Increased timeout for stability
                    std::cout << "[Orchestrator] Node lost (timeout): " << it->first << std::endl;
                    it = remote_nodes_.erase(it);
                } else {
                    ++it;
                }
            }

            if (remote_nodes_.empty()) {
                state_ = State::ISOLATED;
            } else {
                state_ = State::CONNECTED;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void Orchestrator::listen_loop() {
    zmq::socket_t sub(ctx_, ZMQ_SUB);
    sub.connect("tcp://127.0.0.1:8090"); 
    sub.set(zmq::sockopt::subscribe, "HB:");
    sub.set(zmq::sockopt::subscribe, "ATOM:");

    while (running_) {
        zmq::message_t topic_msg;
        auto res = sub.recv(topic_msg, zmq::recv_flags::dontwait);
        if (res) {
            zmq::message_t payload_msg;
            sub.recv(payload_msg, zmq::recv_flags::none);
            
            std::string topic(static_cast<const char*>(topic_msg.data()), topic_msg.size());
            
            if (topic.starts_with("HB:")) {
                std::string peer_id = topic.substr(3);
                if (peer_id == location_id_) continue;

                lite3cpp::Buffer buf(std::vector<uint8_t>(static_cast<const uint8_t*>(payload_msg.data()), 
                                    static_cast<const uint8_t*>(payload_msg.data()) + payload_msg.size()));
                NodeHeartbeat h = NodeHeartbeat::deserialize(buf);
                
                {
                    std::lock_guard<std::mutex> lock(nodes_mutex_);
                    auto now = std::chrono::steady_clock::now().time_since_epoch().count() / 1000000;
                    remote_nodes_[h.location_id] = { h, now };
                }
            } 
            else if (topic.starts_with("ATOM:")) {
                if (!blackboard_) continue;

                lite3cpp::Buffer buf(std::vector<uint8_t>(static_cast<const uint8_t*>(payload_msg.data()), 
                                    static_cast<const uint8_t*>(payload_msg.data()) + payload_msg.size()));
                CpbEntry atom = CpbEntry::deserialize(buf);
                
                // Mirror to local blackboard
                // semantic_merge handles conflict resolution (better_than logic)
                if (blackboard_->semantic_merge(atom)) {
                    std::cout << "[Orchestrator] Synced ATOM from swarm: " << atom.header.uuid << std::endl;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Higher freq for data
    }
}

} // namespace asos
