#ifndef PIPE_CLIENT_H_
#define PIPE_CLIENT_H_

#include <boost/asio.hpp>
#include <string>

class PipeClient {
 public:
  PipeClient(boost::asio::io_context& ioc, const std::string& pipe_name);
  void SendMessage(const std::string& message);

 private:
  boost::asio::io_context& ioc_;
  std::string pipe_name_;
};

#endif  // PIPE_CLIENT_H_