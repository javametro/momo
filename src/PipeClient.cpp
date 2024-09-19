#include <boost/asio/windows/stream_handle.hpp>
#include <iostream>
#include "PipeClient.h"

PipeClient::PipeClient(boost::asio::io_context& ioc,
                       const std::string& pipe_name)
    : ioc_(ioc), pipe_name_(pipe_name) {}

void PipeClient::SendMessage(const std::string& message) {
  try {
    boost::asio::windows::stream_handle pipe(ioc_);
    boost::system::error_code ec;
    pipe.assign(::CreateFileA(pipe_name_.c_str(), GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL),
                ec);

    if (ec) {
      throw boost::system::system_error(ec);
    }

    boost::asio::write(pipe, boost::asio::buffer(message));
    pipe.close(ec);
  } catch (const std::exception& e) {
    std::cerr << "Error sending message through pipe: " << e.what()
              << std::endl;
  }
}