#pragma once
// TsCase: trader-server 的行情通道。继承 pandora 的 strat::StrategyBase，
// 只做一件事：把 MD 回调(onTrade/onOrderbook/onBookTicker)翻译成
// hft/libnst 的 shm 消息(ShmMessage.h)，按 symbol 广播到 /md_{symbol_name}
// (ShmBCast.h)。协议与 hft/md/main.cpp 的 QuoteServer 完全一致，hft 侧
// ShmMDSession 可直接读取。live-only：无回测分支。
//
// 下单通道: cfg(prod.venues[].accounts[].account_id) 里的数字账户各建一对
// shm 通道 —— /order_src_{N}(reader, 策略下单/撤单) + /order_rsp_{N}(writer,
// 回报)。src 收到 shm::NewOrder/CancelOrder 后经 pandora OMS 执行; OMS 回调
// (onOrderAcked/Canceled/Expired/Filled/Rejected/CancelRejected) 按
// ResponseHeader.account_id 路由回对应 rsp 通道(通道不存在则丢弃)。
// 错误码映射与 uc-mm/strat/oms_test_case.cpp 保持一致。
#include "oms/order_manager.h"
#include "strat/strategy_base.h"
#include <common/ConfigFileParser.h>
#include <common/LiveTimer.h>
#include <common/MarketDataMessage.h>
#include <common/ShmBCast.h>
#include <common/ShmMessage.h>
#include <common/Typedef.h>
#include <common/Universe.h>
#include <map>
#include <string>
#include <vector>

class TsCase : public strat::StrategyBase {
  using this_type = TsCase;

public:
  TsCase() : uni_(true /*live only*/, Timestamp::now().to_ndate_str()) {}
  ~TsCase() = default;
  void init(const ConfigFileParser &parser);

  // Redis 回调：不使用
  virtual void onRedisSubscribe(const std::string &) override {}
  virtual void onRedisUnsubscribe(const std::string &) override {}
  virtual void onRedisPatternSubscribe(const std::string &) override {}
  virtual void onRedisPatternUnsubscribe(const std::string &) override {}
  virtual void onRedisSubscribeMessage(const std::string &,
                                       const std::string &) override {}
  virtual void onRedisPublish(const std::string &,
                              const std::string &) override {}
  virtual void onRedisDisconnect() override {}
  virtual void onRedisClose() override {}

  // OMS 回调：翻译为 shm 回报, 按 account_id 路由到 /order_rsp_{N}
  virtual void onOrderAcked(const oms::ResponseHeader &header,
                            const oms::OrderUpdate &msg) override;
  virtual void onOrderCanceled(const oms::ResponseHeader &header,
                               const oms::OrderUpdate &msg) override;
  virtual void onOrderExpired(const oms::ResponseHeader &header,
                              const oms::OrderUpdate &msg) override;
  virtual void onOrderFilled(const oms::ResponseHeader &header,
                             const oms::OrderUpdate &msg) override;
  virtual void onOrderRejected(const oms::ResponseHeader &header,
                               const oms::ErrorMsg &msg) override;
  virtual void onCancelRejected(const oms::ResponseHeader &header,
                                const oms::ErrorMsg &msg) override;
  virtual void onUnifiedErrorResp(const oms::ResponseHeader &header,
                                  const oms::ErrorMsg &msg) override;
  virtual void onDataStreamSubscribe(const oms::ResponseHeader &) override {}
  virtual void onSubscribeDisconnected(const oms::ResponseHeader &,
                                       const oms::ErrorMsg &) override {}

  // MD 回调：翻译并广播
  virtual void onTrade(const MD::Trade &trade) override;
  virtual void onOrderbook(const MD::Orderbook &orderbook) override;
  virtual void onBookTicker(const MD::BookTicker &book_ticker) override;

  virtual void onMdLoaderEnd() override;
  virtual void onDerivedEvent() override {
    live_timer_.poll();
    for (auto &[id, ch] : order_channels_)
      ch->src_reader.poll();
  }
  void on_timer(const Timestamp now);

  // 订单请求通道 ShmReader 的回调载体(每账户一个)
  struct OrderChannel;
  void on_order_msg(OrderChannel &ch, const void *data);

private:
  const std::string translate_symbol_name(Exchange::Vendor vendor,
                                          Exchange::Market market,
                                          const Exchange::Symbol &symbol);
  void publish(const MarketDataMessage &msg);

  void init_order_channels();
  void send_rsp(const std::string &account_id, uint8_t type, const void *body,
                size_t body_len);

  Universe uni_;
  LiveTimer<this_type> live_timer_;
  MarketDataMessage msg_;
  std::vector<ShmWriter *> writers_;
  char send_buf_[SHM_ALIGN_SIZE] = {0};

public:
  // ---- 下单通道: 每账户 src(reader, 策略->ucts) + rsp(writer, ucts->策略) ----
  struct OrderChannel {
    struct SrcProc { // ShmReader 需要 on_read(proc) 形状的处理器
      TsCase *ts;
      OrderChannel *ch;
      void on_read(const void *data) { ts->on_order_msg(*ch, data); }
    };
    std::string account_str;   // cfg 里的原始 account_id (如 "001"), 供 oms 头用
    uint64_t account_num;      // stoull 后的数字, 决定通道名
    SrcProc proc;
    ShmReader<SrcProc> src_reader;
    ShmWriter rsp_writer;
    oms::RequestHeader oms_header = {};
    uint64_t rsp_seqnum = 0;
    OrderChannel(TsCase *ts, const std::string &acc_str, uint64_t acc_num)
        : account_str(acc_str), account_num(acc_num), proc{ts, this},
          src_reader("/order_src_" + std::to_string(acc_num), &proc,
                     SHM_ORDER_SRC_CAPACITY),
          rsp_writer("/order_rsp_" + std::to_string(acc_num),
                     SHM_ORDER_RSP_CAPACITY) {}
  };

private:
  std::map<std::string, OrderChannel *> order_channels_; // key = account_str
  std::vector<std::string> oms_symbol_names_; // 按 cid, "btc-usdt" 形式
  char rsp_buf_[SHM_ALIGN_SIZE] = {0};
};
