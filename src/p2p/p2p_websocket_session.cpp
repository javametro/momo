#include "p2p_websocket_session.h"

// Boost
#include <boost/asio/bind_executor.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/json.hpp>

#include "PipeClient.h"
#include "util.h"

std::shared_ptr<RTCConnection> P2PWebsocketSession::GetRTCConnection() const {
  if (rtc_state_ == webrtc::PeerConnectionInterface::IceConnectionState::
                        kIceConnectionConnected) {
    return connection_;
  } else {
    return nullptr;
  }
}

P2PWebsocketSession::P2PWebsocketSession(boost::asio::io_context& ioc,
                                         boost::asio::ip::tcp::socket socket,
                                         RTCManager* rtc_manager,
                                         P2PWebsocketSessionConfig config)
    : ws_(new Websocket(std::move(socket))),
      rtc_manager_(rtc_manager),
      config_(std::move(config)),
      watchdog_(ioc, std::bind(&P2PWebsocketSession::OnWatchdogExpired, this)),
      pipe_client_(std::make_unique<PipeClient>(ioc, config_.pipe_name)) {
  RTC_LOG(LS_INFO) << __FUNCTION__
                   << "config_.pipe_name: " << config_.pipe_name;
}

P2PWebsocketSession::~P2PWebsocketSession() {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (is_connected_) {
    OnScreenCaptureDisconnected();
  }
}

void P2PWebsocketSession::Run(
    boost::beast::http::request<boost::beast::http::string_body> req) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  DoAccept(std::move(req));
}

void P2PWebsocketSession::OnWatchdogExpired() {
  boost::json::value ping_message = {
      {"type", "ping"},
  };
  ws_->WriteText(boost::json::serialize(ping_message));
  watchdog_.Reset();
}

void P2PWebsocketSession::DoAccept(
    boost::beast::http::request<boost::beast::http::string_body> req) {
  ws_->Accept(std::move(req),
              std::bind(&P2PWebsocketSession::OnAccept, shared_from_this(),
                        std::placeholders::_1));
}

void P2PWebsocketSession::OnAccept(boost::system::error_code ec) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": " << ec;

  if (ec)
    return MOMO_BOOST_ERROR(ec, "Accept");

  DoRead();
}

void P2PWebsocketSession::DoRead() {
  ws_->Read(std::bind(&P2PWebsocketSession::OnRead, shared_from_this(),
                      std::placeholders::_1, std::placeholders::_2,
                      std::placeholders::_3));
}

void P2PWebsocketSession::OnStateChange() {
  if (data_channel_) {
    RTC_LOG(LS_INFO) << "DataChannel state changed to "
                     << data_channel_->state();
  }
}

void P2PWebsocketSession::OnMessage(const webrtc::DataBuffer& buffer) {
  std::string message(buffer.data.data<char>(), buffer.data.size());
  RTC_LOG(LS_INFO) << "Received Datachannel message: " << message;

  // Echo back the received message through the data channel
  rtc::CopyOnWriteBuffer echo_buffer(message);
  webrtc::DataBuffer echo_data(echo_buffer, false);
  if (data_channel_ &&
      data_channel_->state() == webrtc::DataChannelInterface::kOpen) {
    data_channel_->Send(echo_data);
  }

  // Also send to websocket for compatibility
  boost::json::object json_message = {{"type", "datachannel"},
                                      {"message", message}};

  ws_->WriteText(boost::json::serialize(json_message));
}

void P2PWebsocketSession::OnRead(boost::system::error_code ec,
                                 std::size_t bytes_transferred,
                                 std::string recv_string) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": " << ec;

  boost::ignore_unused(bytes_transferred);

  if (ec == boost::beast::websocket::error::closed)
    return;

  if (ec)
    return MOMO_BOOST_ERROR(ec, "Read");

  // これ以降はエラーが起きて処理を中断したとしても DoRead() する
  struct Guard {
    std::function<void()> f;
    ~Guard() { f(); }
  } guard = {[this]() { DoRead(); }};

  RTC_LOG(LS_INFO) << __FUNCTION__ << ": recv_string=" << recv_string;

  boost::system::error_code jec;
  boost::json::value recv_message = boost::json::parse(recv_string, jec);
  if (jec) {
    return;
  }

  std::string type = recv_message.at("type").as_string().c_str();

  if (type == "offer") {
    std::string sdp = recv_message.at("sdp").as_string().c_str();

    connection_ = CreateRTCConnection();
    connection_->SetOffer(sdp, [this]() {
      connection_->CreateAnswer(
          [this](webrtc::SessionDescriptionInterface* desc) {
            std::string sdp;
            desc->ToString(&sdp);
            boost::json::value json_desc = {{"type", "answer"}, {"sdp", sdp}};
            std::string str_desc = boost::json::serialize(json_desc);
            ws_->WriteText(std::move(str_desc));
          });
    });
  } else if (type == "answer") {
    if (!connection_) {
      return;
    }
    std::string sdp = recv_message.at("sdp").as_string().c_str();
    connection_->SetAnswer(sdp);
  } else if (type == "candidate") {
    if (!connection_) {
      return;
    }
    boost::json::value ice = recv_message.at("ice");
    std::string sdp_mid = ice.at("sdpMid").as_string().c_str();
    int sdp_mlineindex = ice.at("sdpMLineIndex").to_number<int>();
    std::string candidate = ice.at("candidate").as_string().c_str();
    connection_->AddIceCandidate(sdp_mid, sdp_mlineindex, candidate);
  } else if (type == "close" || type == "bye") {
    if (is_connected_) {
      is_connected_ = false;
      OnScreenCaptureDisconnected();
    }
    connection_ = nullptr;
  } else if (type == "register") {
    boost::json::value accept_message = {
        {"type", "accept"},
        {"isExistUser", true},
    };
    ws_->WriteText(boost::json::serialize(accept_message));
    watchdog_.Enable(30);
  } else {
    return;
  }
}

