#include <sys/time.h>
#include <unistd.h>

#include <cassert>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "zmq.hpp"

using namespace std;

const string bind_addr[4] {
    "tcp://*:5555",
    "tcp://*:5556",
    "tcp://*:5557",
    "tcp://*:5558"
};

// change IP to actual ip
const string connect_addr[4] {
    "tcp://IP1:5555",
    "tcp://IP2:5556",
    "tcp://IP3:5557",
    "tcp://IP4:5558"
};

const char* identities[4] {"server1", "server2", "client1", "client2"};

const int PEER_NUM = 2;
const int MSG_SIZE = 1024 * 1024 * 1024; // bytes

class Node {
public:
    Node(int index) : _rankid(index) {}
    ~Node(){}
    virtual int init();
    virtual int run() = 0;
    virtual int clear();
    virtual void log(const string &what, const string &type = "INFO");
    virtual void log_msg(const string &arrow, const string &remote, const string &content);

protected:
    zmq::context_t *_zmq_ctx = nullptr;
    zmq::socket_t *_receiver = nullptr;
    zmq::socket_t *_sync = nullptr;
    unordered_map<string, zmq::socket_t *> _senders;
    const int _peer_num = PEER_NUM;
    int _active_peer_num = PEER_NUM;
    int _send_msg_num = 0;
    int _recv_msg_num = 0;
    int _rankid;
    long long _send_bytes = 0;
    long long _recv_bytes = 0;
    string _my_name;
};

class WorkerNode : public Node {
public:
    WorkerNode(int index) : Node(index) {}
    ~WorkerNode();
    int run() override;
    int barrier();

private:
    int _num_msg;
};

class ServerNode : public Node {
public:
    ServerNode(int index) : Node(index) {}
    ~ServerNode();
    int run() override;

private:
    int _barrier_num;
};


string current_time() {
    timeval tv;
    gettimeofday(&tv, NULL);
    int milli = tv.tv_usec/1000;

    char buffer[80];
    strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", localtime(&tv.tv_sec));
    stringstream ss;
    ss << buffer << ":" << setfill('0') << setw(3) << milli;
    return ss.str();
}

void Node::log(const string &what, const string &type) {
    cout << "[" << type << "]" << " " << current_time() << " " << what << endl;
}

void Node::log_msg(const string &arrow, const string &remote, const string &content) {
    string what = _my_name + " " + arrow + " " + remote + ": " + content;
    log(what);
}

int Node::init() {

    _my_name = identities[_rankid];

    try {
        _zmq_ctx = new zmq::context_t(1);
    } catch (const zmq::error_t &e) {
        cerr << "failed to create zmq context: "
             << e.what() << endl;
        return -1;
    }

    try {
        _receiver = new zmq::socket_t(*_zmq_ctx, ZMQ_ROUTER);
        _receiver->bind(bind_addr[_rankid]);
    } catch (const zmq::error_t &e) {
        cerr << "failed to create/bind receiving socket: "
             << e.what() << endl;
        return -1;
    }

    int start;
    if (_rankid <= 1)
        start = 2;
    else
        start = 0;
    for (int i = start; i < start + _peer_num; ++i) {
        try {
            string remote_name = identities[i];
            zmq::socket_t *s = new zmq::socket_t(*_zmq_ctx, ZMQ_DEALER);
            int linger = 0;
            s->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));
            s->setsockopt(ZMQ_IDENTITY, identities[_rankid], strlen(identities[_rankid]));
            s->connect(connect_addr[i]);
            _senders.insert(make_pair(remote_name, s));
        } catch (const zmq::error_t &e) {
            cerr << "failed to create/connect sending socket: "
                 << e.what() << endl;
            return -1;
        }
    }

    return 0;
}

