#include "common.h"
#include <nlohmann/json.hpp>

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
