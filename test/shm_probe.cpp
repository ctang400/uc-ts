// 独立 shm 读端探针：不依赖 libnst，按 ShmBCast/ShmMessage 的内存布局裸读，
// 用于本地验证 ucts 广播的数据（消息率/类型分布/价格合理性/丢包）。
// 用法: ./shm_probe md_BINANCE_PERP_BTC_USDT [seconds]
// 编译: g++ -O2 -std=c++20 -o shm_probe shm_probe.cpp -lrt
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

static constexpr size_t SHM_ALIGN = 64; // == libnst SHM_ALIGN_SIZE

struct Header {
  uint64_t type : 4;
  uint64_t seqnum : 60;
} __attribute__((__packed__));
struct Quote {
  double price;
  double qty;
  int64_t exchange_time;
  uint8_t side : 1;
  uint8_t is_packet_end : 1;
  uint8_t padding : 6;
} __attribute__((__packed__));
struct Layout {
  alignas(64) std::atomic<uint64_t> head;
  alignas(64) std::atomic<uint64_t> heartbeat;
  alignas(64) std::atomic<uint64_t> capacity;
  char data[];
};

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <shm_name e.g. md_BINANCE_PERP_BTC_USDT> [secs]\n", argv[0]);
    return 1;
  }
  const std::string name = argv[1];
  const int secs = argc > 2 ? atoi(argv[2]) : 20;

  int fd = shm_open(name.c_str(), O_RDONLY, 0666);
  if (fd < 0) { perror("shm_open"); return 1; }
  Layout *hdr = (Layout *)mmap(nullptr, sizeof(Layout), PROT_READ, MAP_SHARED, fd, 0);
  const uint64_t cap = hdr->capacity.load();
  munmap(hdr, sizeof(Layout));
  const size_t total = sizeof(Layout) + SHM_ALIGN * cap;
  Layout *shm = (Layout *)mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  printf("connected %s capacity=%lu head=%lu\n", name.c_str(), cap, shm->head.load());

  uint64_t tail = shm->head.load(std::memory_order_acquire);
  uint64_t cnt[16] = {0}, dropped = 0, total_msgs = 0;
  double last_px[16] = {0};
  const auto t_end = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
  while (std::chrono::steady_clock::now() < t_end) {
    uint64_t head = shm->head.load(std::memory_order_acquire);
    if (tail < head) {
      if (head - tail > cap) { dropped += head - tail - 1; tail = head - 1; }
      const char *slot = shm->data + SHM_ALIGN * (tail & (cap - 1));
      const Header *h = (const Header *)slot;
      const Quote *q = (const Quote *)(slot + sizeof(Header));
      if (h->type < 16) {
        cnt[h->type]++;
        if (h->type <= 4) last_px[h->type] = q->price;
      }
      total_msgs++;
      tail++;
    }
  }
  // enums::EventType: 顺序与 libnst 一致(仅用于展示)
  const char *names[] = {"HEARTBEAT", "TRADE", "BBO", "DIFF", "SNAPSHOT", "CLEAR"};
  printf("total msgs in %ds: %lu, dropped(slow-reader): %lu\n", secs, total_msgs, dropped);
  for (int i = 0; i < 6; i++)
    if (cnt[i]) printf("  type %-9s cnt=%-8lu last_px=%.2f\n",
                       i < 6 ? names[i] : "?", cnt[i], last_px[i]);
  const uint64_t hb = shm->heartbeat.load();
  printf("heartbeat age: %.3fs\n",
         (std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count() - (int64_t)hb) / 1e9);
  return 0;
}
