#ifndef P2P_WEBSOCKET_SESSION_H_
#define P2P_WEBSOCKET_SESSION_H_

#include <cstdlib>
#include <memory>
#include <string>

// Boost
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/multi_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/system/error_code.hpp>

#include "rtc/rtc_manager.h"
#include "util.h"
#include "watchdog.h"
#include "websocket.h"
#include "PipeClient.h"

struct P2PWebsocketSessionConfig {
  bool no_google_stun = false;
  std::string pipe_name;
  std::unique_ptr<Websocket> ws_;
};

class P2PWebsocketSession
    : public std::enable_shared_from_this<P2PWebsocketSession>,
      public RTCMessageSender,
      public webrtc::DataChannelObserver{
  P2PWebsocketSession(boost::asio::io_context& ioc,
                      boost::asio::ip::tcp::socket socket,
                      RTCManager* rtc_manager,
                      P2PWebsocketSessionConfig config);

 public:
  static std::shared_ptr<P2PWebsocketSession> Create(
      boost::asio::io_context& ioc,
      boost::asio::ip::tcp::socket socket,
      RTCManager* rtc_manager,
      P2PWebsocketSessionConfig config) {
    return std::shared_ptr<P2PWebsocketSession>(new P2PWebsocketSession(
        ioc, std::move(socket), rtc_manager, std::move(config)));
  }

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer& buffer) override; 
  ~P2PWebsocketSession();

  void Run(boost::beast::http::request<boost::beast::http::string_body> req);

  std::shared_ptr<RTCConnection> GetRTCConnection() const;

 private:
  void OnWatchdogExpired();
  void DoAccept(
      boost::beast::http::request<boost::beast::http::string_body> req);
  void OnAccept(boost::system::error_code ec);

  void DoRead();
  void OnRead(boost::system::error_code ec,
              std::size_t bytes_transferred,
              std::string recv_string);

  std::shared_ptr<RTCConnection> CreateRTCConnection();

  void OnScreenCaptureConnected();
  void OnScreenCaptureDisconnected();

 private:
  //WebRTC
  void OnIceConnectionStateChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
  void OnIceCandidate(const std::string sdp_mid,
                      const int sdp_mlineindex,
                      const std::string sdp) override;

 private:
  std::unique_ptr<Websocket> ws_;
  rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;
  WatchDog watchdog_;

  RTCManager* rtc_manager_;
  P2PWebsocketSessionConfig config_;

  std::shared_ptr<RTCConnection> connection_;
  webrtc::PeerConnectionInterface::IceConnectionState rtc_state_;

  bool is_connected_ = false;
  std::unique_ptr<PipeClient> pipe_client_;
};

#endif  // P2P_WEBSOCKET_SESSION_H_
