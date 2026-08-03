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
#include <unordered_map>

namespace {
// ResponseHeader.account_id 是定长 char[32], 不保证以 '\0' 结尾
inline std::string account_str(const oms::ResponseHeader &header) {
  return std::string(header.account_id,
                     strnlen(header.account_id, sizeof(header.account_id)));
}
// order_id / trade_id 是数字字符串(binance), 转 uint64; 非数字得 0
inline uint64_t id_to_u64(const char *id) { return strtoull(id, nullptr, 10); }
// pandora exchange_timestamp 为 ns; shm 回执 update_time 统一用 us
inline uint64_t rsp_us(const oms::ResponseHeader &h) {
  return h.exchange_timestamp / 1000;
}
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
    books_.push_back(new LevelBook(cid, uni_));
  }

  init_order_channels();

  // md 订阅由 universe 驱动(subscribeMd), 不再依赖 cfg 的静态 symbols 列表
  subscribe_md_from_universe();

  requestMdLoader();
}

// universe 交易所 -> md 数据来源 venue 的 (vendor, market):
// 股票交易所(NYSEARCA 等)的数据经 IBKR 进来且约定 market=MARGIN(4);
// crypto 按合约类型取 LINEAR/SPOT。validate 与订阅共用此映射。
std::pair<Exchange::Vendor, Exchange::Market>
TsCase::md_venue(const SymbolRuleExt *rule) {
  const auto market = rule->er.contract_type == enums::ContractType::PERP
                          ? Exchange::Market::LINEAR
                          : Exchange::Market::SPOT;
  switch (rule->er.exchange) {
  case enums::Exchange::BINANCE:
    return {Exchange::Vendor::BINANCE, market};
  case enums::Exchange::BITGET:
    return {Exchange::Vendor::BITGET, market};
  case enums::Exchange::NYSEARCA:
    return {Exchange::Vendor::IBKR, Exchange::Market::MARGIN};
  default:
    TW("symbol:{} unsupported exchange", rule->symbol_name);
  }
  return {Exchange::Vendor::BINANCE, market}; // unreachable (TW throws)
}

// 逐 symbol 订阅 trade/orderbook/bookticker 三个 topic;
// symbol 为 "quote-base" 小写(oms_symbol_names_, 与 oms 交易名同构)。
// 任一订阅失败(返回 nullopt)直接 TW: 缺流的 md 通道会永远空转, 宁可拒绝启动。
void TsCase::subscribe_md_from_universe() {
  static const std::pair<MD::TopicType, const char *> kTopics[] = {
      {MD::TopicType::Trade, "trade"},
      {MD::TopicType::Orderbook, "orderbook"},
      {MD::TopicType::BookTicker, "bookticker"},
  };
  for (size_t cid = 0; cid < uni_.num_symbols(); cid++) {
    const auto *rule = uni_.symbol_rule(cid);
    const auto [vendor, market] = md_venue(rule);
    for (const auto &[topic, topic_name] : kTopics) {
      const auto tid =
          subscribeMd(vendor, market, topic, oms_symbol_names_[cid]);
      if (!tid)
        TW("subscribeMd failed: {} vendor:{} market:{} topic:{}",
           rule->symbol_name, (int)vendor, (int)market, topic_name);
      INFO("subscribeMd {} vendor:{} market:{} topic:{} -> topic_id:{}",
           oms_symbol_names_[cid], (int)vendor, (int)market, topic_name, *tid);
    }
  }
}

// cfg prod.venues 里的 vendor 字段是字面量整数, 与 Exchange::Vendor 枚举序号
// 必须一致; 上游若在枚举中间插入/重排会静默错位, 用 static_assert 钉死。
static_assert((int)Exchange::Vendor::BINANCE == 0, "Vendor enum reordered");
static_assert((int)Exchange::Vendor::BITGET == 7, "Vendor enum reordered");
static_assert((int)Exchange::Vendor::IBKR == 11, "Vendor enum reordered");
static_assert((int)Exchange::Market::MARGIN == 4, "Market enum reordered");

