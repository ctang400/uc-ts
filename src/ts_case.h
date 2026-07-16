#pragma once
// TsCase: trader-server 的行情通道。继承 pandora 的 strat::StrategyBase，
// 只做一件事：把 MD 回调(onTrade/onOrderbook/onBookTicker)翻译成
// hft/libnst 的 shm 消息(ShmMessage.h)，按 symbol 广播到 /md_{symbol_name}
// (ShmBCast.h)。协议与 hft/md/main.cpp 的 QuoteServer 完全一致，hft 侧
// ShmMDSession 可直接读取。live-only：无回测分支、无策略/OMS 逻辑
// (OMS/Redis 回调全部空实现；下单通道后续单独实现)。
#include "strat/strategy_base.h"
#include <common/ConfigFileParser.h>
#include <common/LiveTimer.h>
#include <common/MarketDataMessage.h>
#include <common/ShmBCast.h>
#include <common/ShmMessage.h>
#include <common/Typedef.h>
#include <common/Universe.h>
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

  // OMS 回调：数据通道阶段不使用（下单通道后续实现）
  virtual void onOrderAcked(const oms::ResponseHeader &,
                            const oms::OrderUpdate &) override {}
  virtual void onOrderCanceled(const oms::ResponseHeader &,
                               const oms::OrderUpdate &) override {}
  virtual void onOrderExpired(const oms::ResponseHeader &,
                              const oms::OrderUpdate &) override {}
  virtual void onOrderFilled(const oms::ResponseHeader &,
                             const oms::OrderUpdate &) override {}
  virtual void onOrderRejected(const oms::ResponseHeader &,
                               const oms::ErrorMsg &) override {}
  virtual void onCancelRejected(const oms::ResponseHeader &,
                                const oms::ErrorMsg &) override {}
  virtual void onUnifiedErrorResp(const oms::ResponseHeader &,
                                  const oms::ErrorMsg &) override {}
  virtual void onDataStreamSubscribe(const oms::ResponseHeader &) override {}
  virtual void onSubscribeDisconnected(const oms::ResponseHeader &,
                                       const oms::ErrorMsg &) override {}

  // MD 回调：翻译并广播
  virtual void onTrade(const MD::Trade &trade) override;
  virtual void onOrderbook(const MD::Orderbook &orderbook) override;
  virtual void onBookTicker(const MD::BookTicker &book_ticker) override;

  virtual void onMdLoaderEnd() override;
  virtual void onDerivedEvent() override { live_timer_.poll(); }
  void on_timer(const Timestamp now);

private:
  const std::string translate_symbol_name(Exchange::Vendor vendor,
                                          Exchange::Market market,
                                          const Exchange::Symbol &symbol);
  void publish(const MarketDataMessage &msg);

  Universe uni_;
  LiveTimer<this_type> live_timer_;
  MarketDataMessage msg_;
  std::vector<ShmWriter *> writers_;
  char send_buf_[SHM_ALIGN_SIZE] = {0};
};
