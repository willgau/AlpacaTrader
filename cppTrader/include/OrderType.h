#include "common.h"
#include <nlohmann/json.hpp>
#include "Benchmark.h"

class BuyOrder {
    public:
        BuyOrder(std::string iSymbol, int iQty, std::string iSide = "buy", std::string iType = "market", std::string iTime = "day")
            : mSymbol(iSymbol), mQty(iQty), mSide(iSide), mType(iType), mTime(iTime) { };

        nlohmann::json toJSON()
        {
            nlohmann::json j;
            j["symbol"] = mSymbol;
            j["qty"] = std::to_string(mQty);
            j["side"] = mSide;
            j["type"] = mType;
            j["time_in_force"] = mTime;

            return j;
        }
private:
    
        std::string mSymbol;
        int mQty;
        std::string mSide;
        std::string mType;
        std::string mTime;
};

class SellOrder {
    public:
        SellOrder(std::string iSymbol, int iQty, double iPriceLimit, std::string iSide = "sell", std::string iType = "limit", std::string iTime = "day")
            : mSymbol(iSymbol), mQty(iQty), mPriceLimit(iPriceLimit), mSide(iSide), mType(iType), mTime(iTime) {
        };

        nlohmann::json toJSON()
        {
            nlohmann::json j;
            j["symbol"] = mSymbol;
            j["qty"] = std::to_string(mQty);
            j["side"] = mSide;
            j["type"] = mType;
            j["time_in_force"] = mTime;
            j["limit_price"] = mPriceLimit;

            return j;
        }
    private:

        std::string mSymbol;
        int mQty;
        double mPriceLimit;
        std::string mSide;
        std::string mType;
        std::string mTime; 
};

static inline void copy_symbol(char(&dst)[16], std::string_view src) 
{
    // Copy up to 15 chars and always null-terminate.
    const std::size_t n = std::min(src.size(), sizeof(dst) - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

enum class Action : std::uint8_t { Buy = 1, Sell = 2 };

#pragma pack(push, 1)
struct OrderMsg {
    std::uint64_t ts_qpc;      // producer timestamp (QPC ticks)
    std::uint64_t seq;         // sequence number
    Action action;             // buy/sell
    std::uint8_t _pad[3]{};    // padding
    std::uint32_t qty;         // shares
    char symbol[16];           // null-terminated
};
#pragma pack(pop)

//Create fake order messages for benchmarking
static inline OrderMsg make_msg(std::uint64_t seq, bool buy) {
    OrderMsg m{};
    m.ts_qpc = qpc_now();
    m.seq = seq;
    m.action = buy ? Action::Buy : Action::Sell;
    m.qty = 1 + (std::uint32_t)(seq % 10);
    std::memset(m.symbol, 0, sizeof(m.symbol));
    // alternate symbols to avoid constant folding
    const char* sym = (seq % 2 == 0) ? "AAPL" : "MSFT";
    copy_symbol(m.symbol, sym);
    return m;
}

static inline void print_msg(OrderMsg m) 
{
    std::cout << "OrderMsg: seq=" << m.seq
        << " action=" << ((m.action == Action::Buy) ? "Buy" : "Sell")
        << " qty=" << m.qty
        << " symbol=" << m.symbol
        << "\n";
}