std::shared_ptr<RTCConnection> P2PWebsocketSession::CreateRTCConnection() {
  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
  webrtc::PeerConnectionInterface::IceServers servers;
  if (!config_.no_google_stun) {
    webrtc::PeerConnectionInterface::IceServer ice_server;
    ice_server.uri = "stun:stun.l.google.com:19302";
    servers.push_back(ice_server);
    ice_server.uri = "stun:stun1.l.google.com:19302";
    servers.push_back(ice_server);
    ice_server.uri = "stun:stun2.l.google.com:19302";
    servers.push_back(ice_server);
  }

  rtc_config.servers = servers;
  rtc_config.ice_connection_receiving_timeout = 5000;         // 5 seconds
  rtc_config.ice_backup_candidate_pair_ping_interval = 2000;  // 2 seconds
  rtc_config.continual_gathering_policy =
      webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;

  auto connection = rtc_manager_->CreateConnection(rtc_config, this);
  rtc_manager_->InitTracks(connection.get());

  webrtc::DataChannelInit config;
  config.ordered = true;
  config.reliable = true;

  auto result = connection->GetConnection()->CreateDataChannelOrError(
      "testdatachannel", &config);
  if (result.ok()) {
    data_channel_ = result.value();
    RTC_LOG(LS_INFO) << "Datachannel created successfully.";
    data_channel_->RegisterObserver(this);
  } else {
    RTC_LOG(LS_ERROR) << "Failed to create DataChannel: "
                      << result.error().message();
  }
  return connection;
}

void P2PWebsocketSession::OnIceConnectionStateChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " rtc_state "
                   << Util::IceConnectionStateToString(rtc_state_) << " -> "
                   << Util::IceConnectionStateToString(new_state);

  rtc_state_ = new_state;

  if (connection_) {
    class StatsCallback : public webrtc::RTCStatsCollectorCallback {
     public:
      void OnStatsDelivered(
          const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report)
          override {
        std::string stats;
        for (const auto& stat : *report) {
          stats += stat.ToJson() + "\n";
        }
        RTC_LOG(LS_INFO) << "ICE Candidates gathered: " << stats;
      }
    };

    connection_->GetConnection()->GetStats(
        new rtc::RefCountedObject<StatsCallback>());
  }

  if (new_state == webrtc::PeerConnectionInterface::IceConnectionState::
                       kIceConnectionConnected) {
    if (!is_connected_) {
      is_connected_ = true;
      OnScreenCaptureConnected();
    }
  } else if (new_state == webrtc::PeerConnectionInterface::IceConnectionState::
                              kIceConnectionDisconnected ||
             new_state == webrtc::PeerConnectionInterface::IceConnectionState::
                              kIceConnectionClosed ||
             new_state == webrtc::PeerConnectionInterface::IceConnectionState::
                              kIceConnectionFailed) {
    if (is_connected_) {
      is_connected_ = false;
      OnScreenCaptureDisconnected();
    }
  }
}

void P2PWebsocketSession::OnIceCandidate(const std::string sdp_mid,
                                         const int sdp_mlineindex,
                                         const std::string sdp) {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  boost::json::object json_cand = {{"type", "candidate"}};
  json_cand["ice"] = {{"candidate", sdp},
                      {"sdpMLineIndex", sdp_mlineindex},
                      {"sdpMid", sdp_mid}};
  std::string str_cand = boost::json::serialize(json_cand);
  ws_->WriteText(str_cand);
}

void P2PWebsocketSession::OnScreenCaptureConnected() {
  RTC_LOG(LS_INFO) << "Screen capture connection established";
  if (pipe_client_) {
    pipe_client_->SendMessage("SCREEN_CAPTURE_CONNECTED");
    RTC_LOG(LS_INFO) << "Send Message: SCREEN_CAPTURE_CONNECTED";
  }
}

void P2PWebsocketSession::OnScreenCaptureDisconnected() {
  RTC_LOG(LS_INFO) << "Screen capture connection disconnected";
  if (pipe_client_) {
    pipe_client_->SendMessage("SCREEN_CAPTURE_DISCONNECTED");
    RTC_LOG(LS_INFO) << "Send Message: SCREEN_CAPTURE_DISCONNECTED";
  }
}