inline const std::string
TsCase::translate_symbol_name(Exchange::Vendor vendor, Exchange::Market market,
                              const Exchange::Symbol &symbol) {
  std::string symbol_name;
  switch (vendor) {
  case Exchange::Vendor::BINANCE:
    symbol_name += "BINANCE";
    break;
  case Exchange::Vendor::BITGET:
    symbol_name += "BITGET";
    break;
  case Exchange::Vendor::IBKR: {
    // 股票: ticker 可能来自不同交易所(NYSE Arca/NASDAQ/...), 无法像 crypto
    // 一样从 vendor+market 机械拼出 —— 用硬编码映射逐个接入。未收录的
    // ticker 返回空串, 调用方 uni_.cid("")==INVALID_CID 直接跳过。
    // IBKR 数据当前只做接收与翻译(md 转发), 不接下单通道。
    static const std::unordered_map<std::string, std::string> stock_map = {
        {"soxl-usd", "NYSEARCA_SPOT_SOXL_USD"},
    };
    std::string ticker(symbol);
    std::transform(ticker.begin(), ticker.end(), ticker.begin(), ::tolower);
    const auto iter = stock_map.find(ticker);
    return iter == stock_map.end() ? "" : iter->second;
  }
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
  if (vendor == Exchange::Vendor::BINANCE ||
      vendor == Exchange::Vendor::BITGET) {
    symbol_name += symbol;
    std::replace(symbol_name.begin(), symbol_name.end(), '-', '_');
    std::transform(symbol_name.begin(), symbol_name.end(), symbol_name.begin(),
                   ::toupper);
  }
  return symbol_name;
}

void TsCase::write_quote(size_t cid, uint8_t type, double price, double qty,
                         uint8_t side, bool is_packet_end,
                         int64_t exchange_time) {
  shm::Header *header = (shm::Header *)send_buf_;
  header->type = type;
  shm::Quote *body = (shm::Quote *)(send_buf_ + sizeof(shm::Header));
  body->price = price;
  body->qty = qty;
  body->side = side;
  body->exchange_time = exchange_time;
  body->is_packet_end = is_packet_end;
  writers_[cid]->write(send_buf_, sizeof(shm::Header) + sizeof(shm::Quote));
}

void TsCase::publish(const MarketDataMessage &msg) {
  switch (msg.type) {
  case enums::EventType::TRADE:
  case enums::EventType::BBO:
  case enums::EventType::DIFF:
  case enums::EventType::SNAPSHOT: {
    write_quote(msg.cid, msg.type, msg.price, msg.qty, msg.side,
                msg.is_packet_end, msg.exchange_time.nsec());
    // 本地簿与转发流保持一致, 供新上线策略的 SNAPSHOT 重放
    books_[msg.cid]->book_message(msg);
  } break;
  case enums::EventType::CLEAR: {
    shm::Header *header = (shm::Header *)send_buf_;
    header->type = msg.type;
    writers_[msg.cid]->write(send_buf_, sizeof(shm::Header));
    books_[msg.cid]->book_message(msg);
  } break;
  default:
    ERROR("invalid message type:{}", (int)msg.type);
    break;
  }
}

// 从本地簿向所有 md 通道重放 SNAPSHOT(与交易所快照同构, 策略端 LevelBook
// 幂等重建)。空簿跳过 —— 宁可让策略再等周期快照, 不发误导性的空簿。
void TsCase::send_snapshots(const Timestamp now) {
  using namespace enums::Side;
  for (size_t cid = 0; cid < uni_.num_symbols(); cid++) {
    const auto &bids = books_[cid]->levels(BUY);
    const auto &asks = books_[cid]->levels(SELL);
    if (bids.empty() || asks.empty()) {
      WARNING("snapshot skip {}: local book empty",
              uni_.symbol_rule(cid)->symbol_name);
      continue;
    }
    for (auto it = bids.begin(); it != bids.end(); ++it)
      write_quote(cid, enums::EventType::SNAPSHOT, it->first,
                  it->second.total_size(), BUY, false, now.nsec());
    for (auto it = asks.begin(); it != asks.end(); ++it)
      write_quote(cid, enums::EventType::SNAPSHOT, it->first,
                  it->second.total_size(), SELL,
                  std::next(it) == asks.end(), now.nsec());
    INFO("snapshot sent {}: {} bids {} asks",
         uni_.symbol_rule(cid)->symbol_name, bids.size(), asks.size());
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
  // 心跳: md/rsp writer 每 100ms tick 刷新
  for (auto *writer : writers_)
    writer->reset_hb(now);
  for (auto &[id, ch] : order_channels_) {
    ch->rsp_writer.reset_hb(now);
    // 策略上线探测: 策略侧 ShmTrader 每 1s 刷 order_src 心跳;
    // 3s 内有心跳视为在线, 离线->在线的沿触发快照排期。
    // session(incarnation) 变化 = 策略快速重启(心跳来不及变陈旧), 同样
    // 视为上线沿补发快照。
    const uint64_t hb = ch->src_reader.heartbeat();
    const uint64_t sess = ch->src_reader.session();
    const bool restarted =
        ch->last_session != 0 && sess != 0 && sess != ch->last_session;
    if (sess != 0)
      ch->last_session = sess;
    if (restarted) {
      WARNING("strategy account:{} restarted (session change)",
              ch->account_str);
      ch->online = false; // 走下面的上线沿逻辑重新排期快照
    }
    const bool online = hb != 0 && now < Timestamp(hb) + Duration::from_sec(3);
    if (online && !ch->online) {
      // 不立即发: 等下一整秒, 同窗口上线的多个策略合并成一次广播
      if (snapshot_due_ == Timestamp())
        snapshot_due_ = Timestamp((now.nsec() / 1000000000LL + 1) * 1000000000LL);
      INFO("strategy account:{} online, snapshot due {}", ch->account_str,
           snapshot_due_.to_date_time());
    } else if (!online && ch->online)
      WARNING("strategy account:{} offline", ch->account_str);
    ch->online = online;
  }
  if (snapshot_due_ != Timestamp() && now >= snapshot_due_) {
    snapshot_due_ = Timestamp();
    send_snapshots(now);
  }
}

// ---------------------------------------------------------------------------
// 下单通道
// ---------------------------------------------------------------------------

void TsCase::init_order_channels() {
  // 每个 symbol 预生成 oms 交易 symbol 名, 格式与
  // uc-mm/strat/UCTraderMaker_1.cpp 一致: "btc-usdt" (quote-base 小写)
  for (size_t cid = 0; cid < uni_.num_symbols(); cid++) {
    const auto *rule = uni_.symbol_rule(cid);
    std::string name = enums::Asset::Enum_Name(rule->quote) + "-" +
                       enums::Asset::Enum_Name(rule->base);
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    oms_symbol_names_.push_back(name);
    INFO("sid:{} cid:{} {} oms symbol:{}", rule->sid, cid, rule->symbol_name,
         name);
  }

  validate_universe_in_cfg();

  const auto &venues =
      utils::ConfigHelper::getInstance()->getSetting("prod.venues");
  for (int i = 0; i < venues.getLength(); i++) {
    const auto &venue = venues[i];
    const int vendor = venue["vendor"];
    const int market = venue["market"];
    // pandora RestrictType: 0=NORMAL(不限制) 1=MD(禁行情自动订阅)
    // 2=OMS(禁交易) 3=MD_OMS(全禁)。只有可交易的 venue(缺失/0/1)才建
    // order channel; 2/3 禁了交易, 建了也发不出单。
    int restrict_type = 0;
    venue.lookupValue("restrict_type", restrict_type);
    if (restrict_type >= 2) {
      INFO("venue vendor:{} market:{} restrict_type:{} (OMS restricted) -> "
           "no order channel",
           vendor, market, restrict_type);
      continue;
    }
    const auto &accounts = venue["accounts"];
    for (int j = 0; j < accounts.getLength(); j++) {
      const char *acc_c = accounts[j]["account_id"];
      const std::string acc(acc_c);
      if (acc.empty())
        TW("empty account_id in prod.venues vendor:{} market:{}", vendor,
           market);
      // 通道名 = account_id 原文, 全局必须唯一
      if (order_channels_.count(acc))
        TW("duplicated account_id:{} in prod.venues", acc);
      auto *ch = new OrderChannel(this, acc);
      ch->oms_header.vendor = (oms::Vendor)vendor;
      ch->oms_header.market = (oms::Market)market;
      string_to_char_array(acc, ch->oms_header.account_id);
      ch->src_reader.connect();
      ch->rsp_writer.connect();
      order_channels_[acc] = ch;
      INFO("order channel account:{} /order_src_{} /order_rsp_{} vendor:{} "
           "market:{}",
           acc, acc, acc, vendor, market);
    }
  }
  if (order_channels_.empty())
    WARNING("no tradable venue (restrict_type>=2) in prod.venues, order "
            "channel disabled");
}

// universe 里的每个 symbol 在 prod.venues 必须有对应 vendor+market 的 venue,
// 否则 md agent 无会话可用, 直接 TW 拒绝启动。
// (md 订阅本身已改为 subscribe_md_from_universe 按 universe 显式发起,
//  不再校验 cfg 的 prod.modules.md.subscribe.symbols 列表。)
void TsCase::validate_universe_in_cfg() {
  auto *helper = utils::ConfigHelper::getInstance();
  const auto &venues = helper->getSetting("prod.venues");
  for (size_t cid = 0; cid < uni_.num_symbols(); cid++) {
    const auto *rule = uni_.symbol_rule(cid);
    const auto [want_vendor_e, want_market_e] = md_venue(rule);
    const int want_vendor = (int)want_vendor_e;
    const int want_market = (int)want_market_e;
    bool venue_found = false;
    for (int i = 0; i < venues.getLength() && !venue_found; i++) {
      const int vendor = venues[i]["vendor"];
      const int market = venues[i]["market"];
      venue_found = (vendor == want_vendor && market == want_market);
    }
    if (!venue_found)
      TW("symbol:{} no venue vendor:{} market:{} in prod.venues",
         rule->symbol_name, want_vendor, want_market);
    INFO("universe symbol:{} validated (vendor:{} market:{} md:{})",
         rule->symbol_name, want_vendor, want_market, oms_symbol_names_[cid]);
  }
}

void TsCase::on_order_msg(OrderChannel &ch, const void *data) {
  const shm::Header *header = (const shm::Header *)data;
  const char *body = (const char *)data + sizeof(shm::Header);
  auto &oms = getOrderManager(ch.oms_header.vendor, ch.oms_header.market);
  switch (header->type) {
  case enums::EventType::NEW_ORDER: {
    const shm::NewOrder *req = (const shm::NewOrder *)body;
    const auto *rule = uni_.symbol_rule_from_sid(req->sid);
    // sid 不在 universe 内, 或 symbol 合约类型与通道 venue 的 market 不符
    // (PERP<->LINEAR / SPOT<->SPOT) 都直接拒单
    if (rule == NULL ||
        (ch.oms_header.market == oms::Market::LINEAR
             ? rule->er.contract_type != enums::ContractType::PERP
             : rule->er.contract_type != enums::ContractType::SPOT)) {
      ERROR("account:{} bad sid:{} ({}) oid:{}", ch.account_str, req->sid,
            rule ? rule->symbol_name : "unknown", req->oid);
      shm::OrderReject rej = {req->oid, 0};
      send_rsp(ch.account_str, enums::EventType::ORDER_REJECT, &rej,
               sizeof(rej));
      break;
    }
    oms::NewOrder oms_order{};
    oms_order.client_order_id = req->oid;
    string_to_char_array(oms_symbol_names_[rule->cid], oms_order.symbol);
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
  case enums::EventType::CANCEL_ORDER: {
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
  shm::Ack ack = {msg.client_order_id, id_to_u64(msg.order_id), rsp_us(header)};
  send_rsp(account_str(header), enums::EventType::ACK, &ack, sizeof(ack));
}

void TsCase::onOrderCanceled(const oms::ResponseHeader &header,
                             const oms::OrderUpdate &msg) {
  shm::Canceled cxl = {msg.client_order_id, rsp_us(header)};
  send_rsp(account_str(header), enums::EventType::CANCELED, &cxl,
           sizeof(cxl));
}

void TsCase::onOrderExpired(const oms::ResponseHeader &header,
                            const oms::OrderUpdate &msg) {
  // 与 uc-mm 一致: EXPIRED 按 CANCELED 处理
  shm::Canceled cxl = {msg.client_order_id, rsp_us(header)};
  send_rsp(account_str(header), enums::EventType::CANCELED, &cxl,
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
  fill.side = (uint8_t)msg.side; // oms::Side 与 enums::Side 同值(BUY=0,SELL=1)
  fill.is_maker = (msg.liq == oms::Liquidity::MAKER);
  fill.update_time = rsp_us(header);
  send_rsp(account_str(header), enums::EventType::FILL, &fill, sizeof(fill));
}

void TsCase::onOrderRejected(const oms::ResponseHeader &header,
                             const oms::ErrorMsg &msg) {
  // 错误码映射与 uc-mm/strat/oms_test_case.cpp 一致
  shm::OrderReject rej = {msg.client_order_id, 0, rsp_us(header)};
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
  send_rsp(account_str(header), enums::EventType::ORDER_REJECT, &rej,
           sizeof(rej));
}

void TsCase::onCancelRejected(const oms::ResponseHeader &header,
                              const oms::ErrorMsg &msg) {
  shm::CancelReject rej = {msg.client_order_id, 0, rsp_us(header)};
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
  send_rsp(account_str(header), enums::EventType::CANCEL_REJECT, &rej,
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
