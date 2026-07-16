// TsCase 实现。MD 翻译逻辑与 uc-mm/strat/oms_test_case.cpp 保持一致
// (含 Binance LINEAR 只放行 MARKET trade 的过滤)；shm 广播与 hft/md/main.cpp
// 的 QuoteServer::book_message 保持一致(消息格式/CLEAR 处理/心跳)。
// include 顺序: logger_define.h 必须早于首次引入 quill 的头(见 uc-mm 同名注释)。
#include "utils/logger_define.h"

#include "ts_case.h"

#include <algorithm>

void TsCase::init(const ConfigFileParser &parser) {
  const auto sys_config_file =
      parser.get<std::string>("environment", "sys_config_file");

  uni_.init(parser);
  if (!uni_.production())
    TW("ucts is live-only");
  StrategyBase::init(sys_config_file);

  const auto now = Timestamp::now();
  const int64_t timeout_interval_ms = 100;
  live_timer_.init(Timestamp(now.nsec() / (timeout_interval_ms * 1000000LL) *
                             (timeout_interval_ms * 1000000LL)),
                   Timestamp::max_time(),
                   Duration::from_nsec(timeout_interval_ms * 1000000LL), this);
  INFO("live timer start:{} delta:{}ms", live_timer_.start().to_date_time(),
       live_timer_.delta().msec());

  for (size_t cid = 0; cid < uni_.num_symbols(); cid++) {
    const auto *rule = uni_.symbol_rule(cid);
    // 与 hft/md/main.cpp 完全一致: /md_{symbol_name}, capacity 1<<14;
    // hft 侧 ShmMDSession 以 "md_"+symbol_name 打开同一对象。
    writers_.push_back(new ShmWriter("/md_" + rule->symbol_name, 1 << 14));
    writers_[cid]->connect();
  }

  requestMdLoader();
}

inline const std::string
TsCase::translate_symbol_name(Exchange::Vendor vendor, Exchange::Market market,
                              const Exchange::Symbol &symbol) {
  std::string symbol_name;
  switch (vendor) {
  case Exchange::Vendor::BINANCE:
    symbol_name += "BINANCE";
    break;
  default:
    TW("invalid vendor:{}", vendor);
  }
  switch (market) {
  case Exchange::Market::LINEAR:
    symbol_name += "_PERP";
    break;
  case Exchange::Market::SPOT:
    symbol_name += "_SPOT";
    break;
  default:
    TW("invalid market:{}", market);
  }
  symbol_name += "_";
  if (vendor == Exchange::Vendor::BINANCE) {
    symbol_name += symbol;
    std::replace(symbol_name.begin(), symbol_name.end(), '-', '_');
    std::transform(symbol_name.begin(), symbol_name.end(), symbol_name.begin(),
                   ::toupper);
  }
  return symbol_name;
}

void TsCase::publish(const MarketDataMessage &msg) {
  switch (msg.type) {
  case enums::EventType::TRADE:
  case enums::EventType::BBO:
  case enums::EventType::DIFF:
  case enums::EventType::SNAPSHOT: {
    shm::Header *header = (shm::Header *)send_buf_;
    header->type = msg.type;
    shm::Quote *body = (shm::Quote *)(send_buf_ + sizeof(shm::Header));
    body->price = msg.price;
    body->qty = msg.qty;
    body->side = msg.side;
    body->exchange_time = msg.exchange_time.nsec();
    body->is_packet_end = msg.is_packet_end;
    writers_[msg.cid]->write(send_buf_,
                             sizeof(shm::Header) + sizeof(shm::Quote));
  } break;
  case enums::EventType::CLEAR: {
    shm::Header *header = (shm::Header *)send_buf_;
    header->type = msg.type;
    writers_[msg.cid]->write(send_buf_, sizeof(shm::Header));
  } break;
  default:
    ERROR("invalid message type:{}", (int)msg.type);
    break;
  }
}

inline void TsCase::onTrade(const MD::Trade &trade) {
  msg_.cid =
      uni_.cid(translate_symbol_name(trade.vendor, trade.market, trade.symbol));
  if (msg_.cid == Universe::INVALID_CID)
    return;
  if (trade.vendor == oms::Vendor::BINANCE &&
      trade.market == oms::Market::LINEAR && trade.r1 != "MARKET")
    return;
  msg_.type = enums::EventType::TRADE;
  msg_.price = static_cast<double>(trade.price);
  msg_.qty = static_cast<double>(trade.quantity);
  msg_.side = (enums::Side::Enum)trade.side;
  msg_.local_time = Timestamp(trade.localTimestamp);
  msg_.exchange_time = Timestamp(trade.exchangeTimestamp);
  msg_.is_packet_end = true;
  publish(msg_);
}

inline void TsCase::onOrderbook(const MD::Orderbook &orderbook) {
  msg_.cid = uni_.cid(translate_symbol_name(orderbook.vendor, orderbook.market,
                                            orderbook.symbol));
  if (msg_.cid == Universe::INVALID_CID)
    return;
  size_t idx = 0;
  msg_.type = orderbook.isSnapshot ? enums::EventType::SNAPSHOT
                                   : enums::EventType::DIFF;
  msg_.local_time = Timestamp(orderbook.localTimestamp);
  msg_.exchange_time = Timestamp(orderbook.exchangeTimestamp);
  msg_.side = enums::Side::BUY;
  for (const auto &s : orderbook.bids) {
    msg_.price = static_cast<double>(s.first);
    msg_.qty = static_cast<double>(s.second);
    msg_.is_packet_end =
        (idx == orderbook.bids.size() - 1 && orderbook.asks.empty());
    publish(msg_);
    idx++;
  }
  idx = 0;
  msg_.side = enums::Side::SELL;
  for (const auto &s : orderbook.asks) {
    msg_.price = static_cast<double>(s.first);
    msg_.qty = static_cast<double>(s.second);
    msg_.is_packet_end = (idx == orderbook.asks.size() - 1);
    publish(msg_);
    idx++;
  }
}

inline void TsCase::onBookTicker(const MD::BookTicker &book_ticker) {
  msg_.cid = uni_.cid(translate_symbol_name(
      book_ticker.vendor, book_ticker.market, book_ticker.symbol));
  if (msg_.cid == Universe::INVALID_CID)
    return;
  msg_.type = enums::EventType::BBO;
  msg_.local_time = Timestamp(book_ticker.localTimestamp);
  msg_.exchange_time = Timestamp(book_ticker.exchangeTimestamp);
  msg_.price = static_cast<double>(book_ticker.bidPrice);
  msg_.qty = static_cast<double>(book_ticker.bidQuantity);
  msg_.is_packet_end = false;
  msg_.side = enums::Side::BUY;
  publish(msg_);
  msg_.price = static_cast<double>(book_ticker.askPrice);
  msg_.qty = static_cast<double>(book_ticker.askQuantity);
  msg_.is_packet_end = true;
  msg_.side = enums::Side::SELL;
  publish(msg_);
}

void TsCase::on_timer(const Timestamp now) {
  // 心跳: reader 1s 无心跳会重连; 100ms tick 刷新绰绰有余
  for (auto *writer : writers_)
    writer->reset_hb(now);
}

void TsCase::onMdLoaderEnd() { INFO("onMdLoaderEnd"); }
