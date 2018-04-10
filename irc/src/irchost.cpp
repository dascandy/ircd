#include <boost/asio.hpp>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <set>
#include <map>

using endpoint_type = boost::asio::ip::tcp::endpoint;
using address_type = boost::asio::ip::address;
using socket_type = boost::asio::ip::tcp::socket;

class peer;

class IIrcHost {
public:
  virtual void handle(std::shared_ptr<peer> client) = 0;
};

class peer : public std::enable_shared_from_this<peer>
{
public:
    boost::asio::ip::tcp::socket sock_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    IIrcHost &host;
public:
    peer(peer const&) = delete;
    peer& operator=(peer const&) = delete;

    peer(socket_type sock, IIrcHost& host)
        : sock_(std::move(sock))
        , strand_(sock_.get_executor())
        , host(host)
    {
    }

    void run()
    {
        sock_.async_read_some(boost::asio::buffer(readBuffer, sizeof(readBuffer)), boost::asio::bind_executor(strand_, [this, self = shared_from_this()](boost::system::error_code ec, size_t size) {
            on_read(ec, size);
        }));
        write("PING :server is testing\r\n");
    }

    void on_read(boost::system::error_code ec, size_t size) {
        if(ec) {
            return;
        }
        inBuffer += std::string(readBuffer, readBuffer + size);
        host.handle(shared_from_this());
        sock_.async_read_some(boost::asio::buffer(readBuffer, sizeof(readBuffer)), boost::asio::bind_executor(strand_, [this, self = shared_from_this()](boost::system::error_code ec, size_t size) {
            on_read(ec, size);
        }));
    }
    
    void write(const std::string& str) {
        printf("sending %s value %s\n", nick.c_str(), str.c_str());
        outBuffer += str;
        if (!writeActive) {
            writeActive = true;
            on_write(boost::system::error_code());
        }
    }

    void on_write(boost::system::error_code ec)
    {
        if(ec) {
            return;
        }

        if (outBuffer.empty()) {
            writeActive = false;
        } else {
            std::swap(currentWrite, outBuffer);
            outBuffer.clear();
            async_write(sock_, boost::asio::buffer(currentWrite.data(), currentWrite.size()), boost::asio::bind_executor(strand_, [this, self = shared_from_this()](boost::system::error_code ec, size_t){ on_write(ec); }));
        }
    }
    bool writeActive = false;
    std::string currentWrite;
    std::string outBuffer;
    std::string inBuffer;
    char readBuffer[4096];
    std::string nick;
};

class IrcHost : public IIrcHost {
public:
  boost::asio::io_context context;
  std::vector<std::shared_ptr<peer>> clients;
  std::map<std::string, std::set<std::shared_ptr<peer>>> channels;
  std::map<std::string, std::function<void(std::shared_ptr<peer>, std::string)>> commands;
  IrcHost() 
    : acceptor_(context)
    , strand_(context)
    , sock_(context)
  {
    boost::asio::ip::tcp::endpoint ep(boost::asio::ip::address_v4::any(), 6667);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen(1000);
    acceptor_.async_accept(sock_, strand_.wrap([this](boost::system::error_code ec){ on_accept(ec); }));
    commands["JOIN"] = [this](std::shared_ptr<peer> p, std::string param){
      auto &channel = channels[param];
      if (channel.find(p) == channel.end()) {
        channel.insert(p);
        for (auto& client : channel) {
          client->write(":" + p->nick + " JOIN " + param + "\r\n");
        }
      }
      p->write(":server 332 " + p->nick + " " + param + " :Channel topic goes here!\r\n");
      //:weber.freenode.net 333 pebi #osdev Mutabah!~tpg@pdpc/supporter/student/thepowersgang 1505616255
    };
    commands["PART"] = [this](std::shared_ptr<peer> p, std::string param){
      auto it = channels[param].find(p);
      if (it != channels[param].end()) {
        for (auto& client : channels[param]) {
          client->write(":" + p->nick + " PART " + param + "\r\n");
        }
        channels[param].erase(it);
      }
    };
    commands["NICK"] = [this](std::shared_ptr<peer> p, std::string param){
      std::string oldnick = p->nick;
      p->nick = param;
      if (!oldnick.empty()) {
        for (auto& client : clients) {
          client->write(":" + oldnick + " NICK " + param + "\r\n");
        }
      } else {
        p->write(":server 001 :" + p->nick + "\r\n");
        p->write(":server 002 :server is testing\r\n");
        p->write(":server 003 :server is testing\r\n");
        p->write(":sereer 004 :server is testing\r\n");
      }
    };
    commands["PRIVMSG"] = [this](std::shared_ptr<peer> p, std::string param){
      std::string target = param.substr(0, param.find_first_of(" "));
      if (target.empty()) return;
      if (channels.find(target) != channels.end()) {
        for (auto& client : channels[target]) {
          if (p != client)
            client->write(":" + p->nick + " PRIVMSG " + param + "\r\n");
        }
      }
    };
    commands["NAMES"] = [this](std::shared_ptr<peer> p, std::string param){
      auto chan = channels[param];
      for (auto& client : chan) {
        p->write(":server 353 " + p->nick + " = " + param + ":" + client->nick + "\r\n");
      }
      p->write(":server 366 " + p->nick + " " + param + " :End of /NAMES list.\r\n");
    };
    commands["PONG"] = [this](std::shared_ptr<peer> p, std::string param){
    };
    commands["PING"] = [this](std::shared_ptr<peer> p, std::string param){
      p->write("PONG " + param + "\r\n");
    };
    commands["QUIT"] = [this](std::shared_ptr<peer> p, std::string ){
      for (auto& ch : channels) {
        if (ch.second.find(p) != ch.second.end()) {
          for (auto& client : ch.second) {
            client->write(":" + p->nick + " PART " + ch.first + "\r\n");
          }
        }
      }
      clients.erase(std::find(clients.begin(), clients.end(), p));
    };
    context.run();
  }
  void handle(std::shared_ptr<peer> client) {
    std::string& buffer = client->inBuffer;
    size_t end = buffer.find_first_of("\n");
    while (end != buffer.npos) {
      std::string sub = buffer.substr(0, end-1);
      printf("Command: %s\n", sub.c_str());
      std::string cmd = sub.substr(0, sub.find_first_of(" "));
      auto it = commands.find(cmd);
      if (it != commands.end()) {
        it->second(client, sub.substr(sub.find_first_of(" ") + 1));
      } else {
        printf("Unimplemented: %s\n", cmd.c_str());
      }
      buffer = buffer.substr(end+1);
      end = buffer.find_first_of("\n"); 
    }
  }
  void on_accept(boost::system::error_code ec)
  {
    if(! acceptor_.is_open())
      return;

    if(ec) {
      return;
    }
    socket_type sock(std::move(sock_));
    acceptor_.async_accept(sock_, strand_.wrap([this](boost::system::error_code ec){ on_accept(ec); }));
    clients.push_back(std::make_shared<peer>(std::move(sock), *this));
    clients.back()->run();
  }
  boost::asio::ip::tcp::acceptor acceptor_;
  boost::asio::io_context::strand strand_;
  socket_type sock_;
};

int main() {
  IrcHost host;

}

