#pragma once


#include "common.h"
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <iostream>
#include "myboost.h"
#include "secrets_local.h"

class Portfolio {

public:

	static Portfolio& getInstance(const std::string& iName = "Default", const double& iCash = 100000.0, const std::string& iHost = "paper-api.alpaca.markets", const std::string& iPort = "443") {
		static Portfolio instance(iName, iCash, iHost, iPort);
		return instance;
	}

	asio::awaitable<void> poll_account_forever();

	awaitable<nlohmann::json> alpaca_post_order( const nlohmann::json& order, ssl::context& tls_ctx);

	std::string getName() const { return mName; }

	double getCash() const { return mCash; }


private:

	Portfolio(const std::string& iName, const double& iCash, const std::string& iHost, const std::string& iPort);

	awaitable<nlohmann::json> alpaca_get_account( ssl::context& tls_ctx);
	
    
	~Portfolio() = default;
    Portfolio(const Portfolio&) = delete;
    Portfolio& operator=(const Portfolio&) = delete;

	std::string mName;
	double mCash;
	std::string mHost;
	std::string mPort;

};