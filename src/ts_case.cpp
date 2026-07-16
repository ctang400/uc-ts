// TsCase 实现。MD 翻译逻辑与 uc-mm/strat/oms_test_case.cpp 保持一致
// (含 Binance LINEAR 只放行 MARKET trade 的过滤)；shm 广播与 hft/md/main.cpp
// 的 QuoteServer::book_message 保持一致(消息格式/CLEAR 处理/心跳)。
// include 顺序: logger_define.h 必须早于首次引入 quill 的头(见 uc-mm 同名注释)。
#include "utils/logger_define.h"

#include "ts_case.h"

#include "utils/config_helper.h"
#include "utils/misc.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
// ResponseHeader.account_id 是定长 char[32], 不保证以 '\0' 结尾
inline std::string account_str(const oms::ResponseHeader &header) {
  return std::string(header.account_id,
                     strnlen(header.account_id, sizeof(header.account_id)));
}
// order_id / trade_id 是数字字符串(binance), 转 uint64; 非数字得 0
inline uint64_t id_to_u64(const char *id) { return strtoull(id, nullptr, 10); }
} // namespace

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
    writers_.push_back(
        new ShmWriter("/md_" + rule->symbol_name, SHM_MD_CAPACITY));
    writers_[cid]->connect();
  }

  init_order_channels();

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
  for (auto &[id, ch] : order_channels_)
    ch->rsp_writer.reset_hb(now);
}

// ---------------------------------------------------------------------------
// 下单通道
// ---------------------------------------------------------------------------

void TsCase::init_order_channels() {
  // sid=0 = 交易 symbol list 第一个; oms 交易 symbol 格式与
  // uc-mm/strat/UCTraderMaker_1.cpp 一致: "btc-usdt" (quote-base 小写)
  const auto *rule0 = uni_.symbol_rule(0);
  oms_symbol_name_ = enums::Asset::Enum_Name(rule0->quote) + "-" +
                     enums::Asset::Enum_Name(rule0->base);
  std::transform(oms_symbol_name_.begin(), oms_symbol_name_.end(),
                 oms_symbol_name_.begin(), ::tolower);

  const auto &venues =
      utils::ConfigHelper::getInstance()->getSetting("prod.venues");
  for (int i = 0; i < venues.getLength(); i++) {
    const auto &venue = venues[i];
    const int vendor = venue["vendor"];
    const int market = venue["market"];
    const auto &accounts = venue["accounts"];
    for (int j = 0; j < accounts.getLength(); j++) {
      const char *acc_c = accounts[j]["account_id"];
      const std::string acc(acc_c);
      // 通道名 /order_src_{N} 要求数字账户; 非数字(如 bn_spot_account1)跳过
      if (acc.empty() ||
          acc.find_first_not_of("0123456789") != std::string::npos) {
        WARNING("account_id:{} not numeric, no order channel", acc);
        continue;
      }
      if (order_channels_.count(acc))
        TW("duplicated account_id:{} in prod.venues", acc);
      auto *ch = new OrderChannel(this, acc, std::stoull(acc));
      ch->oms_header.vendor = (oms::Vendor)vendor;
      ch->oms_header.market = (oms::Market)market;
      string_to_char_array(acc, ch->oms_header.account_id);
      ch->src_reader.connect();
      ch->rsp_writer.connect();
      order_channels_[acc] = ch;
      INFO("order channel account:{} /order_src_{} /order_rsp_{} vendor:{} "
           "market:{} symbol:{}",
           acc, ch->account_num, ch->account_num, vendor, market,
           oms_symbol_name_);
    }
  }
  if (order_channels_.empty())
    WARNING("no numeric account in prod.venues, order channel disabled");
}

void TsCase::on_order_msg(OrderChannel &ch, const void *data) {
  const shm::Header *header = (const shm::Header *)data;
  const char *body = (const char *)data + sizeof(shm::Header);
  auto &oms = getOrderManager(ch.oms_header.vendor, ch.oms_header.market);
  switch (header->type) {
  case shm::OrderMsgType::NEW_ORDER: {
    const shm::NewOrder *req = (const shm::NewOrder *)body;
    if (req->sid != 0) { // 两侧约定 sid 恒为 0, 其他值直接拒单
      ERROR("account:{} invalid sid:{} oid:{}", ch.account_str, req->sid,
            req->oid);
      shm::OrderReject rej = {req->oid, 0};
      send_rsp(ch.account_str, shm::OrderMsgType::ORDER_REJECT, &rej,
               sizeof(rej));
      break;
    }
    oms::NewOrder oms_order{};
    oms_order.client_order_id = req->oid;
    string_to_char_array(oms_symbol_name_, oms_order.symbol);
    oms_order.price = req->price;
    oms_order.qty = req->qty;
    oms_order.side = (oms::Side)req->side;
    oms_order.type = oms::OrderType::LIMIT;
    oms_order.tif = req->order_type == enums::OrderType::GTX
                        ? oms::OrderTif::GTX
                        : oms::OrderTif::GTC;
    if (req->reduce_only)
      oms_order.option_type |= oms::REQ_OPTION_REDUCE_ONLY_ORDER;
    // req->recv_window_ms: 目前 pandora 下单不设置 recv_window, 只透传不使用
    oms.sendOrder(&ch.oms_header, &oms_order);
  } break;
  case shm::OrderMsgType::CANCEL_ORDER: {
    const shm::CancelOrder *req = (const shm::CancelOrder *)body;
    oms.cancelOrder(req->oid);
  } break;
  default:
    ERROR("account:{} invalid order msg type:{}", ch.account_str,
          (int)header->type);
    break;
  }
}