int WorkerNode::run() {
    srand(time(NULL));
    _num_msg = rand() % 5 + 1;
    stringstream ss;
    ss << _my_name << " is going to send " << _num_msg << " messages";
    log(ss.str());

    int rc;
    // sync with servers
    for (auto const &kv : _senders) {
        zmq::message_t sync("sync", 4), ack;
        rc = kv.second->send(sync);
        log_msg("->", kv.first, "sync");
        assert(rc >= 0);
        _receiver->recv(&sync);
        _receiver->recv(&ack);
        string who(static_cast<char*>(sync.data()), sync.size());
        string content(static_cast<char*>(ack.data()), ack.size());
        log_msg("<-", who, content);
    }

    // do real work
    while (_send_msg_num < _num_msg) {
        for (auto const &kv : _senders) {
            zmq::message_t msg(MSG_SIZE), reply, copy;
            copy.copy(&msg);
            log_msg("->", kv.first, to_string(MSG_SIZE) + " bytes");
            rc = kv.second->send(msg);
            _send_bytes += MSG_SIZE;

            assert(rc >= 0);
            _receiver->recv(&reply);
            _receiver->recv(&msg);

            string who(static_cast<char*>(reply.data()), reply.size());
            string content = to_string(msg.size()) + " bytes";
            log_msg("<-", who, content);
            _recv_msg_num++;
            _recv_bytes += msg.size();

            // integrity check
            assert(msg.size() == copy.size());
            rc = memcmp(copy.data(), msg.data(), msg.size());
            assert(rc == 0);
        }
        _send_msg_num++;
    }

    barrier();

    // notify terminating
    for (auto const &kv : _senders) {
        zmq::message_t term("terminate", 9);
        rc = kv.second->send(term);
        log_msg("->", kv.first, "terminate");
        assert(rc >= 0);
    }

    return 0;
}

int WorkerNode::barrier() {
    log(_my_name + " is doing a barrier");
    int rc = 0;
    for (auto const &kv : _senders) {
        zmq::message_t pause("barrier", 7);
        log_msg("->", kv.first, "barrier");
        rc = kv.second->send(pause);
        assert(rc >= 0);
    }

    int response = 0;
    zmq::message_t sync, ack;
    while (response < _peer_num) {
        _receiver->recv(&sync);
        _receiver->recv(&ack);
        string who(static_cast<char*>(sync.data()), sync.size());
        string content(static_cast<char*>(ack.data()), ack.size());
        log_msg("<-", who, content);
        response++;
    }

    log(_my_name + " finished barrier");

    return 0;
}

int ServerNode::run() {
    zmq::message_t src, msg, reply;
    int rc;
    while (_active_peer_num > 0) {
        _receiver->recv(&src);
        _receiver->recv(&msg);
        string who(static_cast<char*>(src.data()), src.size());
        string content;
        if (msg.size() < 1024) {
            content = string(static_cast<char*>(msg.data()), msg.size());
        } else {
            content = to_string(msg.size()) + " bytes";
            _recv_bytes += msg.size();
        }
        log_msg("<-", who, content);
        if (content == "sync") {
            reply.rebuild("ok", 2);
            log_msg("->", who, "ok");
            rc = _senders[who]->send(reply);
            assert(rc >= 0);
        } else if (content == "terminate") {
            --_active_peer_num;
        } else if (content == "barrier") {
            _barrier_num++;
            if (_barrier_num == _peer_num) {
                _barrier_num = 0;
                for (auto const &kv : _senders) {
                    zmq::message_t cont("continue", 8);
                    rc = kv.second->send(cont);
                    log_msg("->", kv.first, "continue");
                    assert(rc >= 0);
                }
            }
        } else {
            // send back exact message just received
            log_msg("->", who, content);
            _send_bytes += msg.size();
            rc = _senders[who]->send(msg);
            assert(rc >= 0);
        }
    }

    return 0;
}

int Node::clear() {
    log(_my_name + " exiting");
    stringstream ss;
    ss << _my_name << " sent " << _send_bytes/1024.0/1024.0
       << " MB received " << _recv_bytes/1024.0/1024.0 << " MB";
    log(ss.str());

    if (_receiver != nullptr) {
        delete _receiver;
        _receiver = nullptr;
    }

    for (auto &kv : _senders) {
        if (kv.second != nullptr) {
            delete kv.second;
            kv.second = nullptr;
        }
    }

    if (_zmq_ctx != nullptr) {
        delete _zmq_ctx;
        _zmq_ctx = nullptr;
    }
    return 0;
}

int main(int argc, char **argv) {

    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <0|1|2|3>" << endl;
        return -1;
    }

    const int index = atoi(argv[1]);
    int rc;

    Node *node;

    if (index == 0 || index == 1) {
        node = new ServerNode(index);
    } else if (index == 2 || index == 3) {
        node = new WorkerNode(index);
    } else {
        cerr << "wrong index " << index << endl;
        return -1;
    }

    rc = node->init();
    if (rc != 0) {
        cerr << "failed to init" << endl;
        return -1;
    }

    rc = node->run();
    if (rc != 0) {
        cerr << "error occured" << endl;
        return -1;
    }

    rc = node->clear();
    if (rc != 0) {
        cerr << "failed to clear" << endl;
        return -1;
    }

    return 0;
}