void TsCase::send_rsp(const std::string &account_id, uint8_t type,
                      const void *body, size_t body_len) {
  auto it = order_channels_.find(account_id);
  if (it == order_channels_.end()) {
    // 其他账户(未开通道)的回报直接丢弃
    DEBUG("rsp type:{} for unknown account:{}, dropped", (int)type,
          account_id);
    return;
  }
  shm::Header *header = (shm::Header *)rsp_buf_;
  header->type = type;
  header->seqnum = it->second->rsp_seqnum++;
  memcpy(rsp_buf_ + sizeof(shm::Header), body, body_len);
  it->second->rsp_writer.write(rsp_buf_, sizeof(shm::Header) + body_len);
}

void TsCase::onOrderAcked(const oms::ResponseHeader &header,
                          const oms::OrderUpdate &msg) {
  shm::Ack ack = {msg.client_order_id, id_to_u64(msg.order_id)};
  send_rsp(account_str(header), shm::OrderMsgType::ACK, &ack, sizeof(ack));
}

void TsCase::onOrderCanceled(const oms::ResponseHeader &header,
                             const oms::OrderUpdate &msg) {
  shm::Canceled cxl = {msg.client_order_id};
  send_rsp(account_str(header), shm::OrderMsgType::CANCELED, &cxl,
           sizeof(cxl));
}

void TsCase::onOrderExpired(const oms::ResponseHeader &header,
                            const oms::OrderUpdate &msg) {
  // 与 uc-mm 一致: EXPIRED 按 CANCELED 处理
  shm::Canceled cxl = {msg.client_order_id};
  send_rsp(account_str(header), shm::OrderMsgType::CANCELED, &cxl,
           sizeof(cxl));
}

void TsCase::onOrderFilled(const oms::ResponseHeader &header,
                           const oms::OrderUpdate &msg) {
  shm::Fill fill = {};
  fill.oid = msg.client_order_id;
  fill.tid = id_to_u64(msg.trade_id);
  fill.filled_price = static_cast<double>(msg.last_fill_price);
  fill.filled_size = static_cast<double>(msg.last_fill_qty);
  fill.filled = static_cast<double>(msg.filled); // pandora 提供累计成交量
  fill.is_maker = (msg.liq == oms::Liquidity::MAKER);
  send_rsp(account_str(header), shm::OrderMsgType::FILL, &fill, sizeof(fill));
}

void TsCase::onOrderRejected(const oms::ResponseHeader &header,
                             const oms::ErrorMsg &msg) {
  // 错误码映射与 uc-mm/strat/oms_test_case.cpp 一致
  shm::OrderReject rej = {msg.client_order_id, 0};
  if (msg.exchange_error_code == -5022) // GTX post-only 被吃, 预期行为不打日志
    rej.reason = enums::ErrorCode::MKT_REJECT_FAIL_EXECUTED_AS_MAKER;
  else {
    ERROR("new order[{}] reject: internal ec:{} exchange ec:{} internal "
          "reason:{} exchange reason:{}",
          msg.client_order_id, msg.internal_error_code, msg.exchange_error_code,
          msg.internal_error_msg, msg.exchange_error_msg);
    if (msg.exchange_error_code == -1008)
      rej.reason = enums::ErrorCode::MKT_REJECT_MARKET_DOWN;
  }
  send_rsp(account_str(header), shm::OrderMsgType::ORDER_REJECT, &rej,
           sizeof(rej));
}

void TsCase::onCancelRejected(const oms::ResponseHeader &header,
                              const oms::ErrorMsg &msg) {
  shm::CancelReject rej = {msg.client_order_id, 0};
  if (msg.exchange_error_code == -2011) // 订单不存在, 多为已成交
    rej.reason = enums::ErrorCode::CXL_REJECT_ORDER_NOT_FOUND;
  else {
    if (msg.exchange_error_code == -1008)
      rej.reason = enums::ErrorCode::MKT_REJECT_MARKET_DOWN;
    ERROR("cxl order[{}] reject: internal ec:{} exchange ec:{} internal "
          "reason:{} exchange reason:{}",
          msg.client_order_id, msg.internal_error_code, msg.exchange_error_code,
          msg.internal_error_msg, msg.exchange_error_msg);
  }
  send_rsp(account_str(header), shm::OrderMsgType::CANCEL_REJECT, &rej,
           sizeof(rej));
}

void TsCase::onUnifiedErrorResp(const oms::ResponseHeader &header,
                                const oms::ErrorMsg &msg) {
  ERROR("unified error[{}]: internal ec:{} exchange ec:{} internal reason:{} "
        "exchange reason:{}",
        msg.client_order_id, msg.internal_error_code, msg.exchange_error_code,
        msg.internal_error_msg, msg.exchange_error_msg);
}

void TsCase::onMdLoaderEnd() { INFO("onMdLoaderEnd"); }
